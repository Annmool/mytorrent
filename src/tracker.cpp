#include "tracker.hpp"
#include "bencode.hpp"

#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <stdexcept>
#include <system_error>

namespace bt {
namespace {
void check(BOOL success, const char* message) {
    if (!success) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), message);
}
class Http {
public:
    HINTERNET handle;
    explicit Http(HINTERNET value) : handle(value) { check(handle != nullptr, "Opening HTTP handle"); }
    ~Http() { WinHttpCloseHandle(handle); }
    Http(const Http&) = delete;
};

std::string get(std::string_view url) {
    const std::wstring wide(url.begin(), url.end());
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = DWORD(-1);
    check(WinHttpCrackUrl(wide.c_str(), 0, ICU_REJECT_USERPWD, &parts), "Parsing tracker URL");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring target = parts.dwUrlPathLength ?
        std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) : L"/";
    if (parts.dwExtraInfoLength) target.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    Http session(WinHttpOpen(L"MyTorrent/0.1", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0));
    check(WinHttpSetTimeouts(session.handle, 5000, 5000, 5000, 5000), "Setting tracker timeouts");
    Http connection(WinHttpConnect(session.handle, host.c_str(), parts.nPort, 0));
    Http request(WinHttpOpenRequest(connection.handle, L"GET", target.c_str(), nullptr, nullptr,
        nullptr, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    DWORD disable = WINHTTP_DISABLE_REDIRECTS;
    check(WinHttpSetOption(request.handle, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable)),
          "Disabling tracker redirects");
    check(WinHttpSendRequest(request.handle, nullptr, 0, nullptr, 0, 0, 0), "Sending tracker announce");
    check(WinHttpReceiveResponse(request.handle, nullptr), "Receiving tracker response");
    DWORD status{}, size = sizeof(status);
    check(WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             nullptr, &status, &size, nullptr), "Reading tracker status");
    if (status != 200) throw std::runtime_error("Tracker HTTP status " + std::to_string(status));

    std::string data;
    char chunk[8192];
    while (true) {
        DWORD read{};
        check(WinHttpReadData(request.handle, chunk, sizeof(chunk), &read), "Reading tracker body");
        if (!read) return data;
        if (data.size() + read > metadata_limit) throw std::runtime_error("Tracker reply too large");
        data.append(chunk, read);
    }
}
}

std::string escape_query(std::string_view bytes) {
    std::string result;
    constexpr char digits[] = "0123456789ABCDEF";
    for (unsigned char byte : bytes) {
        result += '%';
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}

TrackerReply parse_tracker_reply(std::string_view data) {
    const auto root = decode(data);
    if (const auto fail = root.dict().find("failure reason"); fail != root.dict().end()) {
        throw std::runtime_error("Tracker rejected announce: " + fail->second.bytes());
    }
    const auto interval = root.at("interval").integer();
    if (interval <= 0 || interval > 86400) throw std::runtime_error("Invalid tracker interval");
    TrackerReply result{static_cast<unsigned>(interval), {}};
    const auto& peers = root.at("peers");
    if (const auto* compact = std::get_if<std::string>(&peers.data)) {
        if (compact->size() % 6) throw std::runtime_error("Malformed compact IPv4 peer list");
        for (std::size_t offset = 0; offset < compact->size() && result.peers.size() < 100; offset += 6) {
            const auto* bytes = reinterpret_cast<const unsigned char*>(compact->data() + offset);
            std::string ip = std::to_string(bytes[0]);
            for (int i = 1; i < 4; ++i) ip += "." + std::to_string(bytes[i]);
            const auto port = static_cast<std::uint16_t>((bytes[4] << 8) | bytes[5]);
            if (port) result.peers.push_back({std::move(ip), port});
        }
    } else {
        for (const auto& peer : peers.list()) {
            if (result.peers.size() == 100) break;
            try {
                // Older trackers can return non-IPv4 entries. Skip unsupported endpoints.
                result.peers.push_back(parse_endpoint(peer.at("ip").bytes() + ":" +
                    std::to_string(peer.at("port").integer())));
            } catch (const std::runtime_error&) {}
        }
    }
    return result;
}

TrackerReply announce(const Metainfo& meta, std::string_view id, const Announce& state,
                      std::string_view event) {
    auto url = meta.announce;
    url += url.find('?') == url.npos ? '?' : '&';
    url += "info_hash=" + escape_query(meta.info_hash) + "&peer_id=" + escape_query(id) +
           "&port=" + std::to_string(state.port) + "&uploaded=" + std::to_string(state.uploaded) +
           "&downloaded=" + std::to_string(state.downloaded) + "&left=" + std::to_string(state.left) +
           "&compact=1&numwant=25";
    if (!event.empty()) url += "&event=" + std::string(event);
    return parse_tracker_reply(get(url));
}
}
