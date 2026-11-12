#include "torrent/network/peer/peer_listener.h"
#include <stdexcept>
#include <utility>
#include <algorithm>
#include <array>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#define closesocket close
#endif

namespace torrent {
namespace {
struct Socket {
    std::intptr_t value = -1;
    ~Socket() { if (value != -1) closesocket(value); }
};
}

PeerListener::PeerListener(uint16_t port, bool ipv6) {
#ifdef _WIN32
    static const bool initialized = [] { WSADATA data; return WSAStartup(MAKEWORD(2, 2), &data) == 0; }();
    if (!initialized) throw std::runtime_error("WSAStartup failed");
#endif
    Socket listener;
    bool dual = false;
    if (ipv6) {
        listener.value = ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
        if (listener.value != -1) {
            const int disabled = 0;
            dual = setsockopt(listener.value, IPPROTO_IPV6, IPV6_V6ONLY,
                             reinterpret_cast<const char*>(&disabled), sizeof(disabled)) == 0;
            if (!dual) { closesocket(listener.value); listener.value = -1; }
        }
    }
    if (listener.value == -1) listener.value = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener.value == -1) throw std::runtime_error("Cannot create TCP listener");
    const int enabled = 1;
#ifdef _WIN32
    const int option = SO_EXCLUSIVEADDRUSE;
#else
    const int option = SO_REUSEADDR;
#endif
    if (setsockopt(listener.value, SOL_SOCKET, option, reinterpret_cast<const char*>(&enabled), sizeof(enabled)) != 0)
        throw std::runtime_error("Cannot configure TCP listener");
    sockaddr_storage address{};
    int length;
    if (dual) {
        auto& v6 = reinterpret_cast<sockaddr_in6&>(address);
        v6.sin6_family = AF_INET6; v6.sin6_addr = in6addr_any; v6.sin6_port = htons(port);
        length = sizeof(v6);
    } else {
        auto& v4 = reinterpret_cast<sockaddr_in&>(address);
        v4.sin_family = AF_INET; v4.sin_addr.s_addr = htonl(INADDR_ANY); v4.sin_port = htons(port);
        length = sizeof(v4);
    }
    if (::bind(listener.value, reinterpret_cast<sockaddr*>(&address), length) != 0 || ::listen(listener.value, 64) != 0)
        throw std::runtime_error("Cannot listen on TCP port " + std::to_string(port) + "; the port may already be in use");
#ifdef _WIN32
    u_long mode = 1;
    if (ioctlsocket(listener.value, FIONBIO, &mode) != 0) throw std::runtime_error("Cannot configure TCP accept polling");
#else
    if (listener.value >= FD_SETSIZE || fcntl(listener.value, F_SETFL, fcntl(listener.value, F_GETFL, 0) | O_NONBLOCK) < 0)
        throw std::runtime_error("Cannot configure TCP accept polling");
#endif
    socklen_t size = sizeof(address);
    if (getsockname(listener.value, reinterpret_cast<sockaddr*>(&address), &size) != 0)
        throw std::runtime_error("Cannot read TCP listener address");
    port_ = ntohs(dual ? reinterpret_cast<sockaddr_in6&>(address).sin6_port : reinterpret_cast<sockaddr_in&>(address).sin_port);
    socket_ = std::exchange(listener.value, -1);
    ipv6_ = dual;
}

PeerListener::~PeerListener() { if (socket_ != -1) closesocket(socket_); }

ListenerStatus PeerListener::status() const {
    ListenerStatus result{ListenerState::Error, port_, ipv6_, {}};
    int accepting = 0;
    socklen_t size = sizeof(accepting);
    if (getsockopt(socket_, SOL_SOCKET, SO_ACCEPTCONN, reinterpret_cast<char*>(&accepting), &size) != 0)
        result.error = "Cannot check TCP listener";
    else if (!accepting) result.error = "TCP socket is not listening";
    else result.state = ListenerState::Listening;
    return result;
}

bool isLoopbackAddress(const std::string& address) {
    std::array<uint8_t, 16> bytes{};
    if (inet_pton(AF_INET, address.c_str(), bytes.data()) == 1) return bytes[0] == 127;
    if (inet_pton(AF_INET6, address.c_str(), bytes.data()) != 1) return false;
    const auto zero = [](uint8_t value) { return value == 0; };
    return (std::all_of(bytes.begin(), bytes.begin() + 15, zero) && bytes[15] == 1) ||
        (std::all_of(bytes.begin(), bytes.begin() + 10, zero) && bytes[10] == 255 && bytes[11] == 255 && bytes[12] == 127);
}

std::unique_ptr<PeerConnection> PeerListener::accept(const std::vector<uint8_t>& hash, const std::string& id, int timeout_ms) {
    fd_set readable; FD_ZERO(&readable); FD_SET(socket_, &readable);
    timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    if (::select(static_cast<int>(socket_ + 1), &readable, nullptr, nullptr, &timeout) <= 0) return nullptr;
    sockaddr_storage address{}; socklen_t length = sizeof(address);
    Socket client{static_cast<std::intptr_t>(::accept(socket_, reinterpret_cast<sockaddr*>(&address), &length))};
    if (client.value == -1) return nullptr;
#ifndef _WIN32
    if (client.value >= FD_SETSIZE) return nullptr;
#endif
    char host[INET6_ADDRSTRLEN]{};
    uint16_t port;
    if (address.ss_family == AF_INET6) {
        const auto& v6 = reinterpret_cast<sockaddr_in6&>(address);
        if (!inet_ntop(AF_INET6, &v6.sin6_addr, host, sizeof(host))) return nullptr;
        port = ntohs(v6.sin6_port);
    } else {
        const auto& v4 = reinterpret_cast<sockaddr_in&>(address);
        if (!inet_ntop(AF_INET, &v4.sin_addr, host, sizeof(host))) return nullptr;
        port = ntohs(v4.sin_port);
    }
    auto connection = std::make_unique<PeerConnection>(host, port, hash, id);
    connection->socket_fd_ = std::exchange(client.value, -1);
    connection->connected_ = true;
    connection->incoming_ = true;
    if (!connection->setNonBlocking(true) || !connection->setSocketTimeout(30000)) return nullptr;
    return connection;
}

} // namespace torrent
