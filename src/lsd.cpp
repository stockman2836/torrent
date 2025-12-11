#include "lsd.h"
#include "logger.h"
#include "utils.h"
#include <sstream>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define INVALID_SOCKET_FD INVALID_SOCKET
#define SOCKET_ERROR_CODE WSAGetLastError()
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#define INVALID_SOCKET_FD -1
#define SOCKET_ERROR_CODE errno
#define closesocket close
#endif

namespace torrent {

LSD::LSD(uint16_t listen_port, bool enable_ipv6)
    : listen_port_(listen_port)
    , enable_ipv6_(enable_ipv6)
    , socket_fd_(INVALID_SOCKET_FD)
    , running_(false) {
}

LSD::~LSD() {
    stop();
}

void LSD::start() {
    if (running_) {
        LOG_WARN("LSD already running");
        return;
    }

    LOG_INFO("Starting Local Service Discovery (LSD)...");

    // Create multicast socket
    if (!createMulticastSocket()) {
        LOG_ERROR("Failed to create LSD multicast socket");
        return;
    }

    // Join multicast group
    if (!joinMulticastGroup()) {
        LOG_ERROR("Failed to join LSD multicast group");
        closeSocket();
        return;
    }

    running_ = true;

    // Start worker threads
    announce_thread_ = std::thread(&LSD::announceLoop, this);
    listen_thread_ = std::thread(&LSD::listenLoop, this);

    LOG_INFO("LSD started successfully");
}

void LSD::stop() {
    if (!running_) {
        return;
    }

    LOG_INFO("Stopping LSD...");
    running_ = false;

    // Leave multicast group
    leaveMulticastGroup();

    // Close socket
    closeSocket();

    // Wait for threads to finish
    if (announce_thread_.joinable()) {
        announce_thread_.join();
    }
    if (listen_thread_.joinable()) {
        listen_thread_.join();
    }

    LOG_INFO("LSD stopped");
}

void LSD::announce(const std::vector<uint8_t>& info_hash) {
    std::lock_guard<std::mutex> lock(torrents_mutex_);

    // Check if already announcing
    auto it = std::find(announced_torrents_.begin(), announced_torrents_.end(), info_hash);
    if (it != announced_torrents_.end()) {
        return;  // Already announcing
    }

    announced_torrents_.push_back(info_hash);
    LOG_INFO("LSD: Started announcing torrent {}", utils::toHex(info_hash).substr(0, 8));

    // Send immediate announcement
    if (running_) {
        sendAnnouncement(info_hash);
    }
}

void LSD::stopAnnounce(const std::vector<uint8_t>& info_hash) {
    std::lock_guard<std::mutex> lock(torrents_mutex_);

    auto it = std::find(announced_torrents_.begin(), announced_torrents_.end(), info_hash);
    if (it != announced_torrents_.end()) {
        announced_torrents_.erase(it);
        LOG_INFO("LSD: Stopped announcing torrent {}", utils::toHex(info_hash).substr(0, 8));
    }
}

void LSD::setPeerCallback(LSDPeerCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    peer_callback_ = callback;
}

size_t LSD::getAnnouncedTorrentsCount() const {
    std::lock_guard<std::mutex> lock(torrents_mutex_);
    return announced_torrents_.size();
}

size_t LSD::getDiscoveredPeersCount() const {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    return discovered_peers_.size();
}

// ============================================================================
// Private Methods
// ============================================================================

void LSD::announceLoop() {
    LOG_DEBUG("LSD announce loop started");

    while (running_) {
        // Send announcements for all torrents
        {
            std::lock_guard<std::mutex> lock(torrents_mutex_);
            for (const auto& info_hash : announced_torrents_) {
                sendAnnouncement(info_hash);
            }
        }

        // Wait for interval (check every second for shutdown)
        for (int i = 0; i < ANNOUNCE_INTERVAL && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    LOG_DEBUG("LSD announce loop ended");
}

void LSD::listenLoop() {
    LOG_DEBUG("LSD listen loop started");

    char buffer[2048];

    while (running_) {
        struct sockaddr_storage sender_addr;
        socklen_t sender_len = sizeof(sender_addr);

        // Receive multicast message
        int received = recvfrom(socket_fd_, buffer, sizeof(buffer) - 1, 0,
                                (struct sockaddr*)&sender_addr, &sender_len);

        if (received < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAETIMEDOUT || WSAGetLastError() == WSAEWOULDBLOCK) {
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
#endif
                // Timeout or interrupted, continue
                continue;
            }

            LOG_ERROR("LSD: recvfrom error: {}", SOCKET_ERROR_CODE);
            break;
        }

        if (received == 0) {
            continue;
        }

        // Null-terminate message
        buffer[received] = '\0';
        std::string message(buffer, received);

        // Get sender IP and port
        std::string sender_ip;
        uint16_t sender_port = 0;

        if (sender_addr.ss_family == AF_INET) {
            struct sockaddr_in* addr4 = (struct sockaddr_in*)&sender_addr;
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &addr4->sin_addr, ip_str, INET_ADDRSTRLEN);
            sender_ip = ip_str;
            sender_port = ntohs(addr4->sin_port);
        } else if (sender_addr.ss_family == AF_INET6) {
            struct sockaddr_in6* addr6 = (struct sockaddr_in6*)&sender_addr;
            char ip_str[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &addr6->sin6_addr, ip_str, INET6_ADDRSTRLEN);
            sender_ip = ip_str;
            sender_port = ntohs(addr6->sin6_port);
        }

        // Parse message
        parseMessage(message, sender_ip, sender_port);
    }

    LOG_DEBUG("LSD listen loop ended");
}

void LSD::sendAnnouncement(const std::vector<uint8_t>& info_hash) {
    if (socket_fd_ == INVALID_SOCKET_FD) {
        return;
    }

    std::string message = buildAnnounceMessage(info_hash);

    // Send to multicast address
    struct sockaddr_in mcast_addr;
    memset(&mcast_addr, 0, sizeof(mcast_addr));
    mcast_addr.sin_family = AF_INET;
    mcast_addr.sin_port = htons(LSD_MULTICAST_PORT);
    inet_pton(AF_INET, LSD_MULTICAST_ADDR, &mcast_addr.sin_addr);

    int sent = sendto(socket_fd_, message.c_str(), message.length(), 0,
                      (struct sockaddr*)&mcast_addr, sizeof(mcast_addr));

    if (sent < 0) {
        LOG_ERROR("LSD: Failed to send announcement: {}", SOCKET_ERROR_CODE);
    } else {
        LOG_DEBUG("LSD: Sent announcement for {}", utils::toHex(info_hash).substr(0, 8));
    }
}

void LSD::parseMessage(const std::string& message, const std::string& sender_ip, uint16_t sender_port) {
    // Check if this is a valid LSD message
    if (message.find(LSD_COOKIE) != 0) {
        return;  // Not an LSD message
    }

    LOG_DEBUG("LSD: Received message from {}:{}", sender_ip, sender_port);

    // Parse message headers
    std::istringstream stream(message);
    std::string line;

    std::string info_hash_hex;
    uint16_t port = 0;

    while (std::getline(stream, line)) {
        // Remove \r if present
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        // Parse headers
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string header = line.substr(0, colon);
            std::string value = line.substr(colon + 1);

            // Trim whitespace
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);

            if (header == "Infohash") {
                info_hash_hex = value;
            } else if (header == "Port") {
                port = static_cast<uint16_t>(std::stoul(value));
            }
        }
    }

    // Validate and create peer
    if (info_hash_hex.empty() || port == 0) {
        LOG_DEBUG("LSD: Invalid message (missing infohash or port)");
        return;
    }

    // Convert hex info_hash to binary
    std::vector<uint8_t> info_hash = utils::fromHex(info_hash_hex);
    if (info_hash.size() != 20) {
        LOG_DEBUG("LSD: Invalid info_hash size: {}", info_hash.size());
        return;
    }

    // Check if we're interested in this torrent
    {
        std::lock_guard<std::mutex> lock(torrents_mutex_);
        auto it = std::find(announced_torrents_.begin(), announced_torrents_.end(), info_hash);
        if (it == announced_torrents_.end()) {
            // Not interested in this torrent
            return;
        }
    }

    // Create peer
    LSDPeer peer(sender_ip, port, info_hash);

    // Add to discovered peers
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        discovered_peers_.push_back(peer);
    }

    LOG_INFO("LSD: Discovered peer {}:{} for torrent {}",
             sender_ip, port, utils::toHex(info_hash).substr(0, 8));

    // Notify callback
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (peer_callback_) {
            peer_callback_({peer});
        }
    }
}

