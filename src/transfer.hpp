#pragma once
#include "tracker.hpp"
#include <atomic>

namespace bt {
void seed(const Metainfo& meta, const std::filesystem::path& source, std::uint16_t port,
          bool use_tracker, const std::atomic_bool& stopped);
void download(const Metainfo& meta, const std::filesystem::path& directory, const Endpoint* direct, bool resume);
}
