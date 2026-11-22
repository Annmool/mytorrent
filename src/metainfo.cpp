#include "metainfo.hpp"
#include "bencode.hpp"
#include "payload.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <stdexcept>
#include <system_error>

namespace bt {
namespace fs = std::filesystem;
namespace {
void check_crypto(NTSTATUS status) {
    if (status < 0) throw std::runtime_error("Windows cryptography operation failed");
}

struct Sha1Provider {
    BCRYPT_ALG_HANDLE handle{};
    Sha1Provider() { check_crypto(BCryptOpenAlgorithmProvider(&handle, BCRYPT_SHA1_ALGORITHM, nullptr, 0)); }
    ~Sha1Provider() { BCryptCloseAlgorithmProvider(handle, 0); }
};

struct Hash {
    BCRYPT_HASH_HANDLE handle{};
    explicit Hash(BCRYPT_ALG_HANDLE provider) {
        check_crypto(BCryptCreateHash(provider, &handle, nullptr, 0, nullptr, 0, 0));
    }
    ~Hash() { BCryptDestroyHash(handle); }
};

void validate_tracker(std::string_view url) {
    if ((!url.starts_with("http://") && !url.starts_with("https://")) ||
        url.size() > 4096 || url.find('#') != url.npos ||
        url.find_first_of("\r\n\t ") != url.npos || url.find('\0') != url.npos) {
        throw std::runtime_error("Expected an HTTP(S) tracker URL");
    }
}

std::vector<TorrentFile> parse_files(const Value::List& entries) {
    std::vector<TorrentFile> files;
    files.reserve(entries.size());
    for (const auto& entry : entries) {
        const auto length = entry.at("length").integer();
        const auto& components = entry.at("path").list();
        if (length < 0 || components.empty() || components.size() > max_path_components) {
            throw std::runtime_error("Invalid torrent file length or path depth");
        }
        fs::path path;
        for (const auto& component : components) path /= safe_name(component.bytes());
        files.push_back({std::move(path), static_cast<std::uint64_t>(length)});
    }
    return files;
}

Value encode_files(const std::vector<TorrentFile>& files) {
    Value::List entries;
    entries.reserve(files.size());
    for (const auto& file : files) {
        Value::List path;
        for (const auto& component : file.path) path.push_back(Value{path_utf8(component)});
        entries.push_back(Value{Value::Dict{{"length", Value{static_cast<std::int64_t>(file.length)}},
                                            {"path", Value{std::move(path)}}}});
    }
    return Value{std::move(entries)};
}
}

std::string sha1(std::string_view bytes) {
    static const Sha1Provider provider;
    Hash hash(provider.handle);
    if (!bytes.empty()) {
        auto* input = reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data()));
        check_crypto(BCryptHashData(hash.handle, input, static_cast<ULONG>(bytes.size()), 0));
    }
    std::string digest(digest_size, '\0');
    check_crypto(BCryptFinishHash(hash.handle, reinterpret_cast<PUCHAR>(digest.data()), digest_size, 0));
    return digest;
}

std::string hex(std::string_view bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes) {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}

std::string peer_id() {
    std::string id = "-MT0100-" + std::string(12, '\0');
    check_crypto(BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(id.data() + 8), 12,
                                BCRYPT_USE_SYSTEM_PREFERRED_RNG));
    return id;
}

fs::path safe_name(std::string_view name) {
    if (name.empty() || name.size() > 255 || name.back() == '.' || name.back() == ' ') {
        throw std::runtime_error("Unsafe torrent filename");
    }
    for (unsigned char ch : name) {
        if (ch < 32 || std::string_view("<>:\"/\\|?*").find(ch) != name.npos) {
            throw std::runtime_error("Unsafe torrent filename");
        }
    }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(),
                                          static_cast<int>(name.size()), nullptr, 0);
    if (!count) throw std::runtime_error("Invalid filename UTF-8");
    std::wstring wide(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(),
                        static_cast<int>(name.size()), wide.data(), count);
    auto stem = wide.substr(0, wide.find(L'.'));
    while (!stem.empty() && stem.back() == L' ') stem.pop_back();
    for (auto& ch : stem) if (ch >= L'a' && ch <= L'z') ch -= L'a' - L'A';
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
        stem == L"CONIN$" || stem == L"CONOUT$" ||
        (stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) &&
         std::wstring_view(L"0123456789\u00b9\u00b2\u00b3").find(stem.back()) != stem.npos)) {
        throw std::runtime_error("Reserved Windows filename");
    }
    return fs::path(wide);
}

