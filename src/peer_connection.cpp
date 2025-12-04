#include "peer_connection.h"
#include "utils.h"
#include <cstring>
#include <iostream>
#include <chrono>
#include <thread>

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
    , socket_fd_(INVALID_SOCKET)
    , connected_(false)
    , am_choking_(true)
    , am_interested_(false)
    , peer_choking_(true)
    , peer_interested_(false) {
}

PeerConnection::~PeerConnection() {
    disconnect();
}

bool PeerConnection::connect() {
#ifdef _WIN32
    initWinsock();
#endif

    if (connected_) {
        return true;
    }

    // Create socket
    socket_fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_fd_ == INVALID_SOCKET) {
        std::cerr << "Failed to create socket\n";
        return false;
    }

    // Prepare address structure
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_);

    // Convert IP address
    if (inet_pton(AF_INET, ip_.c_str(), &server_addr.sin_addr) <= 0) {
        std::cerr << "Invalid IP address: " << ip_ << "\n";
        closesocket(socket_fd_);
        socket_fd_ = INVALID_SOCKET;
        return false;
    }

    // Set socket to non-blocking for timeout control
    setNonBlocking(true);

    // Attempt connection
    int result = ::connect(socket_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr));

#ifdef _WIN32
    if (result == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
#else
    if (result == SOCKET_ERROR && errno != EINPROGRESS) {
#endif
        std::cerr << "Connection failed immediately\n";
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
        std::cerr << "Connection timeout or error\n";
        closesocket(socket_fd_);
        socket_fd_ = INVALID_SOCKET;
        return false;
    }

    // Check if connection succeeded
    int error = 0;
    socklen_t len = sizeof(error);
    if (getsockopt(socket_fd_, SOL_SOCKET, SO_ERROR, (char*)&error, &len) < 0 || error != 0) {
        std::cerr << "Connection failed: error code " << error << "\n";
        closesocket(socket_fd_);
        socket_fd_ = INVALID_SOCKET;
        return false;
    }

    // Set back to blocking mode
    setNonBlocking(false);

    // Set socket timeouts
    setSocketTimeout(30000); // 30 seconds

    connected_ = true;
    std::cout << "Connected to peer " << ip_ << ":" << port_ << "\n";
    return true;
}

void PeerConnection::disconnect() {
    if (socket_fd_ != INVALID_SOCKET) {
        closesocket(socket_fd_);
        socket_fd_ = INVALID_SOCKET;
    }
    connected_ = false;
}

bool PeerConnection::performHandshake() {
    if (!connected_) {
        std::cerr << "Cannot perform handshake: not connected\n";
        return false;
    }

    // BitTorrent handshake format:
    // <pstrlen><pstr><reserved><info_hash><peer_id>
    // pstrlen = 19
    // pstr = "BitTorrent protocol"
    // reserved = 8 bytes (all zeros)
    // info_hash = 20 bytes
    // peer_id = 20 bytes

    std::vector<uint8_t> handshake;
    handshake.push_back(19);

    std::string protocol = "BitTorrent protocol";
    handshake.insert(handshake.end(), protocol.begin(), protocol.end());

    // Reserved bytes
    for (int i = 0; i < 8; ++i) {
        handshake.push_back(0);
    }

    // Info hash
    handshake.insert(handshake.end(), info_hash_.begin(), info_hash_.end());

    // Peer ID
    handshake.insert(handshake.end(), peer_id_.begin(), peer_id_.end());

    // Send handshake
    if (!sendData(handshake.data(), handshake.size())) {
        std::cerr << "Failed to send handshake\n";
        return false;
    }

    // Receive handshake response (same format, 68 bytes total)
    std::vector<uint8_t> response(68);
    if (!receiveData(response.data(), response.size())) {
        std::cerr << "Failed to receive handshake response\n";
        return false;
    }

    // Validate response
    if (response[0] != 19) {
        std::cerr << "Invalid handshake: wrong protocol length\n";
        return false;
    }

    std::string received_protocol(response.begin() + 1, response.begin() + 20);
    if (received_protocol != "BitTorrent protocol") {
        std::cerr << "Invalid handshake: wrong protocol string\n";
        return false;
    }

    // Verify info_hash matches
    std::vector<uint8_t> received_info_hash(response.begin() + 28, response.begin() + 48);
    if (received_info_hash != info_hash_) {
        std::cerr << "Invalid handshake: info_hash mismatch\n";
        return false;
    }

    // Extract peer_id (bytes 48-68)
    std::vector<uint8_t> peer_id_response(response.begin() + 48, response.end());

    std::cout << "Handshake successful with peer\n";
    return true;
}