std::string LSD::buildAnnounceMessage(const std::vector<uint8_t>& info_hash) const {
    std::ostringstream msg;

    msg << LSD_COOKIE << "\r\n";
    msg << "Host: " << LSD_MULTICAST_ADDR << ":" << LSD_MULTICAST_PORT << "\r\n";
    msg << "Port: " << listen_port_ << "\r\n";
    msg << "Infohash: " << utils::toHex(info_hash) << "\r\n";
    msg << "\r\n";

    return msg.str();
}

// ============================================================================
// Socket Operations
// ============================================================================

bool LSD::createMulticastSocket() {
#ifdef _WIN32
    // Initialize Winsock
    static bool winsock_initialized = false;
    if (!winsock_initialized) {
        WSADATA wsa_data;
        if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
            LOG_ERROR("LSD: WSAStartup failed");
            return false;
        }
        winsock_initialized = true;
    }
#endif

    // Create UDP socket
    socket_fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd_ == INVALID_SOCKET_FD) {
        LOG_ERROR("LSD: Failed to create socket: {}", SOCKET_ERROR_CODE);
        return false;
    }

    // Allow multiple sockets to use the same port
    int reuse = 1;
    if (setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR,
                   (const char*)&reuse, sizeof(reuse)) < 0) {
        LOG_WARN("LSD: Failed to set SO_REUSEADDR: {}", SOCKET_ERROR_CODE);
    }

