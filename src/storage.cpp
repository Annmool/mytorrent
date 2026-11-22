#include "storage.hpp"

#include <windows.h>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
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

OutputFile::OutputFile(fs::path path, std::string_view resume_hash)
    : final_path(fs::absolute(path).make_preferred()), resumable(!resume_hash.empty()) {
    if (fs::exists(final_path)) throw std::runtime_error("Output already exists");
    const auto identifier = resumable ? hex(resume_hash) : hex(std::string_view(peer_id()).substr(8));
    temp_path = final_path.parent_path() / (".mytorrent-" + identifier + ".part");
    handle = CreateFileW(temp_path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr,
                         resumable ? OPEN_ALWAYS : CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) file_error("Opening partial output file");
    if (resumable) {
        try {
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(handle, &info)) file_error("Checking partial output file");
            if ((info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
                info.nNumberOfLinks != 1 || GetFileType(handle) != FILE_TYPE_DISK) {
                throw std::runtime_error("Expected a regular, unlinked partial output file");
            }
        } catch (...) {
            CloseHandle(handle);
            throw;
        }
    }
}

OutputFile::~OutputFile() {
    if (!published && !resumable) {
        FILE_DISPOSITION_INFO disposition{TRUE};
        SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition));
    }
    CloseHandle(handle);
}

std::uint32_t OutputFile::recheck(const Metainfo& meta) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size)) file_error("Reading partial output size");
    const auto available = static_cast<std::uint64_t>(size.QuadPart);
    std::uint64_t verified{};
    std::uint32_t index{};
    for (; index < meta.count(); ++index) {
        const auto length = meta.size(index);
        if (length > available - verified) break;
        std::string data(length, '\0');
        DWORD read{};
        if (!ReadFile(handle, data.data(), length, &read, nullptr)) file_error("Reading partial piece");
        if (read != length) throw std::runtime_error("Incomplete partial piece read");
        if (!meta.matches(index, data)) break;
        verified += length;
    }
    LARGE_INTEGER offset{};
    offset.QuadPart = static_cast<LONGLONG>(verified);
    if (!SetFilePointerEx(handle, offset, nullptr, FILE_BEGIN)) file_error("Seeking to verified data boundary");
    if (verified != available && !SetEndOfFile(handle)) file_error("Truncating invalid partial data");
    return index;
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
    const auto& destination = final_path.native();
    const auto name_bytes = destination.size() * sizeof(wchar_t);
    const auto buffer_size = sizeof(FILE_RENAME_INFO) + name_bytes;
    auto buffer = std::make_unique_for_overwrite<std::byte[]>(buffer_size);
    auto* rename = new (buffer.get()) FILE_RENAME_INFO{};
    rename->FileNameLength = static_cast<DWORD>(name_bytes);
    std::memcpy(buffer.get() + offsetof(FILE_RENAME_INFO, FileName),
                destination.c_str(), name_bytes + sizeof(wchar_t));
    if (!SetFileInformationByHandle(handle, FileRenameInfo, rename, static_cast<DWORD>(buffer_size))) {
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
