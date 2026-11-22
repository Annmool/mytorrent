#include "console_interrupt.hpp"
#include "storage.hpp"
#include "transfer.hpp"

#include <windows.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
constexpr auto usage = R"(MyTorrent 0.1.0
Usage:
  mytorrent create FILE --tracker URL --out FILE.torrent [--piece-size BYTES]
  mytorrent inspect FILE.torrent
  mytorrent verify FILE.torrent --file FILE
  mytorrent seed FILE.torrent --file FILE [--port PORT] [--no-tracker]
  mytorrent download FILE.torrent --out DIRECTORY [--peer IPV4:PORT] [--resume]
)";

std::string utf8(std::wstring_view text) {
    const auto size = static_cast<int>(text.size());
    if (!size) return {};
    const auto needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), size,
                                            nullptr, 0, nullptr, nullptr);
    if (!needed) throw std::runtime_error("Invalid command-line text");
    std::string result(needed, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), size,
                        result.data(), needed, nullptr, nullptr);
    return result;
}

class Options {
    std::map<std::wstring, std::wstring, std::less<>> values;
public:
    Options(int argc, wchar_t** argv, std::initializer_list<std::wstring_view> allowed) {
        for (int i = 3; i < argc; ++i) {
            const std::wstring_view key(argv[i]);
            if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
                throw std::runtime_error("Unknown option: " + utf8(key));
            }
            std::wstring value;
            if (key != L"--no-tracker" && key != L"--resume") {
                if (++i == argc || std::wstring_view(argv[i]).starts_with(L"--")) {
                    throw std::runtime_error("Missing option value: " + utf8(key));
                }
                value = argv[i];
            }
            if (!values.emplace(key, std::move(value)).second) throw std::runtime_error("Duplicate option");
        }
    }
    bool has(std::wstring_view key) const { return values.contains(key); }
    const std::wstring& get(std::wstring_view key) const {
        const auto it = values.find(key);
        if (it == values.end()) throw std::runtime_error("Required option: " + utf8(key));
        return it->second;
    }
    unsigned number(std::wstring_view key, unsigned fallback, unsigned maximum) const {
        if (!has(key)) return fallback;
        unsigned value{};
        const auto& input = get(key);
        if (input.empty()) throw std::runtime_error("Empty numeric option");
        for (const auto digit : input) {
            if (digit < L'0' || digit > L'9' || value > maximum / 10) {
                throw std::runtime_error("Invalid numeric option: " + utf8(key));
            }
            value = value * 10 + static_cast<unsigned>(digit - L'0');
        }
        if (value > maximum) throw std::runtime_error("Numeric option out of range: " + utf8(key));
        return value;
    }
};
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 1 || (argc == 2 && std::wstring_view(argv[1]) == L"--help")) {
            std::cout << usage;
            return 0;
        }
        if (argc == 2 && std::wstring_view(argv[1]) == L"--version") {
            std::cout << "0.1.0\n";
            return 0;
        }
        if (argc < 3) throw std::runtime_error("Missing input path. See --help.");
        const std::wstring_view command(argv[1]);
        const std::filesystem::path input(argv[2]);
        if (command == L"create") {
            const Options options(argc, argv, {L"--tracker", L"--out", L"--piece-size"});
            bt::save_new(options.get(L"--out"), bt::create_metainfo(input, utf8(options.get(L"--tracker")),
                options.number(L"--piece-size", 256 * 1024, bt::max_piece_size)));
            std::cout << "Torrent created\n";
            return 0;
        }
        if (command != L"inspect" && command != L"verify" && command != L"seed" && command != L"download") {
            throw std::runtime_error("Unknown command. See --help.");
        }
        const auto meta = bt::load_metainfo(input);
        if (command == L"inspect") {
            const Options options(argc, argv, {});
            std::cout << "Name: " << meta.name << "\nBytes: " << meta.length
                << "\nPiece length: " << meta.piece_length << "\nPieces: " << meta.count()
                << "\nInfo hash: " << bt::hex(meta.info_hash) << "\nTracker: " << meta.announce << '\n';
        } else if (command == L"verify") {
            const Options options(argc, argv, {L"--file"});
            std::ifstream file(std::filesystem::path(options.get(L"--file")), std::ios::binary);
            bt::verify_file(meta, file);
            std::cout << "File verified\n";
        } else if (command == L"seed") {
            const Options options(argc, argv, {L"--file", L"--port", L"--no-tracker"});
            const bt::Network network;
            const bt::ConsoleInterrupt interrupt;
            bt::seed(meta, options.get(L"--file"), static_cast<std::uint16_t>(options.number(L"--port", 6881, 65535)),
                     !options.has(L"--no-tracker"), interrupt.requested());
        } else {
            const Options options(argc, argv, {L"--out", L"--peer", L"--resume"});
            const bt::Network network;
            if (options.has(L"--peer")) {
                const auto peer = bt::parse_endpoint(utf8(options.get(L"--peer")));
                bt::download(meta, options.get(L"--out"), &peer, options.has(L"--resume"));
            } else {
                bt::download(meta, options.get(L"--out"), nullptr, options.has(L"--resume"));
            }
            std::cout << "Download verified and saved\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
