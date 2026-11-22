#include "folder_output.hpp"
#include "metainfo.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace bt {
namespace fs = std::filesystem;
namespace {
void create_staged_directory(const fs::path& path) {
    if (!CreateDirectoryW(path.c_str(), nullptr)) file_error("Creating staged directory");
}

class StagedFolder {
    fs::path folder;
    FileHandle handle;
    bool published{};

public:
    explicit StagedFolder(const fs::path& parent)
        : folder(parent / (".mytorrent-" + hex(std::string_view(peer_id()).substr(8)) + ".stage")) {
        create_staged_directory(folder);
        try {
            handle = lock_directory(folder, FILE_READ_ATTRIBUTES | DELETE);
        } catch (...) {
            // The directory is still empty if opening it fails.
            RemoveDirectoryW(folder.c_str());
            throw;
        }
    }

    ~StagedFolder() {
        if (!published) {
            handle.reset();
            std::error_code ignored;
            fs::remove_all(folder, ignored);
        }
    }
    const fs::path& path() const { return folder; }
    void publish(const fs::path& destination) {
        rename_file(handle.get(), destination);
        published = true;
    }
};

struct Subdirectory {
    fs::path name;
    FileHandle handle;
};

void copy_file(HANDLE payload, const TorrentFile& entry, const fs::path& path) {
    const auto file = open_handle(path, GENERIC_WRITE, 0, CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT);
    seek_file(payload, entry.offset);
    std::array<char, 65536> buffer;
    for (auto remaining = entry.length; remaining > 0;) {
        const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        read_file(payload, std::span(buffer).first(length));
        write_file(file.get(), std::string_view(buffer.data(), length));
        remaining -= length;
    }
    flush_file(file.get());
}
}

void publish_folder(HANDLE payload, const fs::path& destination, std::span<const TorrentFile> files) {
    StagedFolder stage(destination.parent_path());
    std::vector<const TorrentFile*> ordered;
    ordered.reserve(files.size());
    for (const auto& file : files) ordered.push_back(&file);
    std::ranges::sort(ordered, {}, &TorrentFile::path);

    // Group files by directory to keep only the current chain of directories open.
    std::vector<Subdirectory> parents;
    for (const auto* file : ordered) {
        auto path = stage.path();
        std::size_t depth{};
        for (const auto& component : file->path.parent_path()) {
            path /= component;
            if (depth == parents.size() || parents[depth].name != component) {
                parents.resize(depth);
                create_staged_directory(path);
                parents.push_back({component, lock_directory(path)});
            }
            ++depth;
        }
        parents.resize(depth);
        copy_file(payload, *file, path / file->path.filename());
    }

    parents.clear();
    stage.publish(destination);
}
}
