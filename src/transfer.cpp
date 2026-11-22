#include "discovery.hpp"
#include "storage.hpp"
#include "transfer.hpp"
#include "peer.hpp"
#include "scheduler.hpp"

#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace bt {
namespace {
void report_announce(const Metainfo& meta, std::string_view id, const Announce& state,
                     std::string_view event) {
    try {
        announce(meta, id, state, event);
    } catch (const std::exception& error) {
        std::cerr << "Tracker " << event << ": " << error.what() << '\n';
    }
}
}

void seed(const Metainfo& meta, const std::filesystem::path& source, std::uint16_t port,
          bool use_tracker, const std::atomic_bool& stopped) {
    std::ifstream file(source, std::ios::binary);
    verify_file(meta, file);
    const auto listener = Socket::listen_on(port);
    const auto id = peer_id();
    std::atomic_uint64_t uploaded{};
    std::jthread tracker;
    std::cout << "Listening on TCP port " << listener.port() << std::endl;
    if (use_tracker) {
        tracker = std::jthread([&](std::stop_token stop) {
            std::mutex mutex;
            std::unique_lock lock(mutex);
            std::condition_variable_any wait;
            std::string_view event = "started";
            while (!stop.stop_requested()) {
                unsigned interval = 30;
                try {
                    interval = announce(meta, id, {listener.port(), 0, uploaded.load()}, event).interval;
                    event = {};
                } catch (const std::exception& error) {
                    std::cerr << "Tracker: " << error.what() << '\n';
                }
                wait.wait_for(lock, stop, std::chrono::seconds(interval), [] { return false; });
            }
            if (event.empty()) report_announce(meta, id, {listener.port(), 0, uploaded.load()}, "stopped");
        });
    }
    while (!stopped) {
        if (!listener.has_peer()) continue;
        try {
            const auto peer = listener.accept_peer();
            upload_to_peer(peer, meta, id, file, uploaded, stopped);
        } catch (const std::exception& error) {
            std::cerr << "Peer: " << error.what() << '\n';
        }
    }
}

void download(const Metainfo& meta, const std::filesystem::path& directory, const Endpoint* direct, bool resume) {
    const auto destination = directory / safe_name(meta.name);
    std::filesystem::create_directories(directory);
    OutputFile output(destination, resume ? std::string_view(meta.info_hash) : std::string_view{});
    const auto index = resume ? output.recheck(meta) : 0;
    const auto verified_bytes = index == meta.count() ? meta.length : meta.offset(index);
    if (verified_bytes != 0) {
        std::cout << "Reused " << index << " verified piece(s) (" << verified_bytes << " bytes)\n";
    }
    if (index == meta.count()) {
        output.publish();
        return;
    }
    const auto id = peer_id();
    Announce state{0, meta.length - verified_bytes};
    std::jthread incoming;
    if (!direct) {
        auto listener = Socket::listen_on(0);
        state.port = listener.port();
        std::cout << "Listening on TCP port " << state.port << std::endl;
        incoming = std::jthread([listener = std::move(listener), &meta, &id](std::stop_token stop) {
            try {
                listen_for_download_peers(listener, meta, id, stop);
            } catch (const std::exception& error) {
                std::cerr << "Incoming peers: " << error.what() << '\n';
            }
        });
    }
    auto reply = direct ? TrackerReply{0, {*direct}} : announce(meta, id, state, "started");
    try {
        if (!direct && reply.peers.empty()) {
            reply.peers = wait_for_tracker_peers(meta, id, state, reply.interval);
        }
        download_pieces(meta, id, reply.peers, index, output, state);
        output.publish();
        if (!direct) report_announce(meta, id, state, "completed");
    } catch (...) {
        if (!direct) report_announce(meta, id, state, "stopped");
        throw;
    }
    if (!direct) report_announce(meta, id, state, "stopped");
}
}
