#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace bt {
inline constexpr std::size_t max_path_components = 64;

struct TorrentFile {
    std::filesystem::path path;
    std::uint64_t length{};
    std::uint64_t offset{};
};

std::string path_utf8(const std::filesystem::path& path);
std::uint64_t assign_file_offsets(std::vector<TorrentFile>& files);
}
