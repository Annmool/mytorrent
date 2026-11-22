#pragma once

#include "file_layout.hpp"
#include "windows_file.hpp"

namespace bt {
void publish_folder(HANDLE payload, const std::filesystem::path& destination,
                    std::span<const TorrentFile> files);
}
