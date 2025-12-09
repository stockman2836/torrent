#include "udp_tracker.h"
#include "logger.h"
#include <cstring>
#include <sstream>
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
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

// Helper functions for network byte order conversion
static void writeInt64(std::vector<uint8_t>& buffer, int64_t value) {
    buffer.push_back((value >> 56) & 0xFF);
    buffer.push_back((value >> 48) & 0xFF);
    buffer.push_back((value >> 40) & 0xFF);
    buffer.push_back((value >> 32) & 0xFF);
    buffer.push_back((value >> 24) & 0xFF);
    buffer.push_back((value >> 16) & 0xFF);
    buffer.push_back((value >> 8) & 0xFF);
    buffer.push_back(value & 0xFF);
}

static void writeInt32(std::vector<uint8_t>& buffer, int32_t value) {
    buffer.push_back((value >> 24) & 0xFF);
    buffer.push_back((value >> 16) & 0xFF);
    buffer.push_back((value >> 8) & 0xFF);
    buffer.push_back(value & 0xFF);
}

static void writeUInt32(std::vector<uint8_t>& buffer, uint32_t value) {
    buffer.push_back((value >> 24) & 0xFF);
    buffer.push_back((value >> 16) & 0xFF);
    buffer.push_back((value >> 8) & 0xFF);
    buffer.push_back(value & 0xFF);
}

static void writeUInt16(std::vector<uint8_t>& buffer, uint16_t value) {
    buffer.push_back((value >> 8) & 0xFF);
    buffer.push_back(value & 0xFF);
}

static int64_t readInt64(const uint8_t* data) {
    return (static_cast<int64_t>(data[0]) << 56) |
           (static_cast<int64_t>(data[1]) << 48) |
           (static_cast<int64_t>(data[2]) << 40) |
           (static_cast<int64_t>(data[3]) << 32) |
           (static_cast<int64_t>(data[4]) << 24) |
           (static_cast<int64_t>(data[5]) << 16) |
           (static_cast<int64_t>(data[6]) << 8) |
           static_cast<int64_t>(data[7]);
}

static int32_t readInt32(const uint8_t* data) {
    return (static_cast<int32_t>(data[0]) << 24) |
           (static_cast<int32_t>(data[1]) << 16) |
           (static_cast<int32_t>(data[2]) << 8) |
           static_cast<int32_t>(data[3]);
}

static uint32_t readUInt32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

static uint16_t readUInt16(const uint8_t* data) {
    return (static_cast<uint16_t>(data[0]) << 8) |
           static_cast<uint16_t>(data[1]);
}

// ==================== Connect Request/Response ====================

std::vector<uint8_t> UDPConnectRequest::serialize() const {
    std::vector<uint8_t> buffer;
    buffer.reserve(16);
    writeInt64(buffer, protocol_id);
    writeInt32(buffer, action);
    writeInt32(buffer, transaction_id);
    return buffer;
}

UDPConnectResponse UDPConnectResponse::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 16) {
        throw std::runtime_error("Invalid UDP connect response size");
    }

    UDPConnectResponse response;
    response.action = readInt32(&data[0]);
    response.transaction_id = readInt32(&data[4]);
    response.connection_id = readInt64(&data[8]);

    return response;
}

// ==================== Announce Request/Response ====================

UDPAnnounceRequest::UDPAnnounceRequest(int64_t conn_id,
                                       int32_t tid,
                                       const std::vector<uint8_t>& hash,
                                       const std::string& peer_id_str,
                                       int64_t dl,
                                       int64_t lft,
                                       int64_t ul,
                                       UDPEvent evt,
                                       uint16_t prt)
    : connection_id(conn_id)
    , action(UDP_ACTION_ANNOUNCE)
    , transaction_id(tid)
    , info_hash(hash)
    , downloaded(dl)
    , left(lft)
    , uploaded(ul)
    , event(static_cast<int32_t>(evt))
    , ip(0)  // 0 = use default
    , key(std::random_device{}())
    , num_want(-1)  // -1 = default
    , port(prt) {

    // Convert peer_id string to bytes
    peer_id.assign(peer_id_str.begin(), peer_id_str.end());
    peer_id.resize(20, 0);  // Ensure 20 bytes
}

