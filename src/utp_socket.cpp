#include "utp_socket.h"
#include "logger.h"
#include <cstring>

#ifdef HAVE_LIBUTP
// Full implementation when libutp is available
#include <utp.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#endif

namespace torrent {
namespace utp {

// ============================================================================
// Helper Functions
// ============================================================================

static uint64 getCurrentTimestampMs() {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
    return ms.count();
}

// ============================================================================
// UtpSocket Implementation (with libutp)
// ============================================================================

UtpSocket::UtpSocket()
    : utp_socket_(nullptr)
    , bytes_sent_(0)
    , bytes_received_(0) {
}

UtpSocket::~UtpSocket() {
    close();
}

bool UtpSocket::connect(const std::string& host, uint16_t port) {
    if (!utp_socket_) {
        LOG_ERROR("uTP: Socket not initialized");
        return false;
    }

    LOG_INFO("uTP: Connecting to {}:{}", host, port);

    // Resolve hostname
    struct addrinfo hints, *result;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;  // IPv4
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) != 0) {
        LOG_ERROR("uTP: Failed to resolve host {}", host);
        return false;
    }

    // Connect using libutp
    struct sockaddr* addr = result->ai_addr;
    socklen_t addr_len = result->ai_addrlen;

    utp_connect(utp_socket_, addr, addr_len);

    freeaddrinfo(result);

    LOG_DEBUG("uTP: Connection initiated to {}:{}", host, port);
    return true;
}

bool UtpSocket::listen(uint16_t port) {
    LOG_INFO("uTP: Listening on port {} (passive mode)", port);
    // Listening is handled by UtpManager - sockets are created when connections arrive
    return true;
}

void UtpSocket::close() {
    if (utp_socket_) {
        utp_close(utp_socket_);
        utp_socket_ = nullptr;
        LOG_DEBUG("uTP: Socket closed");
    }
}

ssize_t UtpSocket::send(const uint8_t* data, size_t length) {
    if (!utp_socket_) {
        return -1;
    }

    ssize_t sent = utp_write(utp_socket_, (void*)data, length);
    if (sent > 0) {
        bytes_sent_ += sent;
    }

    return sent;
}

ssize_t UtpSocket::recv(uint8_t* buffer, size_t buffer_size) {
    // Data is delivered via onRead callback
    // This is a synchronous wrapper (not typically used with libutp)
    LOG_WARN("uTP: recv() called - use callbacks instead");
    return -1;
}

bool UtpSocket::isConnected() const {
    return utp_socket_ != nullptr && utp_get_state(utp_socket_) == UTP_STATE_CONNECT;
}

// Callbacks for libutp
uint64 UtpSocket::onRead(utp_callback_arguments* args) {
    UtpSocket* socket = static_cast<UtpSocket*>(utp_get_userdata(args->socket));

    if (socket && args->buf && args->len > 0) {
        socket->bytes_received_ += args->len;

        if (socket->on_data_) {
            socket->on_data_((const uint8_t*)args->buf, args->len);
        }

        LOG_DEBUG("uTP: Received {} bytes", args->len);
    }

    return 0;
}

uint64 UtpSocket::onWrite(utp_callback_arguments* args) {
    UtpSocket* socket = static_cast<UtpSocket*>(utp_get_userdata(args->socket));

    if (socket) {
        LOG_DEBUG("uTP: Write ready ({} bytes available)", args->len);
    }

    return 0;
}

uint64 UtpSocket::onState(utp_callback_arguments* args) {
    UtpSocket* socket = static_cast<UtpSocket*>(utp_get_userdata(args->socket));

    switch (args->state) {
    case UTP_STATE_CONNECT:
        LOG_INFO("uTP: Connection established");
        if (socket && socket->on_connect_) {
            socket->on_connect_();
        }
        break;

    case UTP_STATE_WRITABLE:
        LOG_DEBUG("uTP: Socket writable");
        break;

    case UTP_STATE_EOF:
        LOG_INFO("uTP: Connection closed (EOF)");
        if (socket) {
            socket->utp_socket_ = nullptr;
        }
        break;

    case UTP_STATE_DESTROYING:
        LOG_DEBUG("uTP: Socket destroying");
        if (socket) {
            socket->utp_socket_ = nullptr;
        }
        break;
    }

    return 0;
}

