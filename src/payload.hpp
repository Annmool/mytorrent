#pragma once

#include "metainfo.hpp"
#include "windows_file.hpp"

namespace bt {
class PayloadReader {
    std::filesystem::path source;
    std::uint64_t length;
    std::span<const TorrentFile> files;
    FileHandle root;
    std::vector<FileHandle> parents;
    FileHandle file;
    std::size_t current = static_cast<std::size_t>(-1);

    void open(std::size_t index);

public:
    PayloadReader(const std::filesystem::path& path, std::uint64_t size,
                  std::span<const TorrentFile> entries);
    std::string read(std::uint64_t offset, std::uint32_t size);
};

void verify_payload(const Metainfo& meta, PayloadReader& payload);
std::vector<TorrentFile> collect_folder_files(const std::filesystem::path& folder);
}