bool PeerConnection::sendMessage(const PeerMessage& message) {
    std::vector<uint8_t> data = serializeMessage(message);
    return sendData(data.data(), data.size());
}

std::vector<uint8_t> PeerConnection::serializeMessage(const PeerMessage& message) {
    std::vector<uint8_t> result;

    // Message format: <length><id><payload>
    uint32_t length = 1 + message.payload.size();

    // Length (big-endian)
    result.push_back((length >> 24) & 0xFF);
    result.push_back((length >> 16) & 0xFF);
    result.push_back((length >> 8) & 0xFF);
    result.push_back(length & 0xFF);

    // ID
    result.push_back(static_cast<uint8_t>(message.type));

    // Payload
    result.insert(result.end(), message.payload.begin(), message.payload.end());

    return result;
}

std::unique_ptr<PeerMessage> PeerConnection::receiveMessage() {
    if (!connected_) {
        return nullptr;
    }

    // Read message length (4 bytes, big-endian)
    uint8_t length_bytes[4];
    if (!receiveData(length_bytes, 4)) {
        return nullptr;
    }

    uint32_t message_length = (static_cast<uint32_t>(length_bytes[0]) << 24) |
                              (static_cast<uint32_t>(length_bytes[1]) << 16) |
                              (static_cast<uint32_t>(length_bytes[2]) << 8) |
                              static_cast<uint32_t>(length_bytes[3]);

    // Keep-alive message (length = 0)
    if (message_length == 0) {
        return std::make_unique<PeerMessage>();
    }

    // Sanity check on message length (max 256KB for piece messages)
    if (message_length > 262144) {
        std::cerr << "Invalid message length: " << message_length << "\n";
        disconnect();
        return nullptr;
    }

    // Read message ID (1 byte)
    uint8_t message_id;
    if (!receiveData(&message_id, 1)) {
        return nullptr;
    }

    // Read payload (if any)
    std::vector<uint8_t> payload;
    if (message_length > 1) {
        payload.resize(message_length - 1);
        if (!receiveData(payload.data(), payload.size())) {
            return nullptr;
        }
    }

    auto message = std::make_unique<PeerMessage>();
    message->type = static_cast<MessageType>(message_id);
    message->payload = std::move(payload);

    // Update peer state based on message type
    switch (message->type) {
        case MessageType::CHOKE:
            peer_choking_ = true;
            break;
        case MessageType::UNCHOKE:
            peer_choking_ = false;
            break;
        case MessageType::INTERESTED:
            peer_interested_ = true;
            break;
        case MessageType::NOT_INTERESTED:
            peer_interested_ = false;
            break;
        default:
            break;
    }

    return message;
}

bool PeerConnection::sendKeepAlive() {
    // Keep-alive: <len=0000>
    uint8_t keep_alive[4] = {0, 0, 0, 0};
    return sendData(keep_alive, 4);
}

bool PeerConnection::sendChoke() {
    PeerMessage msg{MessageType::CHOKE, {}};
    am_choking_ = true;
    return sendMessage(msg);
}

bool PeerConnection::sendUnchoke() {
    PeerMessage msg{MessageType::UNCHOKE, {}};
    am_choking_ = false;
    return sendMessage(msg);
}

bool PeerConnection::sendInterested() {
    PeerMessage msg{MessageType::INTERESTED, {}};
    am_interested_ = true;
    return sendMessage(msg);
}

