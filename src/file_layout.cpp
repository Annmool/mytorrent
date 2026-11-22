#include "file_layout.hpp"

#include <windows.h>
#include <limits>
#include <map>
#include <stdexcept>

namespace bt {
namespace {
struct IgnoreCase {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    }
};

struct PathNode {
    std::map<std::wstring, PathNode, IgnoreCase> children;
    bool file{};
};

void add_path(PathNode& root, const std::filesystem::path& path) {
    auto* node = &root;
    for (const auto& component : path) {
        if (node->file) throw std::runtime_error("Torrent path is both a file and a directory");
        auto [entry, inserted] = node->children.try_emplace(component.native());
        if (!inserted && entry->first != component.native()) {
            throw std::runtime_error("Torrent paths differ only in letter case");
        }
        node = &entry->second;
    }
    if (node->file || !node->children.empty()) throw std::runtime_error("Conflicting torrent file path");
    node->file = true;
}
}

std::string path_utf8(const std::filesystem::path& path) {
    const auto bytes = path.generic_u8string();
    return {bytes.begin(), bytes.end()};
}

std::uint64_t assign_file_offsets(std::vector<TorrentFile>& files) {
    constexpr auto max_length = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    PathNode paths;
    std::uint64_t total{};
    for (auto& file : files) {
        add_path(paths, file.path);
        if (file.length > max_length - total) {
            throw std::runtime_error("Total torrent length is too large");
        }
        file.offset = total;
        total += file.length;
    }
    if (!total) throw std::runtime_error("Torrent payload is empty");
    return total;
}
}
