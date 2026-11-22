#include "net.hpp"
#include <ws2tcpip.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace bt {
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto timeout = std::chrono::seconds(10);

[[noreturn]] void net_error(const char* operation, int code = WSAGetLastError()) {
    throw std::system_error(code, std::system_category(), operation);
}

bool wait_for(SOCKET socket, bool writing, Clock::time_point deadline, bool connecting = false,
              std::stop_token stop = {}) {
    using namespace std::chrono;
    while (!stop.stop_requested()) {
        const auto remaining = duration_cast<microseconds>(deadline - Clock::now());
        if (remaining.count() <= 0) throw std::runtime_error("Socket deadline exceeded");
        const auto slice = stop.stop_possible() ? std::min(remaining, microseconds{100000}) : remaining;
        timeval time{static_cast<long>(slice.count() / 1000000),
                     static_cast<long>(slice.count() % 1000000)};
        fd_set ready, errors;
        FD_ZERO(&ready);
        FD_ZERO(&errors);
        FD_SET(socket, &ready);
        FD_SET(socket, &errors);
        const int result = select(0, writing ? nullptr : &ready, writing ? &ready : nullptr,
                                  connecting ? &errors : nullptr, &time);
        if (result == SOCKET_ERROR) net_error("Waiting for socket");
        if (result) return !stop.stop_requested();
        if (slice == remaining) throw std::runtime_error("Socket timed out");
    }
    return false;
}

sockaddr_in address(const Endpoint& endpoint) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(endpoint.port);
    if (InetPtonA(AF_INET, endpoint.ip.c_str(), &addr.sin_addr) != 1) {
        throw std::runtime_error("Expected a numeric IPv4 address");
    }
    return addr;
}
}

Network::Network() {
    WSADATA data{};
    if (const auto error = WSAStartup(MAKEWORD(2, 2), &data)) net_error("Starting Winsock", error);
}
Network::~Network() { WSACleanup(); }

Endpoint parse_endpoint(std::string_view text) {
    const auto colon = text.find(':');
    if (colon == text.npos) throw std::runtime_error("Expected IPv4:PORT");
    const auto digits = text.substr(colon + 1);
    unsigned port{};
    const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), port);
    if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size() || !port || port > 65535) {
        throw std::runtime_error("Invalid peer port");
    }
    Endpoint endpoint{std::string(text.substr(0, colon)), static_cast<std::uint16_t>(port)};
    address(endpoint);
    return endpoint;
}

Socket::Socket(SOCKET value) : handle(value) {
    if (handle == INVALID_SOCKET) net_error("Opening socket");
}
Socket::~Socket() { if (handle != INVALID_SOCKET) closesocket(handle); }
Socket::Socket(Socket&& other) noexcept : handle(std::exchange(other.handle, INVALID_SOCKET)) {}

Socket Socket::connect_to(const Endpoint& endpoint) {
    const auto addr = address(endpoint);
    Socket socket(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    u_long mode = 1;
    if (ioctlsocket(socket.handle, FIONBIO, &mode)) net_error("Setting nonblocking mode");
    if (connect(socket.handle, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr))) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) net_error("Connecting to peer");
        wait_for(socket.handle, true, Clock::now() + timeout, true);
        int error{};
        int size = sizeof(error);
        if (getsockopt(socket.handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size)) {
            net_error("Checking connection");
        }
        if (error) net_error("Connecting to peer", error);
    }
    return socket;
}

Socket Socket::listen_on(std::uint16_t port) {
    Socket socket(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    const BOOL exclusive = TRUE;
    if (setsockopt(socket.handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char*>(&exclusive), sizeof(exclusive))) net_error("Setting exclusive bind");
    const auto addr = address({"0.0.0.0", port});
    if (bind(socket.handle, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr))) net_error("Binding port");
    if (listen(socket.handle, SOMAXCONN)) net_error("Listening for peers");
    return socket;
}

bool Socket::has_peer() const {
    fd_set ready;
    FD_ZERO(&ready);
    FD_SET(handle, &ready);
    timeval wait{0, 250000};
    const int result = select(0, &ready, nullptr, nullptr, &wait);
    if (result == SOCKET_ERROR) net_error("Waiting for incoming peer");
    return result != 0;
}

Socket Socket::accept_peer() const {
    Socket peer(accept(handle, nullptr, nullptr));
    u_long mode = 1;
    if (ioctlsocket(peer.handle, FIONBIO, &mode)) net_error("Setting nonblocking peer mode");
    return peer;
}

std::uint16_t Socket::port() const {
    sockaddr_in addr{};
    int size = sizeof(addr);
    if (getsockname(handle, reinterpret_cast<sockaddr*>(&addr), &size)) net_error("Reading bound port");
    return ntohs(addr.sin_port);
}

bool Socket::receive_into(std::string& buffer, bool allow_eof, std::stop_token stop) const {
    const auto deadline = Clock::now() + timeout;
    for (std::size_t offset = 0; offset < buffer.size();) {
        if (!wait_for(handle, false, deadline, false, stop)) return false;
        const auto size = static_cast<int>(std::min<std::size_t>(buffer.size() - offset, 65536));
        const int result = recv(handle, buffer.data() + offset, size, 0);
        if (result == 0) {
            if (offset == 0 && allow_eof) return false;
            throw std::runtime_error("Peer disconnected mid-read");
        }
        if (result == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
            net_error("Receiving peer data");
        }
        offset += result;
    }
    return true;
}

std::string Socket::receive(std::size_t size, std::stop_token stop) const {
    std::string buffer(size, '\0');
    if (!receive_into(buffer, false, stop)) throw std::runtime_error("Socket read cancelled");
    return buffer;
}

void Socket::send_all(std::string_view bytes, std::stop_token stop) const {
    const auto deadline = Clock::now() + timeout;
    while (!bytes.empty()) {
        if (!wait_for(handle, true, deadline, false, stop)) return;
        const auto size = static_cast<int>(std::min<std::size_t>(bytes.size(), 65536));
        const int result = send(handle, bytes.data(), size, 0);
        if (result == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
        if (result <= 0) net_error("Sending peer data");
        bytes.remove_prefix(result);
    }
}
}
