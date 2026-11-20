#pragma once
#include <cstdint>
#include <filesystem>
#include <istream>
#include <string>
#include <string_view>

namespace bt {
inline constexpr std::uint32_t block_size = 16 * 1024;
inline constexpr std::uint32_t max_piece_size = 16 * 1024 * 1024;
inline constexpr std::size_t digest_size = 20;
struct Metainfo {
    std::string name;
    std::string announce;
    std::uint64_t length{};
    std::uint32_t piece_length{};
    std::string hashes;
    std::string info_hash;

    std::uint32_t count() const;
    std::uint32_t size(std::uint32_t index) const;
    std::uint64_t offset(std::uint32_t index) const;
    bool matches(std::uint32_t index, std::string_view data) const;
};

std::string sha1(std::string_view bytes);
std::string hex(std::string_view bytes);
std::string peer_id();
Metainfo parse_metainfo(std::string_view bytes);
Metainfo load_metainfo(const std::filesystem::path& path);
std::string create_metainfo(const std::filesystem::path& path, std::string announce,
                            std::uint32_t piece_length);
std::filesystem::path safe_name(std::string_view name);
}
