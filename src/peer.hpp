#pragma once

#include "metainfo.hpp"
#include "net.hpp"

#include <atomic>
#include <optional>

namespace bt {
void listen_for_download_peers(const Socket& listener, const Metainfo& meta, std::string_view id,
                               std::stop_token stop);
void upload_to_peer(const Socket& peer, const Metainfo& meta, std::string_view id, std::istream& file,
                    std::atomic_uint64_t& uploaded, const std::atomic_bool& stopped);

class PeerDownload {
    const Socket& peer;
    const Metainfo& meta;
    std::string availability;
    bool choked = true;
    bool saw_bitfield = false;

    std::optional<std::string> receive();
    void wait_until_ready(std::uint32_t index);
    std::string receive_block();

public:
    PeerDownload(const Socket& socket, const Metainfo& torrent, std::string_view id);
    void prepare();
    bool has_piece(std::uint32_t index) const;
    std::string get_piece(std::uint32_t index, std::uint64_t& received);
};
}
