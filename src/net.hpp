#pragma once
#include <winsock2.h>
#include <cstdint>
#include <string>
#include <string_view>

namespace bt {
struct Network {
    Network();
    ~Network();
    Network(const Network&) = delete;
    Network& operator=(const Network&) = delete;
};

struct Endpoint {
    std::string ip;
    std::uint16_t port{};
};
Endpoint parse_endpoint(std::string_view text);

class Socket {
    SOCKET handle = INVALID_SOCKET;
    explicit Socket(SOCKET value);
public:
    ~Socket();
    Socket(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    static Socket connect_to(const Endpoint& endpoint);
    static Socket listen_on(std::uint16_t port);
    bool has_peer() const;
    Socket accept_peer() const;
    std::uint16_t port() const;
    std::string receive(std::size_t size) const;
    bool receive_into(std::string& buffer, bool allow_eof = false) const;
    void send_all(std::string_view bytes) const;
};
}