uint64 UtpSocket::onError(utp_callback_arguments* args) {
    UtpSocket* socket = static_cast<UtpSocket*>(utp_get_userdata(args->socket));

    LOG_ERROR("uTP: Error code {}", args->error_code);

    if (socket && socket->on_error_) {
        socket->on_error_(args->error_code);
    }

    return 0;
}

// ============================================================================
// UtpManager Implementation (with libutp)
// ============================================================================

// Callbacks for uTP context
static uint64 utpOnRead(utp_callback_arguments* args) {
    return UtpSocket::onRead(args);
}

static uint64 utpOnWrite(utp_callback_arguments* args) {
    return UtpSocket::onWrite(args);
}

static uint64 utpOnState(utp_callback_arguments* args) {
    return UtpSocket::onState(args);
}

static uint64 utpOnError(utp_callback_arguments* args) {
    return UtpSocket::onError(args);
}

static uint64 utpOnAccept(utp_callback_arguments* args) {
    LOG_INFO("uTP: Incoming connection accepted");

    // Set userdata for the new socket
    UtpSocket* socket = new UtpSocket();
    socket->utp_socket_ = args->socket;
    utp_set_userdata(args->socket, socket);

    return 0;
}

static uint64 utpSendTo(utp_callback_arguments* args) {
    // Send UDP packet
    UtpManager* manager = static_cast<UtpManager*>(utp_context_get_userdata(args->context));

    if (manager) {
        int sent = sendto(manager->udp_socket_,
                          (const char*)args->buf, args->len, 0,
                          args->address, args->address_len);

        if (sent < 0) {
            LOG_ERROR("uTP: sendto failed");
        }
    }

    return 0;
}

UtpManager::UtpManager(uint16_t port)
    : utp_context_(nullptr)
    , udp_socket_(-1)
    , port_(port)
    , total_bytes_sent_(0)
    , total_bytes_received_(0) {

    initializeLibutp();
}

UtpManager::~UtpManager() {
    shutdownLibutp();
}

void UtpManager::initializeLibutp() {
    LOG_INFO("uTP: Initializing libutp on port {}", port_);

#ifdef _WIN32
    // Initialize Winsock
    static bool winsock_initialized = false;
    if (!winsock_initialized) {
        WSADATA wsa_data;
        if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
            LOG_ERROR("uTP: WSAStartup failed");
            return;
        }
        winsock_initialized = true;
    }
#endif

    // Create UDP socket
    udp_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_socket_ < 0) {
        LOG_ERROR("uTP: Failed to create UDP socket");
        return;
    }

    // Bind to port
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(udp_socket_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("uTP: Failed to bind UDP socket to port {}", port_);
#ifdef _WIN32
        closesocket(udp_socket_);
#else
        ::close(udp_socket_);
#endif
        udp_socket_ = -1;
        return;
    }

    // Set non-blocking
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(udp_socket_, FIONBIO, &mode);
#else
    int flags = fcntl(udp_socket_, F_GETFL, 0);
    fcntl(udp_socket_, F_SETFL, flags | O_NONBLOCK);
#endif

    // Create uTP context
    utp_context_ = utp_init(2);  // Version 2
    if (!utp_context_) {
        LOG_ERROR("uTP: Failed to initialize libutp context");
        return;
    }

    // Set context userdata
    utp_context_set_userdata(utp_context_, this);

    // Set up callbacks
    utp_set_callback(utp_context_, UTP_ON_READ, &utpOnRead);
    utp_set_callback(utp_context_, UTP_ON_WRITE, &utpOnWrite);
    utp_set_callback(utp_context_, UTP_ON_STATE_CHANGE, &utpOnState);
    utp_set_callback(utp_context_, UTP_ON_ERROR, &utpOnError);
    utp_set_callback(utp_context_, UTP_ON_ACCEPT, &utpOnAccept);
    utp_set_callback(utp_context_, UTP_SENDTO, &utpSendTo);

    LOG_INFO("uTP: Initialized successfully on UDP port {}", port_);
}

void UtpManager::shutdownLibutp() {
    if (utp_context_) {
        stopListening();
        utp_destroy(utp_context_);
        utp_context_ = nullptr;
        LOG_INFO("uTP: Shutdown complete");
    }
}

