#pragma once
#include "metainfo.hpp"
#include "windows_file.hpp"

namespace bt {
void save_new(const std::filesystem::path& path, std::string_view bytes);

class OutputFile {
    std::filesystem::path final_path;
    std::span<const TorrentFile> files;
    FileHandle parent;
    FileHandle handle;
    bool published{};
    bool resumable{};
public:
    explicit OutputFile(std::filesystem::path path, std::string_view resume_hash = {},
                        std::span<const TorrentFile> entries = {});
    ~OutputFile();
    OutputFile(const OutputFile&) = delete;
    OutputFile& operator=(const OutputFile&) = delete;
    std::uint32_t recheck(const Metainfo& meta);
    void write(std::string_view bytes);
    void publish();
};
}
