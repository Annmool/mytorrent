#include "peer.hpp"
#include "payload.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

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

void send_handshake(const Socket& peer, const Metainfo& meta, std::string_view id,
                    std::stop_token stop = {}) {
    peer.send_all(std::string(1, static_cast<char>(protocol.size())) + std::string(protocol) +
                  std::string(8, '\0') + meta.info_hash + std::string(id), stop);
}

void expect_handshake(const Socket& peer, const Metainfo& meta, std::string_view id,
                      std::stop_token stop = {}) {
    const auto handshake = peer.receive(68, stop);
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

std::optional<std::string> next(const Socket& peer, const Metainfo& meta, std::stop_token stop = {}) {
    std::string prefix(4, '\0');
    if (!peer.receive_into(prefix, true, stop)) return std::nullopt;
    const auto length = number(prefix);
    if (length > std::max(9 + block_size, 1 + (meta.count() + 7) / 8)) {
        throw std::runtime_error("Oversized peer message");
    }
    return peer.receive(length, stop);
}

unsigned char mask(std::uint32_t index) { return static_cast<unsigned char>(0x80 >> (index % 8)); }

}

void listen_for_download_peers(const Socket& listener, const Metainfo& meta, std::string_view id,
                               std::stop_token stop) {
    while (!stop.stop_requested()) {
        if (!listener.has_peer() || stop.stop_requested()) continue;
        try {
            const auto peer = listener.accept_peer();
            expect_handshake(peer, meta, id, stop);
            send_handshake(peer, meta, id, stop);
            // Connections start choked. The downloader does not offer pieces to incoming peers.
            while (!stop.stop_requested() && next(peer, meta, stop)) {}
        } catch (const std::runtime_error&) {
            // A disconnected, idle, or malformed peer does not affect the download.
        }
    }
}

void upload_to_peer(const Socket& peer, const Metainfo& meta, std::string_view id, PayloadReader& payload,
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
            send(peer, piece, word(index) + word(begin) + payload.read(meta.offset(index) + begin, length));
            uploaded += length;
        }
    }
}

std::optional<std::string> PeerDownload::receive() {
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

void PeerDownload::wait_until_ready(std::uint32_t index) {
    for (int frames = 0; frames < 256; ++frames) {
        if (!choked && has_piece(index)) return;
        if (receive()) throw std::runtime_error("Unsolicited piece");
    }
    throw std::runtime_error("Peer did not offer the requested piece");
}

std::string PeerDownload::receive_block() {
    for (int frames = 0; frames < 256; ++frames) {
        auto reply = receive();
        if (choked) throw std::runtime_error("Peer choked an outstanding request");
        if (reply) return std::move(reply).value();
    }
    throw std::runtime_error("Peer did not send the requested block");
}

PeerDownload::PeerDownload(const Socket& socket, const Metainfo& torrent, std::string_view id)
    : peer(socket), meta(torrent), availability((meta.count() + 7) / 8, '\0') {
    send_handshake(peer, meta, id);
    expect_handshake(peer, meta, id);
    send(peer, interested);
}

void PeerDownload::prepare() {
    for (int frames = 0; frames < 256; ++frames) {
        const bool has_data = std::ranges::any_of(availability, [](char bits) { return bits != 0; });
        if (!choked && has_data) return;
        if (receive()) throw std::runtime_error("Unsolicited piece");
    }
    throw std::runtime_error("Peer did not advertise pieces and unchoke");
}

bool PeerDownload::has_piece(std::uint32_t index) const {
    return (availability[index / 8] & mask(index)) != 0;
}

std::string PeerDownload::get_piece(std::uint32_t index, std::uint64_t& received) {
    const auto size = meta.size(index);
    constexpr unsigned request_window = 8;
    std::string data(size, '\0');
    std::vector<bool> complete((size + block_size - 1) / block_size);
    std::uint32_t next_offset{};
    unsigned pending{};

    wait_until_ready(index);
    while (pending != 0 || next_offset < size) {
        while (pending < request_window && next_offset < size) {
            const auto length = std::min(block_size, size - next_offset);
            send(peer, request, word(index) + word(next_offset) + word(length));
            next_offset += length;
            ++pending;
        }

        const auto reply = receive_block();
        const auto begin = number(reply, 5);
        if (number(reply, 1) != index || begin >= next_offset || begin % block_size != 0) {
            throw std::runtime_error("Received an unrequested block");
        }
        const auto slot = begin / block_size;
        const auto length = std::min(block_size, size - begin);
        if (reply.size() != 9 + length || complete[slot]) {
            throw std::runtime_error("Received a duplicate block or incorrect block length");
        }
        std::copy_n(reply.data() + 9, length, data.data() + begin);
        complete[slot] = true;
        received += length;
        --pending;
    }
    if (!meta.matches(index, data)) throw std::runtime_error("Downloaded piece failed its hash check");
    return data;
}
}
