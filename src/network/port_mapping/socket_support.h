#pragma once

#include "gateway_client.h"
#include <array>
#include <chrono>
#include <cstring>
#include <stdexcept>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace torrent::mapping {

struct Socket {
#ifdef _WIN32
    SOCKET value = INVALID_SOCKET;
#else
    int value = -1;
#endif
    Socket() {
#ifdef _WIN32
        static const bool ready = [] { WSADATA data; return WSAStartup(MAKEWORD(2, 2), &data) == 0; }();
        if (!ready) throw std::runtime_error("Cannot initialize router discovery sockets");
#endif
        value = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
        if (value == INVALID_SOCKET) throw std::runtime_error("Cannot create router discovery socket");
#else
        if (value < 0 || value >= FD_SETSIZE) {
            if (value >= 0) ::close(value);
            throw std::runtime_error("Cannot create router discovery socket");
        }
#endif
    }
    ~Socket() {
#ifdef _WIN32
        closesocket(value);
#else
        ::close(value);
#endif
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
};

inline sockaddr_in endpoint(const std::string& ip, uint16_t port) {
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) throw std::invalid_argument("Router address must be numeric IPv4");
    return address;
}

inline std::string ipv4(const in_addr& address) {
    char result[INET_ADDRSTRLEN]{};
    if (!inet_ntop(AF_INET, &address, result, sizeof(result))) throw std::runtime_error("Cannot read router address");
    return result;
}

inline bool readable(const Socket& socket, int milliseconds = 50) {
    fd_set input; FD_ZERO(&input); FD_SET(socket.value, &input);
    timeval timeout{milliseconds / 1000, (milliseconds % 1000) * 1000};
    const auto result = select(static_cast<int>(socket.value + 1), &input, nullptr, nullptr, &timeout);
    if (result < 0) throw std::runtime_error("Router socket polling failed");
    return result > 0;
}

} // namespace torrent::mapping
