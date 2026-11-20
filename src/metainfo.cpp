#include "metainfo.hpp"
#include "bencode.hpp"
#include "storage.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
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
    if (info.dict().contains("files") || info.dict().contains("meta version")) {
        throw std::runtime_error("Expected a single-file v1 torrent");
    }
    if (info.dict().contains("private")) throw std::runtime_error("Private torrent flags are unsupported");
    const auto length = info.at("length").integer();
    const auto piece_length = info.at("piece length").integer();
    if (length <= 0 || piece_length <= 0 || piece_length > max_piece_size) {
        throw std::runtime_error("Unsupported file length or piece size");
    }
    Metainfo meta{info.at("name").bytes(), root.at("announce").bytes(),
        static_cast<std::uint64_t>(length), static_cast<std::uint32_t>(piece_length),
        info.at("pieces").bytes(), sha1(bytes.substr(info.begin, info.end - info.begin))};
    validate_tracker(meta.announce);
    safe_name(meta.name);
    const auto expected = 1 + (meta.length - 1) / meta.piece_length;
    if (meta.hashes.size() % digest_size || meta.count() != expected) {
        throw std::runtime_error("Piece hashes do not match torrent length");
    }
    return meta;
}

Metainfo load_metainfo(const fs::path& path) {
    const auto size = fs::file_size(path);
    if (size > metadata_limit) throw std::runtime_error("Torrent metadata is too large");
    std::ifstream file(path, std::ios::binary);
    return parse_metainfo(read_exact(file, static_cast<std::size_t>(size)));
}

std::string create_metainfo(const fs::path& path, std::string announce, std::uint32_t piece_length) {
    validate_tracker(announce);
    if (piece_length < block_size || piece_length > max_piece_size ||
        (piece_length & (piece_length - 1))) {
        throw std::runtime_error("Piece size must be a power of two between 16 KiB and 16 MiB");
    }
    const auto length = fs::file_size(path);
    if (!length || 1 + (length - 1) / piece_length > (metadata_limit - 8192) / digest_size) {
        throw std::runtime_error("Empty file or too many pieces for the metadata limit");
    }
    const auto utf8 = path.filename().u8string();
    const std::string name(utf8.begin(), utf8.end());
    safe_name(name);

    std::ifstream file(path, std::ios::binary);
    std::string hashes;
    for (auto remaining = length; remaining > 0;) {
        const auto bytes = std::min<std::uintmax_t>(piece_length, remaining);
        hashes += sha1(read_exact(file, static_cast<std::size_t>(bytes)));
        remaining -= bytes;
    }
    if (file.peek() != EOF || !file.eof() || file.bad()) throw std::runtime_error("Source changed or read failed");
    Value::Dict info{{"length", Value{static_cast<std::int64_t>(length)}}, {"name", Value{name}},
                     {"piece length", Value{std::int64_t(piece_length)}}, {"pieces", Value{std::move(hashes)}}};
    return encode(Value{Value::Dict{{"announce", Value{std::move(announce)}}, {"info", Value{std::move(info)}}}});
}

}