bool PeerConnection::sendNotInterested() {
    PeerMessage msg{MessageType::NOT_INTERESTED, {}};
    am_interested_ = false;
    return sendMessage(msg);
}

bool PeerConnection::sendHave(uint32_t piece_index) {
    std::vector<uint8_t> payload(4);
    payload[0] = (piece_index >> 24) & 0xFF;
    payload[1] = (piece_index >> 16) & 0xFF;
    payload[2] = (piece_index >> 8) & 0xFF;
    payload[3] = piece_index & 0xFF;

    PeerMessage msg{MessageType::HAVE, payload};
    return sendMessage(msg);
}

bool PeerConnection::sendBitfield(const std::vector<bool>& bitfield) {
    std::vector<uint8_t> payload((bitfield.size() + 7) / 8, 0);

    for (size_t i = 0; i < bitfield.size(); ++i) {
        if (bitfield[i]) {
            payload[i / 8] |= (1 << (7 - (i % 8)));
        }
    }

    PeerMessage msg{MessageType::BITFIELD, payload};
    return sendMessage(msg);
}

bool PeerConnection::sendRequest(uint32_t piece_index, uint32_t offset, uint32_t length) {
    std::vector<uint8_t> payload(12);

    // Piece index
    payload[0] = (piece_index >> 24) & 0xFF;
    payload[1] = (piece_index >> 16) & 0xFF;
    payload[2] = (piece_index >> 8) & 0xFF;
    payload[3] = piece_index & 0xFF;

    // Offset
    payload[4] = (offset >> 24) & 0xFF;
    payload[5] = (offset >> 16) & 0xFF;
    payload[6] = (offset >> 8) & 0xFF;
    payload[7] = offset & 0xFF;

    // Length
    payload[8] = (length >> 24) & 0xFF;
    payload[9] = (length >> 16) & 0xFF;
    payload[10] = (length >> 8) & 0xFF;
    payload[11] = length & 0xFF;

    PeerMessage msg{MessageType::REQUEST, payload};
    return sendMessage(msg);
}

bool PeerConnection::sendPiece(uint32_t piece_index, uint32_t offset, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> payload(8 + data.size());

    // Piece index
    payload[0] = (piece_index >> 24) & 0xFF;
    payload[1] = (piece_index >> 16) & 0xFF;
    payload[2] = (piece_index >> 8) & 0xFF;
    payload[3] = piece_index & 0xFF;

    // Offset
    payload[4] = (offset >> 24) & 0xFF;
    payload[5] = (offset >> 16) & 0xFF;
    payload[6] = (offset >> 8) & 0xFF;
    payload[7] = offset & 0xFF;

    // Data
    std::copy(data.begin(), data.end(), payload.begin() + 8);

    PeerMessage msg{MessageType::PIECE, payload};
    return sendMessage(msg);
}

bool PeerConnection::sendCancel(uint32_t piece_index, uint32_t offset, uint32_t length) {
    std::vector<uint8_t> payload(12);

    // Same format as REQUEST
    payload[0] = (piece_index >> 24) & 0xFF;
    payload[1] = (piece_index >> 16) & 0xFF;
    payload[2] = (piece_index >> 8) & 0xFF;
    payload[3] = piece_index & 0xFF;

    payload[4] = (offset >> 24) & 0xFF;
    payload[5] = (offset >> 16) & 0xFF;
    payload[6] = (offset >> 8) & 0xFF;
    payload[7] = offset & 0xFF;

    payload[8] = (length >> 24) & 0xFF;
    payload[9] = (length >> 16) & 0xFF;
    payload[10] = (length >> 8) & 0xFF;
    payload[11] = length & 0xFF;

    PeerMessage msg{MessageType::CANCEL, payload};
    return sendMessage(msg);
}

// Helper functions implementation

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
    if (!connected_ || socket_fd_ == INVALID_SOCKET) {
        std::cerr << "Cannot send: not connected\n";
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
    if (!connected_ || socket_fd_ == INVALID_SOCKET) {
        std::cerr << "Cannot receive: not connected\n";
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
