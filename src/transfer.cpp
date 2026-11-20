#include "storage.hpp"
#include "transfer.hpp"

#include <algorithm>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace bt {
namespace {
constexpr std::string_view protocol = "BitTorrent protocol";
enum Message : unsigned char {
    choke, unchoke, interested, not_interested, have, bitfield, request, piece, cancel
};

std::string word(std::uint32_t number) {
    std::string bytes(4, '\0');
    for (int i = 3; i >= 0; --i) {
        bytes[i] = static_cast<char>(number & 255);
        number >>= 8;
    }
    return bytes;
}

std::uint32_t number(std::string_view bytes, std::size_t offset = 0) {
    if (offset > bytes.size() || bytes.size() - offset < 4) {
        throw std::runtime_error("Truncated peer integer");
    }
    std::uint32_t value{};
    for (auto byte : bytes.substr(offset, 4)) value = (value << 8) | static_cast<unsigned char>(byte);
    return value;
}

void send_handshake(const Socket& peer, const Metainfo& meta, std::string_view id) {
    peer.send_all(std::string(1, static_cast<char>(protocol.size())) + std::string(protocol) +
                  std::string(8, '\0') + meta.info_hash + std::string(id));
}

void expect_handshake(const Socket& peer, const Metainfo& meta, std::string_view id) {
    const auto handshake = peer.receive(68);
    if (static_cast<unsigned char>(handshake[0]) != protocol.size() ||
        handshake.substr(1, protocol.size()) != protocol || handshake.substr(28, 20) != meta.info_hash ||
        handshake.substr(48) == id) {
        throw std::runtime_error("Invalid handshake, wrong torrent, or self-connection");
    }
}

void send(const Socket& peer, Message type, std::string_view payload = {}) {
    peer.send_all(word(static_cast<std::uint32_t>(1 + payload.size())) +
                  static_cast<char>(type) + std::string(payload));
}

std::optional<std::string> next(const Socket& peer, const Metainfo& meta) {
    std::string prefix(4, '\0');
    if (!peer.receive_into(prefix, true)) return std::nullopt;
    const auto length = number(prefix);
    if (length > std::max(9 + block_size, 1 + (meta.count() + 7) / 8)) {
        throw std::runtime_error("Oversized peer message");
    }
    return peer.receive(length);
}

unsigned char mask(std::uint32_t index) { return static_cast<unsigned char>(0x80 >> (index % 8)); }

void upload(const Socket& peer, const Metainfo& meta, std::string_view id, std::istream& file,
            std::atomic_uint64_t& uploaded, const std::atomic_bool& stopped) {
    expect_handshake(peer, meta, id);
    send_handshake(peer, meta, id);
    std::string availability((meta.count() + 7) / 8, '\0');
    for (std::uint32_t i = 0; i < meta.count(); ++i) availability[i / 8] |= mask(i);
    send(peer, bitfield, availability);

    bool ready = false;
    while (!stopped) {
        const auto message = next(peer, meta);
        if (!message) return;
        if (message->empty()) continue;
        const auto type = static_cast<unsigned char>(message->front());
        if (type == interested && message->size() == 1 && !ready) {
            send(peer, unchoke);
            ready = true;
        } else if (type == not_interested && message->size() == 1 && ready) {
            send(peer, choke);
            ready = false;
        } else if (type == request) {
            if (message->size() != 13 || !ready) throw std::runtime_error("Unexpected block request");
            const auto index = number(*message, 1);
            const auto begin = number(*message, 5);
            const auto length = number(*message, 9);
            const auto size = meta.size(index);
            if (!length || length > block_size || begin >= size || length > size - begin) {
                throw std::runtime_error("Block request outside piece");
            }
            file.clear();
            file.seekg(static_cast<std::streamoff>(meta.offset(index) + begin));
            send(peer, piece, word(index) + word(begin) + read_exact(file, length));
            uploaded += length;
        }
    }
}

class Downloader {
    const Socket& peer;
    const Metainfo& meta;
    std::string availability;
    bool choked = true;
    bool saw_bitfield = false;

