#include "windows_file.hpp"

#include <cstddef>
#include <cstring>
#include <new>
#include <stdexcept>
#include <system_error>

namespace bt {
[[noreturn]] void file_error(const char* operation) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
}

FileHandle open_handle(const std::filesystem::path& path, DWORD access, DWORD share,
                       DWORD disposition, DWORD flags) {
    const auto handle = CreateFileW(path.c_str(), access, share, nullptr, disposition, flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE) file_error("Opening file or directory");
    return FileHandle(handle);
}

FileHandle lock_directory(const std::filesystem::path& path, DWORD access) {
    auto directory = open_handle(path, access, FILE_SHARE_READ, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);
    const auto attributes = file_information(directory.get()).dwFileAttributes;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        throw std::runtime_error("Expected a directory without reparse points");
    }
    return directory;
}

BY_HANDLE_FILE_INFORMATION file_information(HANDLE file) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info)) file_error("Reading file attributes");
    return info;
}

std::uint64_t file_length(HANDLE file) {
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file, &length)) file_error("Reading file length");
    return static_cast<std::uint64_t>(length.QuadPart);
}

void seek_file(HANDLE file, std::uint64_t offset) {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) file_error("Seeking in file");
}

void read_file(HANDLE file, std::span<char> bytes) {
    DWORD count{};
    if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr)) {
        file_error("Reading file");
    }
    if (count != bytes.size()) throw std::runtime_error("Could not read expected file bytes");
}

void write_file(HANDLE file, std::string_view bytes) {
    DWORD count{};
    if (!WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr)) {
        file_error("Writing output file");
    }
    if (count != bytes.size()) throw std::runtime_error("Incomplete output write");
}

void flush_file(HANDLE file) {
    if (!FlushFileBuffers(file)) file_error("Flushing output file");
}

void rename_file(HANDLE file, const std::filesystem::path& destination) {
    const auto& name = destination.native();
    const auto name_bytes = name.size() * sizeof(wchar_t);
    const auto buffer_size = sizeof(FILE_RENAME_INFO) + name_bytes;
    auto buffer = std::make_unique_for_overwrite<std::byte[]>(buffer_size);
    auto* rename = new (buffer.get()) FILE_RENAME_INFO{};
    rename->FileNameLength = static_cast<DWORD>(name_bytes);
    std::memcpy(buffer.get() + offsetof(FILE_RENAME_INFO, FileName), name.c_str(), name_bytes + sizeof(wchar_t));
    if (!SetFileInformationByHandle(file, FileRenameInfo, rename, static_cast<DWORD>(buffer_size))) {
        file_error("Publishing output");
    }
}
}
