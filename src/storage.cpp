#include "storage.hpp"

#include <windows.h>
#include <cstdio>
#include <stdexcept>
#include <system_error>

namespace bt {
namespace fs = std::filesystem;
namespace {
[[noreturn]] void file_error(const char* message) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), message);
}
}

std::string read_exact(std::istream& file, std::size_t size) {
    std::string data(size, '\0');
    if (!file.read(data.data(), static_cast<std::streamsize>(size))) {
        throw std::runtime_error("Could not read expected file bytes");
    }
    return data;
}

void verify_file(const Metainfo& meta, std::istream& file) {
    for (std::uint32_t index = 0; index < meta.count(); ++index) {
        if (!meta.matches(index, read_exact(file, meta.size(index)))) {
            throw std::runtime_error("Piece hash mismatch at index " + std::to_string(index));
        }
    }
    if (file.peek() != EOF || !file.eof() || file.bad()) throw std::runtime_error("File length or read error");
    file.clear();
    file.seekg(0);
}

OutputFile::OutputFile(fs::path path) : final_path(std::move(path)), temp_path(final_path) {
    if (fs::exists(final_path)) throw std::runtime_error("Output already exists");
    temp_path = final_path.parent_path() / (".mytorrent-" + hex(std::string_view(peer_id()).substr(8)) + ".part");
    handle = CreateFileW(temp_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) file_error("Creating temporary output file");
}

OutputFile::~OutputFile() {
    if (handle) CloseHandle(handle);
    if (!published) DeleteFileW(temp_path.c_str());
}

void OutputFile::write(std::string_view bytes) {
    DWORD written{};
    if (!WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)) {
        file_error("Writing output file");
    }
    if (written != bytes.size()) throw std::runtime_error("Incomplete output write");
}

void OutputFile::publish() {
    if (!FlushFileBuffers(handle)) file_error("Flushing output file");
    if (!CloseHandle(handle)) file_error("Closing output file");
    handle = nullptr;
    if (!MoveFileExW(temp_path.c_str(), final_path.c_str(), MOVEFILE_WRITE_THROUGH)) {
        file_error("Publishing output file");
    }
    published = true;
}

void save_new(const fs::path& path, std::string_view bytes) {
    OutputFile file(path);
    file.write(bytes);
    file.publish();
}
}