    std::optional<std::string> receive() {
        auto msg = next(peer, meta);
        if (!msg) throw std::runtime_error("Peer disconnected");
        if (msg->empty()) return std::nullopt;
        const auto type = static_cast<unsigned char>(msg->front());
        if (type == bitfield) {
            if (saw_bitfield || msg->size() != 1 + availability.size()) {
                throw std::runtime_error("Invalid peer bitfield");
            }
            availability = msg->substr(1);
            saw_bitfield = true;
            for (auto i = meta.count(); i % 8; ++i) {
                if (availability.back() & mask(i)) throw std::runtime_error("Invalid bitfield padding");
            }
        } else if (type == have) {
            if (msg->size() != 5) throw std::runtime_error("Invalid have message");
            const auto index = number(*msg, 1);
            if (index >= meta.count()) throw std::runtime_error("Invalid have index");
            availability[index / 8] |= mask(index);
        } else if (type == choke || type == unchoke) {
            if (msg->size() != 1) throw std::runtime_error("Invalid choke message");
            choked = type == choke;
        } else if (type == piece) {
            return msg;
        }
        return std::nullopt;
    }

    void wait_until_ready(std::uint32_t index) {
        for (int frames = 0; frames < 256; ++frames) {
            if (!choked && (availability[index / 8] & mask(index))) return;
            if (receive()) throw std::runtime_error("Unsolicited piece");
        }
        throw std::runtime_error("Peer did not offer the requested piece");
    }

    std::string block(std::uint32_t index, std::uint32_t begin, std::uint32_t length) {
        wait_until_ready(index);
        send(peer, request, word(index) + word(begin) + word(length));
        for (int frames = 0; frames < 256; ++frames) {
            const auto reply = receive();
            if (choked) throw std::runtime_error("Peer choked an outstanding request");
            if (!reply) continue;
            if (reply->size() != 9 + length || number(*reply, 1) != index || number(*reply, 5) != begin) {
                throw std::runtime_error("Block does not match the request");
            }
            return reply->substr(9);
        }
        throw std::runtime_error("Peer did not send the requested block");
    }

public:
    Downloader(const Socket& socket, const Metainfo& torrent, std::string_view id)
        : peer(socket), meta(torrent), availability((meta.count() + 7) / 8, '\0') {
        send_handshake(peer, meta, id);
        expect_handshake(peer, meta, id);
        send(peer, interested);
    }

    std::string get_piece(std::uint32_t index, std::uint64_t& received) {
        const auto size = meta.size(index);
        std::string data;
        data.reserve(size);
        for (std::uint32_t begin = 0; begin < size;) {
            const auto length = std::min(block_size, size - begin);
            data += block(index, begin, length);
            received += length;
            begin += length;
        }
        if (!meta.matches(index, data)) throw std::runtime_error("Downloaded piece failed its hash check");
        return data;
    }
};

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
            upload(peer, meta, id, file, uploaded, stopped);
        } catch (const std::exception& error) {
            std::cerr << "Peer: " << error.what() << '\n';
        }
    }
}

void download(const Metainfo& meta, const std::filesystem::path& directory, const Endpoint* direct) {
    const auto destination = directory / safe_name(meta.name);
    std::filesystem::create_directories(directory);
    OutputFile output(destination);
    const auto id = peer_id();
    Announce state{0, meta.length};
    const auto peers = direct ? std::vector<Endpoint>{*direct} : announce(meta, id, state, "started").peers;
    try {
        std::uint32_t index{};
        for (const auto& endpoint : peers) {
            std::optional<Socket> socket;
            std::optional<Downloader> client;
            try {
                socket.emplace(Socket::connect_to(endpoint));
                client.emplace(*socket, meta, id);
            } catch (const std::exception& error) {
                std::cerr << endpoint.ip << ": " << error.what() << '\n';
                continue;
            }
            while (index < meta.count()) {
                std::string data;
                try {
                    data = client->get_piece(index, state.downloaded);
                } catch (const std::exception& error) {
                    std::cerr << endpoint.ip << ": " << error.what() << '\n';
                    break;
                }
                output.write(data);
                state.left -= data.size();
                ++index;
            }
            if (index == meta.count()) break;
        }
        if (state.left != 0) throw std::runtime_error("Peer list did not provide the complete file");
        output.publish();
        if (!direct) report_announce(meta, id, state, "completed");
    } catch (...) {
        if (!direct) report_announce(meta, id, state, "stopped");
        throw;
    }
    if (!direct) report_announce(meta, id, state, "stopped");
}
}
