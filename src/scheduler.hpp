#pragma once

#include "storage.hpp"
#include "tracker.hpp"

#include <span>

namespace bt {
void download_pieces(const Metainfo& meta, std::string_view id, std::span<const Endpoint> endpoints,
                     std::uint32_t first, OutputFile& output, Announce& state);
}