std::uint32_t Metainfo::count() const { return static_cast<std::uint32_t>(hashes.size() / digest_size); }
std::uint64_t Metainfo::offset(std::uint32_t index) const {
    if (index >= count()) throw std::runtime_error("Piece index outside torrent");
    return std::uint64_t(index) * piece_length;
}
std::uint32_t Metainfo::size(std::uint32_t index) const {
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(piece_length, length - offset(index)));
}
bool Metainfo::matches(std::uint32_t index, std::string_view data) const {
    return data.size() == size(index) &&
        sha1(data) == std::string_view(hashes).substr(std::size_t(index) * digest_size, digest_size);
}

Metainfo parse_metainfo(std::string_view bytes) {
    const auto root = decode(bytes);
    const auto& info = root.at("info");
    const auto has_files = info.dict().contains("files");
    if (has_files == info.dict().contains("length") || info.dict().contains("meta version")) {
        throw std::runtime_error("Expected a v1 torrent with either length or files");
    }
    if (info.dict().contains("private")) throw std::runtime_error("Private torrent flags are unsupported");
    const auto piece_length = info.at("piece length").integer();
    if (piece_length <= 0 || piece_length > max_piece_size) {
        throw std::runtime_error("Unsupported piece size");
    }
    auto files = has_files ? parse_files(info.at("files").list()) : std::vector<TorrentFile>{};
    std::uint64_t length{};
    if (has_files) {
        length = assign_file_offsets(files);
    } else {
        const auto file_length = info.at("length").integer();
        if (file_length <= 0) throw std::runtime_error("Unsupported file length");
        length = static_cast<std::uint64_t>(file_length);
    }
    Metainfo meta{info.at("name").bytes(), root.at("announce").bytes(),
        length, static_cast<std::uint32_t>(piece_length), info.at("pieces").bytes(),
        sha1(bytes.substr(info.begin, info.end - info.begin)), std::move(files)};
    validate_tracker(meta.announce);
    safe_name(meta.name);
    const auto expected = 1 + (meta.length - 1) / meta.piece_length;
    if (meta.hashes.size() % digest_size || meta.count() != expected) {
        throw std::runtime_error("Piece hashes do not match torrent length");
    }
    return meta;
}

Metainfo load_metainfo(const fs::path& path) {
    const auto file = open_handle(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, 0);
    const auto size = file_length(file.get());
    if (size > metadata_limit) throw std::runtime_error("Torrent metadata is too large");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    read_file(file.get(), bytes);
    return parse_metainfo(bytes);
}

std::string create_metainfo(const fs::path& path, std::string announce, std::uint32_t piece_length) {
    validate_tracker(announce);
    if (piece_length < block_size || piece_length > max_piece_size ||
        (piece_length & (piece_length - 1))) {
        throw std::runtime_error("Piece size must be a power of two between 16 KiB and 16 MiB");
    }
    auto source = fs::absolute(path).lexically_normal().make_preferred();
    if (!source.has_filename() && source.has_relative_path()) source = source.parent_path();
    const auto name = path_utf8(source.filename());
    safe_name(name);
    const bool folder = fs::is_directory(source);
    auto files = folder ? collect_folder_files(source) : std::vector<TorrentFile>{};
    const auto length = folder ? assign_file_offsets(files) : fs::file_size(source);
    if (!length || 1 + (length - 1) / piece_length > (metadata_limit - 8192) / digest_size) {
        throw std::runtime_error("Empty payload or too many pieces for the metadata limit");
    }

    PayloadReader payload(source, length, files);
    std::string hashes;
    for (std::uint64_t offset = 0; offset < length;) {
        const auto size = static_cast<std::uint32_t>(std::min<std::uint64_t>(piece_length, length - offset));
        hashes += sha1(payload.read(offset, size));
        offset += size;
    }
    Value::Dict info{{"name", Value{name}},
                     {"piece length", Value{std::int64_t(piece_length)}}, {"pieces", Value{std::move(hashes)}}};
    if (folder) info.emplace("files", encode_files(files));
    else info.emplace("length", Value{static_cast<std::int64_t>(length)});
    auto torrent = encode(Value{Value::Dict{{"announce", Value{std::move(announce)}},
                                            {"info", Value{std::move(info)}}}});
    if (torrent.size() > metadata_limit) throw std::runtime_error("Torrent metadata is too large");
    return torrent;
}

}
