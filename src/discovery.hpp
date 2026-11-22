#pragma once

#include "tracker.hpp"

namespace bt {
std::vector<Endpoint> wait_for_tracker_peers(const Metainfo& meta, std::string_view id,
                                             const Announce& state, unsigned interval);
}