std::vector<uint8_t> UDPAnnounceRequest::serialize() const {
    std::vector<uint8_t> buffer;
    buffer.reserve(98);

    writeInt64(buffer, connection_id);
    writeInt32(buffer, action);
    writeInt32(buffer, transaction_id);

    // info_hash (20 bytes)
    for (size_t i = 0; i < 20; ++i) {
        buffer.push_back(i < info_hash.size() ? info_hash[i] : 0);
    }

    // peer_id (20 bytes)
    for (size_t i = 0; i < 20; ++i) {
        buffer.push_back(i < peer_id.size() ? peer_id[i] : 0);
    }

    writeInt64(buffer, downloaded);
    writeInt64(buffer, left);
    writeInt64(buffer, uploaded);
    writeInt32(buffer, event);
    writeUInt32(buffer, ip);
    writeUInt32(buffer, key);
    writeInt32(buffer, num_want);
    writeUInt16(buffer, port);

    return buffer;
}

UDPAnnounceResponse UDPAnnounceResponse::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 20) {
        throw std::runtime_error("Invalid UDP announce response size");
    }

    UDPAnnounceResponse response;
    response.action = readInt32(&data[0]);
    response.transaction_id = readInt32(&data[4]);
    response.interval = readInt32(&data[8]);
    response.leechers = readInt32(&data[12]);
    response.seeders = readInt32(&data[16]);

    // Parse peers (6 bytes each: 4 bytes IP + 2 bytes port)
    size_t offset = 20;
    while (offset + 6 <= data.size()) {
        uint32_t ip = readUInt32(&data[offset]);
        uint16_t port = readUInt16(&data[offset + 4]);

        std::ostringstream ip_str;
        ip_str << ((ip >> 24) & 0xFF) << "."
               << ((ip >> 16) & 0xFF) << "."
               << ((ip >> 8) & 0xFF) << "."
               << (ip & 0xFF);

        response.peers.emplace_back(ip_str.str(), port);
        offset += 6;
    }

    return response;
}

// ==================== Error Response ====================

UDPErrorResponse UDPErrorResponse::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 8) {
        throw std::runtime_error("Invalid UDP error response size");
    }

    UDPErrorResponse response;
    response.action = readInt32(&data[0]);
    response.transaction_id = readInt32(&data[4]);

    if (data.size() > 8) {
        response.message = std::string(data.begin() + 8, data.end());
    }

    return response;
}

// ==================== UDP Tracker Client ====================

UDPTrackerClient::UDPTrackerClient(const std::string& tracker_url,
                                   const std::vector<uint8_t>& info_hash,
                                   const std::string& peer_id)
    : tracker_url_(tracker_url)
    , info_hash_(info_hash)
    , peer_id_(peer_id)
    , socket_fd_(INVALID_SOCKET)
    , connection_id_(0)
    , connected_(false)
    , rng_(rd_()) {

#ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif
}

UDPTrackerClient::~UDPTrackerClient() {
    if (socket_fd_ != INVALID_SOCKET) {
        closesocket(socket_fd_);
    }
}

bool UDPTrackerClient::parseURL(const std::string& url, std::string& host, uint16_t& port) {
    // Expected format: udp://tracker.example.com:8080/announce
    if (url.substr(0, 6) != "udp://") {
        LOG_ERROR("Invalid UDP tracker URL: {}", url);
        return false;
    }

    size_t start = 6;  // After "udp://"
    size_t end = url.find(':', start);
    if (end == std::string::npos) {
        LOG_ERROR("No port found in UDP tracker URL: {}", url);
        return false;
    }

    host = url.substr(start, end - start);

    size_t port_end = url.find('/', end);
    if (port_end == std::string::npos) {
        port_end = url.length();
    }

    try {
        port = static_cast<uint16_t>(std::stoi(url.substr(end + 1, port_end - end - 1)));
    } catch (...) {
        LOG_ERROR("Invalid port in UDP tracker URL: {}", url);
        return false;
    }

    LOG_DEBUG("Parsed UDP tracker URL: host={}, port={}", host, port);
    return true;
}

int32_t UDPTrackerClient::generateTransactionId() {
    std::uniform_int_distribution<int32_t> dist;
    return dist(rng_);
}

