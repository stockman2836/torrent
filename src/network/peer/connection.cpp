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
#include <algorithm>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

// Helper to initialize Winsock
static void initWinsock() {
    static const bool initialized = [] {
        WSADATA wsa_data;
        return WSAStartup(MAKEWORD(2, 2), &wsa_data) == 0;
    }();
    if (!initialized) throw std::runtime_error("WSAStartup failed");
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
namespace {
int socketError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}
bool retryable(int code) {
#ifdef _WIN32
    return code == WSAEWOULDBLOCK || code == WSAEINTR;
#else
    return code == EAGAIN || code == EWOULDBLOCK || code == EINTR;
#endif
}
} // namespace

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
    , am_choking_(true)  // BEP 3: uploads start choked until UNCHOKE is sent.
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
    if (connected_) return true;
    end_reason_ = PeerEndReason::None;
    error_code_ = 0;
    if (cancelled_ && cancelled_()) { setEndReason(PeerEndReason::Stopped); return false; }
    const auto fail = [this](PeerEndReason reason, int code = 0) {
        setEndReason(reason, code);
        if (socket_fd_ != INVALID_SOCKET) closesocket(socket_fd_);
        socket_fd_ = INVALID_SOCKET;
        return false;
    };
    socket_fd_ = socket(is_ipv6_ ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_fd_ == INVALID_SOCKET) return fail(PeerEndReason::ConnectFailed, socketError());
    sockaddr_storage storage{};
    sockaddr* address = reinterpret_cast<sockaddr*>(&storage);
    socklen_t length;
    if (is_ipv6_) {
        auto* value = reinterpret_cast<sockaddr_in6*>(&storage);
        value->sin6_family = AF_INET6; value->sin6_port = htons(port_);
        if (inet_pton(AF_INET6, ip_.c_str(), &value->sin6_addr) != 1) return fail(PeerEndReason::ConnectFailed);
        length = sizeof(*value);
    } else {
        auto* value = reinterpret_cast<sockaddr_in*>(&storage);
        value->sin_family = AF_INET; value->sin_port = htons(port_);
        if (inet_pton(AF_INET, ip_.c_str(), &value->sin_addr) != 1) return fail(PeerEndReason::ConnectFailed);
        length = sizeof(*value);
    }
    if (!setNonBlocking(true)) return fail(PeerEndReason::ConnectFailed, socketError());
    const auto connected = ::connect(socket_fd_, address, length);
    if (connected == SOCKET_ERROR) {
        const auto code = socketError();
#ifdef _WIN32
        if (code != WSAEWOULDBLOCK) return fail(PeerEndReason::ConnectFailed, code);
#else
        if (code != EINPROGRESS) return fail(PeerEndReason::ConnectFailed, code);
#endif
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        if (cancelled_ && cancelled_()) return fail(PeerEndReason::Stopped);
        if (std::chrono::steady_clock::now() >= deadline) return fail(PeerEndReason::ConnectTimeout);
        fd_set output; FD_ZERO(&output); FD_SET(socket_fd_, &output);
        fd_set errors = output; // Winsock reports refused nonblocking connects here.
        timeval timeout{0, 100000};
        const int ready = select(static_cast<int>(socket_fd_ + 1), nullptr, &output, &errors, &timeout);
        if (ready < 0) {
            const auto code = socketError();
            if (retryable(code)) continue;
            return fail(PeerEndReason::ConnectFailed, code);
        }
        if (!ready) continue;
        int error = 0; socklen_t size = sizeof(error);
        if (getsockopt(socket_fd_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size) < 0)
            return fail(PeerEndReason::ConnectFailed, socketError());
        if (error) return fail(PeerEndReason::ConnectFailed, error);
        break;
    }
    if (!setNonBlocking(false) || !setSocketTimeout(30000)) return fail(PeerEndReason::ConnectFailed, socketError());
    connected_ = true;
    return true;
}