std::shared_ptr<UtpSocket> UtpManager::createSocket() {
    if (!utp_context_) {
        LOG_ERROR("uTP: Context not initialized");
        return nullptr;
    }

    auto socket = std::make_shared<UtpSocket>();
    socket->utp_socket_ = utp_create_socket(utp_context_);

    if (!socket->utp_socket_) {
        LOG_ERROR("uTP: Failed to create socket");
        return nullptr;
    }

    utp_set_userdata(socket->utp_socket_, socket.get());

    LOG_DEBUG("uTP: Created new socket");
    return socket;
}

bool UtpManager::startListening(uint16_t port) {
    // Already listening via UDP socket created in initializeLibutp
    LOG_INFO("uTP: Listening on port {}", port);
    return udp_socket_ != -1;
}

void UtpManager::stopListening() {
    if (udp_socket_ != -1) {
#ifdef _WIN32
        closesocket(udp_socket_);
#else
        ::close(udp_socket_);
#endif
        udp_socket_ = -1;
        LOG_INFO("uTP: Stopped listening");
    }
}

void UtpManager::tick() {
    if (!utp_context_ || udp_socket_ == -1) {
        return;
    }

    // Receive UDP packets
    char buffer[4096];
    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);

    while (true) {
        int received = recvfrom(udp_socket_, buffer, sizeof(buffer), 0,
                               (struct sockaddr*)&addr, &addr_len);

        if (received < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEWOULDBLOCK) break;
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
#endif
            LOG_ERROR("uTP: recvfrom error");
            break;
        }

        if (received > 0) {
            // Process packet with libutp
            utp_process_udp(utp_context_, (const byte*)buffer, received,
                           (struct sockaddr*)&addr, addr_len);
        }
    }

    // Check for timeouts and retransmissions
    utp_check_timeouts(utp_context_);
}

bool UtpManager::isAvailable() {
    return UTP_AVAILABLE;
}

} // namespace utp
} // namespace torrent

#else
// ============================================================================
// Stub implementation when libutp is NOT available
// ============================================================================

namespace torrent {
namespace utp {

UtpSocket::UtpSocket()
    : utp_socket_(nullptr)
    , bytes_sent_(0)
    , bytes_received_(0) {
    LOG_WARN("uTP: libutp not available - uTP support disabled");
}

UtpSocket::~UtpSocket() {}

bool UtpSocket::connect(const std::string& host, uint16_t port) {
    LOG_WARN("uTP: Not available (libutp not compiled)");
    return false;
}

bool UtpSocket::listen(uint16_t port) {
    LOG_WARN("uTP: Not available (libutp not compiled)");
    return false;
}

void UtpSocket::close() {}

ssize_t UtpSocket::send(const uint8_t* data, size_t length) {
    return -1;
}

ssize_t UtpSocket::recv(uint8_t* buffer, size_t buffer_size) {
    return -1;
}

bool UtpSocket::isConnected() const {
    return false;
}

uint64_t UtpSocket::onRead(utp_callback_arguments* args) { return 0; }
uint64_t UtpSocket::onWrite(utp_callback_arguments* args) { return 0; }
uint64_t UtpSocket::onState(utp_callback_arguments* args) { return 0; }
uint64_t UtpSocket::onError(utp_callback_arguments* args) { return 0; }

// UtpManager stub
UtpManager::UtpManager(uint16_t port)
    : utp_context_(nullptr)
    , udp_socket_(-1)
    , port_(port)
    , total_bytes_sent_(0)
    , total_bytes_received_(0) {
    LOG_WARN("uTP: libutp not available - manager disabled");
}

UtpManager::~UtpManager() {}

void UtpManager::initializeLibutp() {}
void UtpManager::shutdownLibutp() {}

std::shared_ptr<UtpSocket> UtpManager::createSocket() {
    LOG_WARN("uTP: Not available (libutp not compiled)");
    return nullptr;
}

bool UtpManager::startListening(uint16_t port) {
    return false;
}

void UtpManager::stopListening() {}

void UtpManager::tick() {}

bool UtpManager::isAvailable() {
    return false;
}

} // namespace utp
} // namespace torrent

#endif // HAVE_LIBUTP
