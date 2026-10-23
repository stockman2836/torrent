#include "torrent/network/peer/peer_connection.h"
#include "torrent/metainfo/block.h"
#include "torrent/core/utils.h"
#include "torrent/core/logger.h"
#include "torrent/network/peer/extension_protocol.h"
#include "torrent/network/peer/mse_handshake.h"
#include "torrent/network/discovery/pex_manager.h"
#include <cstring>
#include <iostream>
#include <chrono>
#include <thread>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

// Helper to initialize Winsock
static bool winsock_initialized = false;
static void initWinsock() {
    if (!winsock_initialized) {
        WSADATA wsa_data;
        WSAStartup(MAKEWORD(2, 2), &wsa_data);
        winsock_initialized = true;
    }
}
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <errno.h>
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close
#endif


namespace torrent {

PeerConnection::PeerConnection(const std::string& ip,
                              uint16_t port,
                              const std::vector<uint8_t>& info_hash,
                              const std::string& peer_id)
    : ip_(ip)
    , port_(port)
    , info_hash_(info_hash)
    , peer_id_(peer_id)
    , remote_peer_id_()
    , transport_type_(TransportType::TCP)  // Default to TCP
    , socket_fd_(INVALID_SOCKET)
    , utp_socket_(nullptr)
    , connected_(false)
    , handshake_completed_(false)
    , am_choking_(false)  // Start unchoked - allow uploads
    , am_interested_(false)
    , peer_choking_(true)
    , peer_interested_(false)
    , is_ipv6_(utils::detectIPVersion(ip) == utils::IPVersion::IPv6)
    , supports_fast_extension_(true)
    , peer_supports_fast_extension_(false) {
}

PeerConnection::~PeerConnection() {
    disconnect();
}

bool PeerConnection::connect() {
#ifdef _WIN32
    initWinsock();
#endif

    if (connected_) {
        LOG_DEBUG("Already connected to peer {}:{}", ip_, port_);
        return true;
    }

    LOG_INFO("Connecting to peer {}:{} ({})", ip_, port_, is_ipv6_ ? "IPv6" : "IPv4");

    // Create socket with appropriate address family
    int address_family = is_ipv6_ ? AF_INET6 : AF_INET;
    socket_fd_ = socket(address_family, SOCK_STREAM, IPPROTO_TCP);
    if (socket_fd_ == INVALID_SOCKET) {
        LOG_ERROR("Failed to create socket for {}:{}", ip_, port_);
        return false;
    }

    // Prepare address structure
    struct sockaddr_storage server_addr_storage;
    memset(&server_addr_storage, 0, sizeof(server_addr_storage));

    struct sockaddr* server_addr_ptr;
    socklen_t server_addr_len;

    if (is_ipv6_) {
        // IPv6 address
        struct sockaddr_in6* addr6 = (struct sockaddr_in6*)&server_addr_storage;
        addr6->sin6_family = AF_INET6;
        addr6->sin6_port = htons(port_);

        // Convert IPv6 address
        if (inet_pton(AF_INET6, ip_.c_str(), &addr6->sin6_addr) <= 0) {
            LOG_ERROR("Invalid IPv6 address: {}", ip_);
            closesocket(socket_fd_);
            socket_fd_ = INVALID_SOCKET;
            return false;
        }

        server_addr_ptr = (struct sockaddr*)addr6;
        server_addr_len = sizeof(struct sockaddr_in6);
    } else {
        // IPv4 address
        struct sockaddr_in* addr4 = (struct sockaddr_in*)&server_addr_storage;
        addr4->sin_family = AF_INET;
        addr4->sin_port = htons(port_);

        // Convert IPv4 address
        if (inet_pton(AF_INET, ip_.c_str(), &addr4->sin_addr) <= 0) {
            LOG_ERROR("Invalid IPv4 address: {}", ip_);
            closesocket(socket_fd_);
            socket_fd_ = INVALID_SOCKET;
            return false;
        }

        server_addr_ptr = (struct sockaddr*)addr4;
        server_addr_len = sizeof(struct sockaddr_in);
    }

    // Set socket to non-blocking for timeout control
    setNonBlocking(true);

    // Attempt connection
    int result = ::connect(socket_fd_, server_addr_ptr, server_addr_len);

#ifdef _WIN32
    if (result == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
#else
    if (result == SOCKET_ERROR && errno != EINPROGRESS) {
#endif
        LOG_ERROR("Connection to {}:{} failed immediately", ip_, port_);
        closesocket(socket_fd_);
        socket_fd_ = INVALID_SOCKET;
        return false;
    }

    // Wait for connection with timeout (10 seconds)
    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(socket_fd_, &write_fds);

    struct timeval timeout;
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;

    result = select(socket_fd_ + 1, nullptr, &write_fds, nullptr, &timeout);

    if (result <= 0) {
        LOG_WARN("Connection to {}:{} timeout or error", ip_, port_);
        closesocket(socket_fd_);
        socket_fd_ = INVALID_SOCKET;
        return false;
    }

    // Check if connection succeeded
    int error = 0;
    socklen_t len = sizeof(error);
    if (getsockopt(socket_fd_, SOL_SOCKET, SO_ERROR, (char*)&error, &len) < 0 || error != 0) {
        LOG_ERROR("Connection to {}:{} failed with error code {}", ip_, port_, error);
        closesocket(socket_fd_);
        socket_fd_ = INVALID_SOCKET;
        return false;
    }

    // Set back to blocking mode
    setNonBlocking(false);

    // Set socket timeouts
    setSocketTimeout(30000); // 30 seconds

    connected_ = true;
    LOG_INFO("Successfully connected to peer {}:{}", ip_, port_);
    return true;
}

void PeerConnection::disconnect() {
    if (transport_type_ == TransportType::TCP && socket_fd_ != INVALID_SOCKET) {
        LOG_DEBUG("Disconnecting from peer {}:{} (TCP)", ip_, port_);
        closesocket(socket_fd_);
        socket_fd_ = INVALID_SOCKET;
    } else if (transport_type_ == TransportType::UTP && utp_socket_) {
        LOG_DEBUG("Disconnecting from peer {}:{} (uTP)", ip_, port_);
        utp_socket_->close();
        utp_socket_ = nullptr;
    }
    connected_ = false;
    handshake_completed_ = false;
    remote_peer_id_.clear();
    clearPendingRequests();
    clearPendingUploads();
}

void PeerConnection::setUtpSocket(std::shared_ptr<utp::UtpSocket> utp_socket) {
    if (!utp_socket) {
        LOG_ERROR("Attempted to set null uTP socket");
        return;
    }

    // Close existing TCP connection if any
    if (socket_fd_ != INVALID_SOCKET) {
        closesocket(socket_fd_);
        socket_fd_ = INVALID_SOCKET;
    }

    // Set uTP transport
    transport_type_ = TransportType::UTP;
    utp_socket_ = utp_socket;
    connected_ = utp_socket_->isConnected();

    LOG_INFO("Peer connection {}:{} now using uTP", ip_, port_);
}

bool PeerConnection::setNonBlocking(bool non_blocking) {
    if (socket_fd_ == INVALID_SOCKET) {
        return false;
    }

#ifdef _WIN32
    u_long mode = non_blocking ? 1 : 0;
    return ioctlsocket(socket_fd_, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(socket_fd_, F_GETFL, 0);
    if (flags == -1) {
        return false;
    }

    if (non_blocking) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }

    return fcntl(socket_fd_, F_SETFL, flags) == 0;
#endif
}

bool PeerConnection::setSocketTimeout(int timeout_ms) {
    if (socket_fd_ == INVALID_SOCKET) {
        return false;
    }

#ifdef _WIN32
    DWORD timeout = timeout_ms;
    if (setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) != 0) {
        return false;
    }
    if (setsockopt(socket_fd_, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout)) != 0) {
        return false;
    }
#else
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    if (setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        return false;
    }
    if (setsockopt(socket_fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
        return false;
    }
#endif

    return true;
}

bool PeerConnection::sendData(const void* data, size_t length) {
    if (!connected_) {
        std::cerr << "Cannot send: not connected\n";
        return false;
    }

    // Handle uTP transport
    if (transport_type_ == TransportType::UTP) {
        if (!utp_socket_ || !utp_socket_->isConnected()) {
            std::cerr << "Cannot send: uTP not connected\n";
            return false;
        }

        ssize_t sent = utp_socket_->send(static_cast<const uint8_t*>(data), length);
        if (sent < 0 || static_cast<size_t>(sent) != length) {
            std::cerr << "uTP send failed\n";
            return false;
        }
        return true;
    }

    // Handle TCP transport
    if (socket_fd_ == INVALID_SOCKET) {
        std::cerr << "Cannot send: TCP socket invalid\n";
        return false;
    }

    size_t total_sent = 0;
    const uint8_t* ptr = static_cast<const uint8_t*>(data);

    while (total_sent < length) {
        int sent = send(socket_fd_, (const char*)(ptr + total_sent), length - total_sent, 0);

        if (sent == SOCKET_ERROR) {
#ifdef _WIN32
            int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK || error == WSAEINTR) {
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
#endif
                // Retry
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            std::cerr << "Send failed\n";
            disconnect();
            return false;
        }

        if (sent == 0) {
            std::cerr << "Connection closed by peer\n";
            disconnect();
            return false;
        }

        total_sent += sent;
    }

    return true;
}

bool PeerConnection::receiveData(void* buffer, size_t length) {
    if (!connected_) {
        std::cerr << "Cannot receive: not connected\n";
        return false;
    }

    // Handle uTP transport
    // Note: uTP uses callbacks for receiving data. For now, synchronous receive
    // is not supported with uTP. Applications should use TCP for connections
    // that require synchronous I/O, or implement buffering in UtpSocket.
    if (transport_type_ == TransportType::UTP) {
        std::cerr << "Synchronous receive not supported with uTP (use callbacks)\n";
        return false;
    }

    // Handle TCP transport
    if (socket_fd_ == INVALID_SOCKET) {
        std::cerr << "Cannot receive: TCP socket invalid\n";
        return false;
    }

    size_t total_received = 0;
    uint8_t* ptr = static_cast<uint8_t*>(buffer);

    while (total_received < length) {
        int received = recv(socket_fd_, (char*)(ptr + total_received), length - total_received, 0);

        if (received == SOCKET_ERROR) {
#ifdef _WIN32
            int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK || error == WSAEINTR) {
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
#endif
                // Retry
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            std::cerr << "Receive failed\n";
            disconnect();
            return false;
        }

        if (received == 0) {
            std::cerr << "Connection closed by peer\n";
            disconnect();
            return false;
        }

        total_received += received;
    }

    return true;
}

bool PeerConnection::receiveDataWithTimeout(void* buffer, size_t length, int timeout_ms) {
    if (!connected_ || socket_fd_ == INVALID_SOCKET) {
        return false;
    }

    // Use select to wait for data with timeout
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(socket_fd_, &read_fds);

    struct timeval timeout;
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    int result = select(socket_fd_ + 1, &read_fds, nullptr, nullptr, &timeout);

    if (result <= 0) {
        return false; // Timeout or error
    }

    return receiveData(buffer, length);
}

} // namespace torrent