#ifdef SO_REUSEPORT
    if (setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEPORT,
                   (const char*)&reuse, sizeof(reuse)) < 0) {
        LOG_WARN("LSD: Failed to set SO_REUSEPORT: {}", SOCKET_ERROR_CODE);
    }
#endif

    // Bind to multicast port
    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = htons(LSD_MULTICAST_PORT);

    if (bind(socket_fd_, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
        LOG_ERROR("LSD: Failed to bind socket: {}", SOCKET_ERROR_CODE);
        closeSocket();
        return false;
    }

    // Set socket timeout for receive
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    if (setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO,
                   (const char*)&timeout, sizeof(timeout)) < 0) {
        LOG_WARN("LSD: Failed to set receive timeout: {}", SOCKET_ERROR_CODE);
    }

    LOG_DEBUG("LSD: Created multicast socket");
    return true;
}

void LSD::closeSocket() {
    if (socket_fd_ != INVALID_SOCKET_FD) {
        closesocket(socket_fd_);
        socket_fd_ = INVALID_SOCKET_FD;
        LOG_DEBUG("LSD: Closed socket");
    }
}

bool LSD::joinMulticastGroup() {
    if (socket_fd_ == INVALID_SOCKET_FD) {
        return false;
    }

    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));

    // Multicast address
    inet_pton(AF_INET, LSD_MULTICAST_ADDR, &mreq.imr_multiaddr);

    // Local interface (any)
    mreq.imr_interface.s_addr = INADDR_ANY;

    // Join multicast group
    if (setsockopt(socket_fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   (const char*)&mreq, sizeof(mreq)) < 0) {
        LOG_ERROR("LSD: Failed to join multicast group: {}", SOCKET_ERROR_CODE);
        return false;
    }

    LOG_INFO("LSD: Joined multicast group {}:{}", LSD_MULTICAST_ADDR, LSD_MULTICAST_PORT);
    return true;
}

bool LSD::leaveMulticastGroup() {
    if (socket_fd_ == INVALID_SOCKET_FD) {
        return false;
    }

    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));

    // Multicast address
    inet_pton(AF_INET, LSD_MULTICAST_ADDR, &mreq.imr_multiaddr);

    // Local interface (any)
    mreq.imr_interface.s_addr = INADDR_ANY;

    // Leave multicast group
    if (setsockopt(socket_fd_, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                   (const char*)&mreq, sizeof(mreq)) < 0) {
        LOG_WARN("LSD: Failed to leave multicast group: {}", SOCKET_ERROR_CODE);
        return false;
    }

    LOG_DEBUG("LSD: Left multicast group");
    return true;
}

} // namespace torrent
