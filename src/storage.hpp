#pragma once
#include "metainfo.hpp"

namespace bt {
std::string read_exact(std::istream& file, std::size_t size);
void verify_file(const Metainfo& meta, std::istream& file);
void save_new(const std::filesystem::path& path, std::string_view bytes);

class OutputFile {
    std::filesystem::path final_path;
    std::filesystem::path temp_path;
    void* handle{};
    bool published{};
    bool resumable{};
public:
    explicit OutputFile(std::filesystem::path path, std::string_view resume_hash = {});
    ~OutputFile();
    OutputFile(const OutputFile&) = delete;
    OutputFile& operator=(const OutputFile&) = delete;
    std::uint32_t recheck(const Metainfo& meta);
    void write(std::string_view bytes);
    void publish();
};
}
