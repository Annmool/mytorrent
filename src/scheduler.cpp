#include "scheduler.hpp"
#include "peer.hpp"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <syncstream>
#include <thread>
#include <utility>

namespace bt {
namespace {
constexpr std::size_t connection_limit = 4;
constexpr std::uint32_t piece_window = 8;

struct Candidate {
    Endpoint endpoint;
    bool failed{};
};

struct Slot {
    bool assigned{};
    std::string verified;
};

class Schedule {
    const Metainfo& meta;
    OutputFile& output;
    Announce& state;
    std::uint32_t first;
    std::uint32_t verified_count{};
    std::array<Slot, piece_window> slots;
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t participants{};
    std::size_t waiting{};
    bool halted{};
    std::exception_ptr failure;

    Slot& slot(std::uint32_t index) { return slots[index % piece_window]; }

    void halt_if_idle() {
        if (waiting == participants) halted = true;
        changed.notify_all();
    }

public:
    Schedule(const Metainfo& torrent, std::uint32_t index, OutputFile& file, Announce& counters)
        : meta(torrent), output(file), state(counters), first(index) {}

    // Called by the coordinator only when no workers are running.
    bool finished() const { return first == meta.count(); }
    std::uint32_t progress() const { return verified_count; }
    void begin_wave(std::size_t count) {
        participants = count;
        halted = false;
    }
    void check_failure() const {
        if (failure) std::rethrow_exception(failure);
    }

    std::optional<std::uint32_t> claim(const PeerDownload& peer) {
        std::unique_lock lock(mutex);
        while (!halted && !finished()) {
            const auto end = std::min(meta.count(), first + piece_window);
            for (auto index = first; index < end; ++index) {
                auto& job = slot(index);
                if (!job.assigned && job.verified.empty() && peer.has_piece(index)) {
                    job.assigned = true;
                    return index;
                }
            }
            // All remaining sessions are idle when no peer can advance this window.
            ++waiting;
            if (waiting == participants) halt_if_idle();
            else changed.wait(lock);
            --waiting;
        }
        return std::nullopt;
    }

    void release(std::uint32_t index) {
        const std::lock_guard lock(mutex);
        slot(index).assigned = false;
        changed.notify_all();
    }

    void complete(std::uint32_t index, std::string bytes) {
        const std::lock_guard lock(mutex);
        auto& job = slot(index);
        job.assigned = false;
        job.verified = std::move(bytes);
        ++verified_count;
        try {
            // Resume can reuse a contiguous prefix even if replies finished out of order.
            while (!halted && !finished() && !slot(first).verified.empty()) {
                auto& next = slot(first);
                output.write(next.verified);
                state.left -= next.verified.size();
                next = Slot{};
                ++first;
            }
        } catch (...) {
            failure = std::current_exception();
            halted = true;
        }
        changed.notify_all();
    }

    void leave(std::uint64_t received) {
        const std::lock_guard lock(mutex);
        state.downloaded += received;
        --participants;
        halt_if_idle();
    }

    void stop() {
        const std::lock_guard lock(mutex);
        halted = true;
        changed.notify_all();
    }
};

void run_peer(Schedule& schedule, const Metainfo& meta, std::string_view id, Candidate& candidate) {
    const auto& endpoint = candidate.endpoint;
    std::uint64_t received{};
    try {
        const auto socket = Socket::connect_to(endpoint);
        PeerDownload peer(socket, meta, id);
        peer.prepare();
        while (const auto index = schedule.claim(peer)) {
            std::string bytes;
            try {
                bytes = peer.get_piece(*index, received);
            } catch (...) {
                schedule.release(*index);
                throw;
            }
            schedule.complete(*index, std::move(bytes));
        }
    } catch (const std::exception& error) {
        candidate.failed = true;
        std::osyncstream(std::cerr) << endpoint.ip << ':' << endpoint.port << ": " << error.what() << '\n';
    }
    schedule.leave(received);
}

void run_wave(Schedule& schedule, const Metainfo& meta, std::string_view id,
              std::span<Candidate> endpoints) {
    std::vector<std::jthread> workers;
    workers.reserve(endpoints.size());
    schedule.begin_wave(endpoints.size());
    try {
        for (auto& endpoint : endpoints) {
            workers.emplace_back(run_peer, std::ref(schedule), std::cref(meta), id, std::ref(endpoint));
        }
    } catch (...) {
        schedule.stop();
        throw;
    }
}
}

void download_pieces(const Metainfo& meta, std::string_view id, std::span<const Endpoint> endpoints,
                     std::uint32_t first, OutputFile& output, Announce& state) {
    std::vector<Candidate> peers;
    std::set<std::pair<std::string, std::uint16_t>> seen;
    for (const auto& endpoint : endpoints) {
        if (seen.emplace(endpoint.ip, endpoint.port).second) peers.push_back({endpoint});
    }

    Schedule schedule(meta, first, output, state);
    while (!schedule.finished()) {
        const auto previous = schedule.progress();
        for (std::size_t i = 0; i < peers.size() && !schedule.finished(); i += connection_limit) {
            const auto wave = std::span(peers).subspan(i, std::min(connection_limit, peers.size() - i));
            run_wave(schedule, meta, id, wave);
            schedule.check_failure();
        }
        std::erase_if(peers, [](const Candidate& peer) { return peer.failed; });
        if (!schedule.finished() && (peers.empty() || schedule.progress() == previous)) {
            throw std::runtime_error("Peer list did not provide the complete file");
        }
    }
}
}
