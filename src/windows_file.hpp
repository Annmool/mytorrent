#pragma once

#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

namespace bt {
struct CloseHandleDeleter {
    void operator()(void* handle) const noexcept { CloseHandle(handle); }
};
using FileHandle = std::unique_ptr<void, CloseHandleDeleter>;

[[noreturn]] void file_error(const char* operation);
FileHandle open_handle(const std::filesystem::path& path, DWORD access, DWORD share,
                       DWORD disposition, DWORD flags);
FileHandle lock_directory(const std::filesystem::path& path, DWORD access = FILE_READ_ATTRIBUTES);
BY_HANDLE_FILE_INFORMATION file_information(HANDLE file);
std::uint64_t file_length(HANDLE file);
void seek_file(HANDLE file, std::uint64_t offset);
void read_file(HANDLE file, std::span<char> bytes);
void write_file(HANDLE file, std::string_view bytes);
void flush_file(HANDLE file);
void rename_file(HANDLE file, const std::filesystem::path& destination);
}