bool UDPTrackerClient::sendRequest(const std::vector<uint8_t>& request,
                                  std::vector<uint8_t>& response,
                                  int timeout_seconds) {
    // Parse tracker URL
    std::string host;
    uint16_t port;
    if (!parseURL(tracker_url_, host, port)) {
        return false;
    }

    // Resolve hostname
    struct addrinfo hints, *result;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) != 0) {
        LOG_ERROR("Failed to resolve UDP tracker host: {}", host);
        return false;
    }

    // Create UDP socket
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        LOG_ERROR("Failed to create UDP socket");
        freeaddrinfo(result);
        return false;
    }

    // Set socket timeout
    struct timeval tv;
    tv.tv_sec = timeout_seconds;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    // Send request
    ssize_t sent = sendto(sock, reinterpret_cast<const char*>(request.data()),
                         request.size(), 0, result->ai_addr, result->ai_addrlen);

    if (sent != static_cast<ssize_t>(request.size())) {
        LOG_ERROR("Failed to send UDP request");
        closesocket(sock);
        freeaddrinfo(result);
        return false;
    }

    LOG_DEBUG("Sent UDP request: {} bytes", sent);

    // Receive response
    uint8_t buffer[2048];
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);

    ssize_t received = recvfrom(sock, reinterpret_cast<char*>(buffer), sizeof(buffer),
                               0, (struct sockaddr*)&from_addr, &from_len);

    closesocket(sock);
    freeaddrinfo(result);

    if (received <= 0) {
        LOG_WARN("UDP receive timeout or error");
        return false;
    }

    LOG_DEBUG("Received UDP response: {} bytes", received);
    response.assign(buffer, buffer + received);
    return true;
}

bool UDPTrackerClient::connect() {
    int32_t transaction_id = generateTransactionId();
    UDPConnectRequest request(transaction_id);

    std::vector<uint8_t> response_data;
    if (!sendRequest(request.serialize(), response_data, 15)) {
        LOG_ERROR("UDP tracker connect failed");
        return false;
    }

    try {
        UDPConnectResponse response = UDPConnectResponse::deserialize(response_data);

        if (response.transaction_id != transaction_id) {
            LOG_ERROR("UDP tracker connect: transaction ID mismatch");
            return false;
        }

        connection_id_ = response.connection_id;
        connected_ = true;

        LOG_INFO("UDP tracker connected. Connection ID: {}", connection_id_);
        return true;

    } catch (const std::exception& e) {
        LOG_ERROR("Failed to parse UDP connect response: {}", e.what());
        return false;
    }
}

TrackerResponse UDPTrackerClient::announce(int64_t uploaded,
                                          int64_t downloaded,
                                          int64_t left,
                                          uint16_t port,
                                          const std::string& event) {
    TrackerResponse result;
    result.interval = 1800;
    result.complete = 0;
    result.incomplete = 0;

    LOG_INFO("UDP tracker announce: uploaded={}, downloaded={}, left={}, port={}, event={}",
             uploaded, downloaded, left, port, event);

    // Connect if not connected
    if (!connected_) {
        if (!connect()) {
            result.failure_reason = "Failed to connect to UDP tracker";
            return result;
        }
    }

    // Map event string to UDPEvent
    UDPEvent udp_event = UDPEvent::NONE;
    if (event == "started") udp_event = UDPEvent::STARTED;
    else if (event == "completed") udp_event = UDPEvent::COMPLETED;
    else if (event == "stopped") udp_event = UDPEvent::STOPPED;

    // Create announce request
    int32_t transaction_id = generateTransactionId();
    UDPAnnounceRequest request(connection_id_, transaction_id, info_hash_,
                              peer_id_, downloaded, left, uploaded,
                              udp_event, port);

    std::vector<uint8_t> response_data;
    if (!sendRequest(request.serialize(), response_data, 15)) {
        result.failure_reason = "UDP tracker announce failed";
        LOG_ERROR("UDP tracker announce failed");
        return result;
    }

    try {
        // Check if it's an error response
        if (response_data.size() >= 8) {
            int32_t action = readInt32(&response_data[0]);

            if (action == UDP_ACTION_ERROR) {
                UDPErrorResponse error_resp = UDPErrorResponse::deserialize(response_data);
                result.failure_reason = "Tracker error: " + error_resp.message;
                LOG_ERROR("UDP tracker error: {}", error_resp.message);
                return result;
            }
        }

        UDPAnnounceResponse response = UDPAnnounceResponse::deserialize(response_data);

        if (response.transaction_id != transaction_id) {
            result.failure_reason = "Transaction ID mismatch";
            LOG_ERROR("UDP tracker announce: transaction ID mismatch");
            return result;
        }

        result.interval = response.interval;
        result.complete = response.seeders;
        result.incomplete = response.leechers;
        result.peers = response.peers;

        LOG_INFO("UDP tracker announce successful: interval={}, seeders={}, leechers={}, peers={}",
                 result.interval, result.complete, result.incomplete, result.peers.size());

        return result;

    } catch (const std::exception& e) {
        result.failure_reason = std::string("Failed to parse UDP announce response: ") + e.what();
        LOG_ERROR("Failed to parse UDP announce response: {}", e.what());
        return result;
    }
}

} // namespace torrent
