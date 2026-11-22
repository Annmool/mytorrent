#include "payload.hpp"
#include "bencode.hpp"

#include <algorithm>
#include <stdexcept>

namespace bt {
namespace fs = std::filesystem;

PayloadReader::PayloadReader(const fs::path& path, std::uint64_t size, std::span<const TorrentFile> entries)
    : source(fs::absolute(path).make_preferred()), length(size), files(entries) {
    if (files.empty()) {
        open(0);
    } else {
        if (!source.has_filename() && source.has_relative_path()) source = source.parent_path();
        root = lock_directory(source);
        for (std::size_t index = 0; index < files.size(); ++index) {
            if (!files[index].length) open(index);
        }
    }
}

void PayloadReader::open(std::size_t index) {
    if (current == index) return;
    current = static_cast<std::size_t>(-1);
    file.reset();
    parents.clear();
    auto path = source;
    auto expected = length;
    DWORD flags{};
    if (!files.empty()) {
        const auto& entry = files[index];
        for (const auto& component : entry.path.parent_path()) {
            path /= component;
            parents.push_back(lock_directory(path));
        }
        path /= entry.path.filename();
        expected = entry.length;
        flags = FILE_FLAG_OPEN_REPARSE_POINT;
    }
    auto next = open_handle(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, flags);
    if (GetFileType(next.get()) != FILE_TYPE_DISK ||
        (file_information(next.get()).dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
        throw std::runtime_error("Expected a regular payload file");
    }
    if (file_length(next.get()) != expected) {
        throw std::runtime_error("Payload file length mismatch: " + path_utf8(path));
    }
    file = std::move(next);
    current = index;
}

std::string PayloadReader::read(std::uint64_t offset, std::uint32_t size) {
    if (offset > length || size > length - offset || size > max_piece_size) {
        throw std::runtime_error("Read outside torrent payload");
    }
    std::string data(size, '\0');
    std::span<char> remaining(data);
    if (files.empty()) {
        seek_file(file.get(), offset);
        read_file(file.get(), remaining);
        return data;
    }
    auto entry = std::ranges::upper_bound(files, offset, {},
        [](const TorrentFile& part) { return part.offset + part.length; });
    while (!remaining.empty()) {
        const auto start = offset - entry->offset;
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(entry->length - start, remaining.size()));
        if (count) {
            open(static_cast<std::size_t>(entry - files.begin()));
            seek_file(file.get(), start);
            read_file(file.get(), remaining.first(count));
            remaining = remaining.subspan(count);
            offset += count;
        }
        ++entry;
    }
    return data;
}

void verify_payload(const Metainfo& meta, PayloadReader& payload) {
    for (std::uint32_t index = 0; index < meta.count(); ++index) {
        if (!meta.matches(index, payload.read(meta.offset(index), meta.size(index)))) {
            throw std::runtime_error("Piece hash mismatch at index " + std::to_string(index));
        }
    }
}

std::vector<TorrentFile> collect_folder_files(const fs::path& folder) {
    const auto root = lock_directory(folder);
    std::vector<TorrentFile> files;
    std::size_t path_bytes{};
    std::size_t values = 7; // The outer dictionary, info, and their five values.
    for (auto item = fs::recursive_directory_iterator(folder);
         item != fs::recursive_directory_iterator{}; ++item) {
        const auto& entry = *item;
        const auto attributes = GetFileAttributesW(entry.path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) file_error("Reading source path attributes");
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            throw std::runtime_error("Folder source contains a reparse point");
        }
        const auto depth = static_cast<std::size_t>(item.depth()) + 1;
        if (depth > max_path_components) throw std::runtime_error("Source path is too deep");
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        const auto relative = entry.path().lexically_relative(folder);
        fs::path path;
        for (const auto& component : relative) {
            const auto name = path_utf8(component);
            path_bytes += name.size();
            path /= safe_name(name);
        }
        values += 3 + depth; // A file dictionary, length, path list, and its components.
        if (values > bencode_value_limit || path_bytes > metadata_limit) {
            throw std::runtime_error("Folder file list is too large for torrent metadata");
        }
        files.push_back({std::move(path), entry.file_size()});
    }
    std::ranges::sort(files, {}, &TorrentFile::path);
    return files;
}
}