void PeerConnection::disconnect() {
    if (transport_type_ == TransportType::TCP && socket_fd_ != INVALID_SOCKET) {
        LOG_DEBUG("Disconnecting from peer {}:{} (TCP)", ip_, port_);
        if (incoming_) {
            // Complete a bounded graceful shutdown so an exclusive listener can restart.
            shutdown(socket_fd_, 1); // SD_SEND / SHUT_WR.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
            bool drained = false;
            while (std::chrono::steady_clock::now() < deadline) {
                fd_set readable; FD_ZERO(&readable); FD_SET(socket_fd_, &readable);
                timeval delay{0, 20000};
                if (select(static_cast<int>(socket_fd_ + 1), &readable, nullptr, nullptr, &delay) <= 0) continue;
                char discard[4096];
                const auto received = recv(socket_fd_, discard, sizeof(discard), 0);
                if (received == 0) { drained = true; break; }
                if (received < 0) break;
            }
            if (!drained) {
                linger abortive{1, 0};
                setsockopt(socket_fd_, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&abortive), sizeof(abortive));
            }
        }
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

bool PeerConnection::sendData(const void* data, size_t length, bool cleanup) {
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
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    const uint8_t* ptr = static_cast<const uint8_t*>(data);

    while (total_sent < length) {
        if (!cleanup && cancelled_ && cancelled_()) { setEndReason(PeerEndReason::Stopped); return false; }
        if (std::chrono::steady_clock::now() >= deadline) {
            setEndReason(PeerEndReason::SendTimeout); disconnect(); return false;
        }
        int sent = send(socket_fd_, (const char*)(ptr + total_sent), length - total_sent, 0);

        if (sent == SOCKET_ERROR) {
#ifdef _WIN32
            int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK || error == WSAEINTR) {
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
#endif
                if (cancelled_ && cancelled_()) return false;
                // Retry
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            setEndReason(PeerEndReason::SendFailed, socketError());
            std::cerr << "Send failed\n";
            disconnect();
            return false;
        }

        if (sent == 0) {
            setEndReason(PeerEndReason::RemoteClosed);
            std::cerr << "Connection closed by peer\n";
            disconnect();
            return false;
        }

        total_sent += sent;
    }

    return true;
}

bool PeerConnection::receiveData(void* buffer, size_t length) {
    const auto fail = [this](PeerEndReason reason, int code = 0) {
        setEndReason(reason, code); disconnect(); return false;
    };
    if (!connected_ || socket_fd_ == INVALID_SOCKET || transport_type_ != TransportType::TCP)
        return fail(PeerEndReason::ReceiveFailed);
    size_t received_total = 0;
    auto* output = static_cast<uint8_t*>(buffer);
    const auto deadline = std::min(read_deadline_, std::chrono::steady_clock::now() + std::chrono::seconds(30));
    while (received_total < length) {
        if (cancelled_ && cancelled_()) return fail(PeerEndReason::Stopped);
        if (std::chrono::steady_clock::now() >= deadline)
            return fail(read_deadline_ == std::chrono::steady_clock::time_point::max() ?
                PeerEndReason::ReceiveTimeout : PeerEndReason::HandshakeTimeout);
        fd_set input; FD_ZERO(&input); FD_SET(socket_fd_, &input);
        timeval timeout{0, 100000};
        const int ready = select(static_cast<int>(socket_fd_ + 1), &input, nullptr, nullptr, &timeout);
        if (ready < 0) {
            const auto code = socketError();
            if (retryable(code)) continue;
            return fail(PeerEndReason::ReceiveFailed, code);
        }
        if (!ready) continue;
        const int received = recv(socket_fd_, reinterpret_cast<char*>(output + received_total), length - received_total, 0);
        if (received == SOCKET_ERROR) {
            const auto code = socketError();
            if (retryable(code)) continue;
            return fail(PeerEndReason::ReceiveFailed, code);
        }
        if (!received) return fail(PeerEndReason::RemoteClosed);
        received_total += received;
    }
    return true;
}

bool PeerConnection::waitForData(int timeout_ms) {
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

    if (result < 0 && !retryable(socketError())) {
        setEndReason(PeerEndReason::ReceiveFailed, socketError()); disconnect();
    }
    return result > 0;
}

bool PeerConnection::receiveDataWithTimeout(void* buffer, size_t length, int timeout_ms) {
    return waitForData(timeout_ms) && receiveData(buffer, length);
}

} // namespace torrent
