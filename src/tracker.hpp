#pragma once
#include "metainfo.hpp"
#include "net.hpp"
#include <vector>

namespace bt {
struct Announce {
    std::uint16_t port;
    std::uint64_t left;
    std::uint64_t uploaded{};
    std::uint64_t downloaded{};
};
struct TrackerReply {
    unsigned interval{};
    std::vector<Endpoint> peers;
};
std::string escape_query(std::string_view bytes);
TrackerReply parse_tracker_reply(std::string_view data);
TrackerReply announce(const Metainfo& meta, std::string_view id, const Announce& state,
                      std::string_view event = {});
}
