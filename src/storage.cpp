#include "storage.hpp"
#include "folder_output.hpp"

#include <stdexcept>

namespace bt {
namespace fs = std::filesystem;

OutputFile::OutputFile(fs::path path, std::string_view resume_hash, std::span<const TorrentFile> entries)
    : final_path(fs::absolute(path).make_preferred()), files(entries), resumable(!resume_hash.empty()) {
    if (!files.empty()) parent = lock_directory(final_path.parent_path());
    if (fs::exists(fs::symlink_status(final_path))) throw std::runtime_error("Output already exists");
    const auto identifier = resumable ? hex(resume_hash) : hex(std::string_view(peer_id()).substr(8));
    const auto partial = final_path.parent_path() / (".mytorrent-" + identifier + ".part");
    handle = open_handle(partial, GENERIC_READ | GENERIC_WRITE | DELETE, 0,
                         resumable ? OPEN_ALWAYS : CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT);
    if (resumable) {
        const auto info = file_information(handle.get());
        if ((info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
            info.nNumberOfLinks != 1 || GetFileType(handle.get()) != FILE_TYPE_DISK) {
            throw std::runtime_error("Expected a regular, unlinked partial output file");
        }
    }
}

OutputFile::~OutputFile() {
    if (!published && !resumable) {
        FILE_DISPOSITION_INFO disposition{TRUE};
        SetFileInformationByHandle(handle.get(), FileDispositionInfo, &disposition, sizeof(disposition));
    }
}

std::uint32_t OutputFile::recheck(const Metainfo& meta) {
    const auto available = file_length(handle.get());
    std::uint64_t verified{};
    std::uint32_t index{};
    for (; index < meta.count(); ++index) {
        const auto length = meta.size(index);
        if (length > available - verified) break;
        std::string data(length, '\0');
        read_file(handle.get(), data);
        if (!meta.matches(index, data)) break;
        verified += length;
    }
    seek_file(handle.get(), verified);
    if (verified != available && !SetEndOfFile(handle.get())) file_error("Truncating invalid partial data");
    return index;
}

void OutputFile::write(std::string_view bytes) { write_file(handle.get(), bytes); }

void OutputFile::publish() {
    flush_file(handle.get());
    if (files.empty()) {
        rename_file(handle.get(), final_path);
        published = true;
    } else {
        publish_folder(handle.get(), final_path, files);
        // The verified stream is only needed again if publication fails.
        resumable = false;
    }
}

void save_new(const fs::path& path, std::string_view bytes) {
    OutputFile file(path);
    file.write(bytes);
    file.publish();
}
}
