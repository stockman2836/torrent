#include "peer_connection.h"
#include "piece_manager.h"
#include "utils.h"
#include "logger.h"
#include "extension_protocol.h"
#include "mse_handshake.h"
#include "pex_manager.h"
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
    , socket_fd_(INVALID_SOCKET)
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
    if (socket_fd_ != INVALID_SOCKET) {
        LOG_DEBUG("Disconnecting from peer {}:{}", ip_, port_);
        closesocket(socket_fd_);
        socket_fd_ = INVALID_SOCKET;
    }
    connected_ = false;
    handshake_completed_ = false;
    remote_peer_id_.clear();
    clearPendingRequests();
    clearPendingUploads();
}

bool PeerConnection::performHandshake() {
    if (!connected_) {
        LOG_ERROR("Cannot perform handshake with {}:{} - not connected", ip_, port_);
        return false;
    }

    if (handshake_completed_) {
        LOG_WARN("Handshake with {}:{} already completed", ip_, port_);
        return true;
    }

    std::cout << "Performing BitTorrent handshake with " << ip_ << ":" << port_ << "...\n";

    // BitTorrent handshake format:
    // <pstrlen><pstr><reserved><info_hash><peer_id>
    // pstrlen = 19 (1 byte)
    // pstr = "BitTorrent protocol" (19 bytes)
    // reserved = 8 bytes (all zeros for basic protocol)
    // info_hash = 20 bytes
    // peer_id = 20 bytes
    // Total: 68 bytes

    // Build handshake message
    std::vector<uint8_t> handshake;
    handshake.reserve(68);

    // Protocol string length
    handshake.push_back(19);

    // Protocol string
    std::string protocol = "BitTorrent protocol";
    handshake.insert(handshake.end(), protocol.begin(), protocol.end());

    // Reserved bytes (8 bytes)
    // Byte 7, bit 0x04: Fast Extension (BEP 6)
    // Byte 5, bit 0x10: Extension Protocol (BEP 10)
    std::vector<uint8_t> reserved(8, 0);
    if (supports_fast_extension_) {
        reserved[7] |= 0x04;  // Fast Extension support
    }
    reserved[5] |= 0x10;  // Extension Protocol support
    handshake.insert(handshake.end(), reserved.begin(), reserved.end());

    // Info hash (20 bytes)
    if (info_hash_.size() != 20) {
        std::cerr << "ERROR: Invalid info_hash size: " << info_hash_.size() << " (expected 20)\n";
        return false;
    }
    handshake.insert(handshake.end(), info_hash_.begin(), info_hash_.end());

    // Peer ID (20 bytes)
    if (peer_id_.size() != 20) {
        std::cerr << "ERROR: Invalid peer_id size: " << peer_id_.size() << " (expected 20)\n";
        return false;
    }
    handshake.insert(handshake.end(), peer_id_.begin(), peer_id_.end());

    // Sanity check
    if (handshake.size() != 68) {
        std::cerr << "ERROR: Invalid handshake size: " << handshake.size() << " (expected 68)\n";
        return false;
    }

    // Send handshake
    std::cout << "  Sending handshake (" << handshake.size() << " bytes)...\n";
    if (!sendData(handshake.data(), handshake.size())) {
        std::cerr << "ERROR: Failed to send handshake\n";
        disconnect();
        return false;
    }

    // Receive handshake response (same format, 68 bytes total)
    std::cout << "  Waiting for handshake response...\n";
    std::vector<uint8_t> response(68);
    if (!receiveData(response.data(), response.size())) {
        std::cerr << "ERROR: Failed to receive handshake response\n";
        disconnect();
        return false;
    }

    // Validate response - protocol string length
    if (response[0] != 19) {
        std::cerr << "ERROR: Invalid handshake response - wrong protocol length: "
                  << static_cast<int>(response[0]) << " (expected 19)\n";
        disconnect();
        return false;
    }

    // Validate response - protocol string
    std::string received_protocol(response.begin() + 1, response.begin() + 20);
    if (received_protocol != "BitTorrent protocol") {
        std::cerr << "ERROR: Invalid handshake response - wrong protocol string: '"
                  << received_protocol << "'\n";
        disconnect();
        return false;
    }

    // Extract reserved bytes (bytes 20-27) and check for extension support
    std::vector<uint8_t> reserved_bytes(response.begin() + 20, response.begin() + 28);

    // Check if peer supports Fast Extension (BEP 6)
    // Byte 7, bit 0x04
    if (reserved_bytes[7] & 0x04) {
        peer_supports_fast_extension_ = true;
        LOG_INFO("Peer supports Fast Extension (BEP 6)");
    }

    // Check if peer supports Extension Protocol (BEP 10)
    if (reserved_bytes[5] & 0x10) {
        LOG_DEBUG("Peer supports Extension Protocol (BEP 10)");
    }

    // Verify info_hash matches (bytes 28-47)
    std::vector<uint8_t> received_info_hash(response.begin() + 28, response.begin() + 48);
    if (received_info_hash != info_hash_) {
        std::cerr << "ERROR: Invalid handshake response - info_hash mismatch\n";
        std::cerr << "  Expected: ";
        for (size_t i = 0; i < std::min(info_hash_.size(), size_t(8)); ++i) {
            std::cerr << std::hex << static_cast<int>(info_hash_[i]) << " ";
        }
        std::cerr << "...\n  Received: ";
        for (size_t i = 0; i < std::min(received_info_hash.size(), size_t(8)); ++i) {
            std::cerr << std::hex << static_cast<int>(received_info_hash[i]) << " ";
        }
        std::cerr << std::dec << "...\n";
        disconnect();
        return false;
    }

    // Extract peer_id (bytes 48-67)
    remote_peer_id_.assign(response.begin() + 48, response.end());

    // Validate peer_id size
    if (remote_peer_id_.size() != 20) {
        std::cerr << "ERROR: Invalid peer_id size in response: "
                  << remote_peer_id_.size() << " (expected 20)\n";
        disconnect();
        return false;
    }

    // Mark handshake as completed
    handshake_completed_ = true;

    std::cout << "  [SUCCESS] Handshake completed!\n";
    std::cout << "  Remote Peer ID: ";
    for (size_t i = 0; i < std::min(remote_peer_id_.size(), size_t(20)); ++i) {
        char c = remote_peer_id_[i];
        if (c >= 32 && c <= 126) {
            std::cout << c;
        } else {
            std::cout << '.';
        }
    }
    std::cout << "\n";

    return true;
}

bool PeerConnection::performHandshake(const std::vector<bool>& our_bitfield) {
    // First, perform the standard handshake
    if (!performHandshake()) {
        return false;
    }

    // After successful handshake, send our bitfield if we have any pieces
    bool has_any_piece = false;
    for (bool piece : our_bitfield) {
        if (piece) {
            has_any_piece = true;
            break;
        }
    }

    if (has_any_piece) {
        std::cout << "Sending our bitfield to peer...\n";
        if (!sendBitfield(our_bitfield)) {
            std::cerr << "WARNING: Failed to send bitfield, but handshake succeeded\n";
            // Don't fail the handshake just because bitfield send failed
        } else {
            std::cout << "Successfully sent bitfield to peer\n";
        }
    } else {
        std::cout << "Not sending bitfield (no pieces downloaded yet)\n";
    }

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
        auto message = std::make_unique<PeerMessage>();
        message->type = MessageType::KEEP_ALIVE;
        std::cout << "Received KEEP_ALIVE message\n";
        message_queue_.push(std::move(message));
        return popMessage();
    }

    // Sanity check on message length (max 256KB for piece messages + overhead)
    if (message_length > 262144 + 9) {
        std::cerr << "ERROR: Invalid message length: " << message_length << " (max 262153)\n";
        disconnect();
        return nullptr;
    }

    // Read message ID (1 byte)
    uint8_t message_id;
    if (!receiveData(&message_id, 1)) {
        std::cerr << "ERROR: Failed to read message ID\n";
        return nullptr;
    }

    // Read payload (if any)
    std::vector<uint8_t> payload;
    if (message_length > 1) {
        payload.resize(message_length - 1);
        if (!receiveData(payload.data(), payload.size())) {
            std::cerr << "ERROR: Failed to read message payload (expected " << (message_length - 1) << " bytes)\n";
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
            std::cout << "Received CHOKE message\n";
            // Clear pending requests when choked
            if (hasPendingRequests()) {
                std::cout << "Clearing " << numPendingRequests() << " pending requests due to CHOKE\n";
                clearPendingRequests();
            }
            break;
        case MessageType::UNCHOKE:
            peer_choking_ = false;
            std::cout << "Received UNCHOKE message\n";
            break;
        case MessageType::INTERESTED:
            peer_interested_ = true;
            std::cout << "Received INTERESTED message\n";
            break;
        case MessageType::NOT_INTERESTED:
            peer_interested_ = false;
            std::cout << "Received NOT_INTERESTED message\n";
            break;
        case MessageType::HAVE: {
            HaveMessage have_msg;
            if (parseHave(*message, have_msg)) {
                // Update peer bitfield - mark the piece as available
                if (have_msg.piece_index < peer_bitfield_.size()) {
                    peer_bitfield_[have_msg.piece_index] = true;
                } else {
                    // Expand bitfield if needed
                    peer_bitfield_.resize(have_msg.piece_index + 1, false);
                    peer_bitfield_[have_msg.piece_index] = true;
                }
            }
            break;
        }
        case MessageType::BITFIELD: {
            BitfieldMessage bitfield_msg;
            if (parseBitfield(*message, bitfield_msg)) {
                // Update peer bitfield
                peer_bitfield_ = std::move(bitfield_msg.bitfield);

                // Log statistics
                size_t piece_count = getPeerPieceCount();
                size_t total_pieces = peer_bitfield_.size();
                double completion = total_pieces > 0 ?
                    (static_cast<double>(piece_count) / total_pieces * 100.0) : 0.0;

                std::cout << "Peer bitfield received: " << piece_count << "/" << total_pieces
                          << " pieces (" << completion << "%)";
                if (isPeerSeeder()) {
                    std::cout << " - SEEDER";
                }
                std::cout << "\n";
            }
            break;
        }
        case MessageType::REQUEST:
            std::cout << "Received REQUEST message\n";
            break;
        case MessageType::PIECE: {
            std::cout << "Received PIECE message (" << message->payload.size() << " bytes payload)\n";
            // Parse PIECE message to get piece_index and offset, then remove from pending
            PieceMessage piece_msg;
            if (parsePiece(*message, piece_msg)) {
                removeRequest(piece_msg.piece_index, piece_msg.offset);
            }
            break;
        }
        case MessageType::CANCEL:
            std::cout << "Received CANCEL message\n";
            break;
        case MessageType::SUGGEST_PIECE: {
            LOG_DEBUG("Received SUGGEST_PIECE message");
            SuggestPieceMessage suggest_msg;
            if (parseSuggestPiece(*message, suggest_msg)) {
                suggested_pieces_.push_back(suggest_msg.piece_index);
                LOG_INFO("Peer suggests downloading piece {}", suggest_msg.piece_index);
            }
            break;
        }
        case MessageType::HAVE_ALL:
            LOG_INFO("Received HAVE_ALL message - peer is a seeder");
            // Mark all pieces as available
            for (size_t i = 0; i < peer_bitfield_.size(); ++i) {
                peer_bitfield_[i] = true;
            }
            break;
        case MessageType::HAVE_NONE:
            LOG_INFO("Received HAVE_NONE message - peer has no pieces");
            // Mark all pieces as unavailable
            for (size_t i = 0; i < peer_bitfield_.size(); ++i) {
                peer_bitfield_[i] = false;
            }
            break;
        case MessageType::REJECT_REQUEST: {
            RejectRequestMessage reject_msg;
            if (parseRejectRequest(*message, reject_msg)) {
                LOG_WARN("Request rejected: piece {} offset {} length {}",
                         reject_msg.piece_index, reject_msg.offset, reject_msg.length);
                // Remove from pending requests
                removeRequest(reject_msg.piece_index, reject_msg.offset);
            }
            break;
        }
        case MessageType::ALLOWED_FAST: {
            AllowedFastMessage allowed_msg;
            if (parseAllowedFast(*message, allowed_msg)) {
                allowed_fast_set_.insert(allowed_msg.piece_index);
                LOG_INFO("Peer allows fast access to piece {}", allowed_msg.piece_index);
            }
            break;
        }
        case MessageType::EXTENDED: {
            LOG_DEBUG("Received EXTENDED message ({} bytes payload)", message->payload.size());
            if (extension_protocol_ && !message->payload.empty()) {
                uint8_t ext_id = message->payload[0];
                std::vector<uint8_t> ext_payload(message->payload.begin() + 1, message->payload.end());

                if (ext_id == 0) {
                    // Extended handshake
                    LOG_INFO("Received extended handshake from peer");
                    extension_protocol_->parseExtendedHandshake(ext_payload);

                    // After extended handshake, send initial PEX message if enabled
                    if (pex_manager_ && extension_protocol_->peerSupportsExtension(EXT_NAME_PEX)) {
                        LOG_INFO("Peer supports PEX, will exchange peers");
                    }
                } else {
                    // Regular extension message
                    extension_protocol_->handleExtensionMessage(ext_id, ext_payload);
                }
            }
            break;
        }
        default:
            std::cout << "Received unknown message type: " << static_cast<int>(message->type) << "\n";
            break;
    }

    // Add message to queue for processing
    message_queue_.push(std::move(message));

    // Return the message from queue
    return popMessage();
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

// ============================================================================
// Fast Extension (BEP 6) Messages
// ============================================================================

bool PeerConnection::sendHaveAll() {
    if (!peer_supports_fast_extension_) {
        LOG_WARN("Cannot send HAVE_ALL: peer doesn't support Fast Extension");
        return false;
    }

    PeerMessage msg{MessageType::HAVE_ALL, {}};
    LOG_DEBUG("Sending HAVE_ALL message");
    return sendMessage(msg);
}

bool PeerConnection::sendHaveNone() {
    if (!peer_supports_fast_extension_) {
        LOG_WARN("Cannot send HAVE_NONE: peer doesn't support Fast Extension");
        return false;
    }

    PeerMessage msg{MessageType::HAVE_NONE, {}};
    LOG_DEBUG("Sending HAVE_NONE message");
    return sendMessage(msg);
}

bool PeerConnection::sendSuggestPiece(uint32_t piece_index) {
    if (!peer_supports_fast_extension_) {
        LOG_WARN("Cannot send SUGGEST_PIECE: peer doesn't support Fast Extension");
        return false;
    }

    std::vector<uint8_t> payload(4);
    payload[0] = (piece_index >> 24) & 0xFF;
    payload[1] = (piece_index >> 16) & 0xFF;
    payload[2] = (piece_index >> 8) & 0xFF;
    payload[3] = piece_index & 0xFF;

    PeerMessage msg{MessageType::SUGGEST_PIECE, payload};
    LOG_DEBUG("Sending SUGGEST_PIECE: piece {}", piece_index);
    return sendMessage(msg);
}

bool PeerConnection::sendRejectRequest(uint32_t piece_index, uint32_t offset, uint32_t length) {
    if (!peer_supports_fast_extension_) {
        LOG_WARN("Cannot send REJECT_REQUEST: peer doesn't support Fast Extension");
        return false;
    }

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

    PeerMessage msg{MessageType::REJECT_REQUEST, payload};
    LOG_DEBUG("Sending REJECT_REQUEST: piece {} offset {} length {}", piece_index, offset, length);
    return sendMessage(msg);
}

bool PeerConnection::sendAllowedFast(uint32_t piece_index) {
    if (!peer_supports_fast_extension_) {
        LOG_WARN("Cannot send ALLOWED_FAST: peer doesn't support Fast Extension");
        return false;
    }

    std::vector<uint8_t> payload(4);
    payload[0] = (piece_index >> 24) & 0xFF;
    payload[1] = (piece_index >> 16) & 0xFF;
    payload[2] = (piece_index >> 8) & 0xFF;
    payload[3] = piece_index & 0xFF;

    PeerMessage msg{MessageType::ALLOWED_FAST, payload};
    LOG_DEBUG("Sending ALLOWED_FAST: piece {}", piece_index);
    return sendMessage(msg);
}

bool PeerConnection::sendExtendedMessage(uint8_t ext_id, const std::vector<uint8_t>& payload) {
    if (!extension_protocol_) {
        LOG_WARN("Cannot send extended message: extension protocol not initialized");
        return false;
    }

    // Extended message format: <msg_id=20><ext_id><payload>
    std::vector<uint8_t> full_payload;
    full_payload.push_back(ext_id);
    full_payload.insert(full_payload.end(), payload.begin(), payload.end());

    PeerMessage msg{MessageType::EXTENDED, full_payload};
    return sendMessage(msg);
}

bool PeerConnection::sendExtendedHandshake() {
    if (!extension_protocol_) {
        // Initialize extension protocol if not already done
        extension_protocol_ = std::make_unique<ExtensionProtocol>();
    }

    auto handshake_payload = extension_protocol_->buildExtendedHandshake();
    return sendExtendedMessage(0, handshake_payload); // ext_id=0 for handshake
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

std::unique_ptr<PeerMessage> PeerConnection::popMessage() {
    if (message_queue_.empty()) {
        return nullptr;
    }

    auto message = std::move(message_queue_.front());
    message_queue_.pop();
    return message;
}

bool PeerConnection::parseBitfield(const PeerMessage& message, BitfieldMessage& result) {
    if (message.type != MessageType::BITFIELD) {
        std::cerr << "ERROR: Message is not a BITFIELD message\n";
        return false;
    }

    if (message.payload.empty()) {
        std::cerr << "ERROR: BITFIELD message has empty payload\n";
        return false;
    }

    // Convert byte array to vector<bool>
    result.bitfield.clear();
    result.bitfield.reserve(message.payload.size() * 8);

    for (size_t i = 0; i < message.payload.size(); ++i) {
        uint8_t byte = message.payload[i];
        for (int bit = 7; bit >= 0; --bit) {
            result.bitfield.push_back((byte & (1 << bit)) != 0);
        }
    }

    // Note: The bitfield may have extra bits due to byte padding
    // The caller should trim it to the actual number of pieces if needed
    std::cout << "Parsed BITFIELD message: " << result.bitfield.size() << " bits (may need trimming)\n";
    return true;
}

bool PeerConnection::parseHave(const PeerMessage& message, HaveMessage& result) {
    if (message.type != MessageType::HAVE) {
        std::cerr << "ERROR: Message is not a HAVE message\n";
        return false;
    }

    if (message.payload.size() != 4) {
        std::cerr << "ERROR: HAVE message payload must be 4 bytes, got " << message.payload.size() << "\n";
        return false;
    }

    // Parse piece index (big-endian)
    result.piece_index = (static_cast<uint32_t>(message.payload[0]) << 24) |
                         (static_cast<uint32_t>(message.payload[1]) << 16) |
                         (static_cast<uint32_t>(message.payload[2]) << 8) |
                         static_cast<uint32_t>(message.payload[3]);

    std::cout << "Parsed HAVE message: piece_index=" << result.piece_index << "\n";
    return true;
}

bool PeerConnection::parsePiece(const PeerMessage& message, PieceMessage& result) {
    if (message.type != MessageType::PIECE) {
        std::cerr << "ERROR: Message is not a PIECE message\n";
        return false;
    }

    if (message.payload.size() < 8) {
        std::cerr << "ERROR: PIECE message payload must be at least 8 bytes, got " << message.payload.size() << "\n";
        return false;
    }

    // Parse piece index (big-endian, bytes 0-3)
    result.piece_index = (static_cast<uint32_t>(message.payload[0]) << 24) |
                         (static_cast<uint32_t>(message.payload[1]) << 16) |
                         (static_cast<uint32_t>(message.payload[2]) << 8) |
                         static_cast<uint32_t>(message.payload[3]);

    // Parse offset (big-endian, bytes 4-7)
    result.offset = (static_cast<uint32_t>(message.payload[4]) << 24) |
                    (static_cast<uint32_t>(message.payload[5]) << 16) |
                    (static_cast<uint32_t>(message.payload[6]) << 8) |
                    static_cast<uint32_t>(message.payload[7]);

    // Extract data (remaining bytes)
    result.data.assign(message.payload.begin() + 8, message.payload.end());

    std::cout << "Parsed PIECE message: piece_index=" << result.piece_index
              << ", offset=" << result.offset
              << ", data_size=" << result.data.size() << " bytes\n";
    return true;
}

bool PeerConnection::parseRequest(const PeerMessage& message, RequestMessage& result) {
    if (message.type != MessageType::REQUEST) {
        std::cerr << "ERROR: Message is not a REQUEST message\n";
        return false;
    }

    if (message.payload.size() != 12) {
        std::cerr << "ERROR: REQUEST message payload must be 12 bytes, got " << message.payload.size() << "\n";
        return false;
    }

    // Parse piece index (big-endian, bytes 0-3)
    result.piece_index = (static_cast<uint32_t>(message.payload[0]) << 24) |
                         (static_cast<uint32_t>(message.payload[1]) << 16) |
                         (static_cast<uint32_t>(message.payload[2]) << 8) |
                         static_cast<uint32_t>(message.payload[3]);

    // Parse offset (big-endian, bytes 4-7)
    result.offset = (static_cast<uint32_t>(message.payload[4]) << 24) |
                    (static_cast<uint32_t>(message.payload[5]) << 16) |
                    (static_cast<uint32_t>(message.payload[6]) << 8) |
                    static_cast<uint32_t>(message.payload[7]);

    // Parse length (big-endian, bytes 8-11)
    result.length = (static_cast<uint32_t>(message.payload[8]) << 24) |
                    (static_cast<uint32_t>(message.payload[9]) << 16) |
                    (static_cast<uint32_t>(message.payload[10]) << 8) |
                    static_cast<uint32_t>(message.payload[11]);

    std::cout << "Parsed REQUEST message: piece_index=" << result.piece_index
              << ", offset=" << result.offset
              << ", length=" << result.length << "\n";
    return true;
}

bool PeerConnection::parseCancel(const PeerMessage& message, CancelMessage& result) {
    if (message.type != MessageType::CANCEL) {
        std::cerr << "ERROR: Message is not a CANCEL message\n";
        return false;
    }

    if (message.payload.size() != 12) {
        std::cerr << "ERROR: CANCEL message payload must be 12 bytes, got " << message.payload.size() << "\n";
        return false;
    }

    // Parse piece index (big-endian, bytes 0-3)
    result.piece_index = (static_cast<uint32_t>(message.payload[0]) << 24) |
                         (static_cast<uint32_t>(message.payload[1]) << 16) |
                         (static_cast<uint32_t>(message.payload[2]) << 8) |
                         static_cast<uint32_t>(message.payload[3]);

    // Parse offset (big-endian, bytes 4-7)
    result.offset = (static_cast<uint32_t>(message.payload[4]) << 24) |
                    (static_cast<uint32_t>(message.payload[5]) << 16) |
                    (static_cast<uint32_t>(message.payload[6]) << 8) |
                    static_cast<uint32_t>(message.payload[7]);

    // Parse length (big-endian, bytes 8-11)
    result.length = (static_cast<uint32_t>(message.payload[8]) << 24) |
                    (static_cast<uint32_t>(message.payload[9]) << 16) |
                    (static_cast<uint32_t>(message.payload[10]) << 8) |
                    static_cast<uint32_t>(message.payload[11]);

    std::cout << "Parsed CANCEL message: piece_index=" << result.piece_index
              << ", offset=" << result.offset
              << ", length=" << result.length << "\n";
    return true;
}

// ============================================================================
// Fast Extension Message Parsing (BEP 6)
// ============================================================================

bool PeerConnection::parseSuggestPiece(const PeerMessage& message, SuggestPieceMessage& result) {
    if (message.type != MessageType::SUGGEST_PIECE) {
        LOG_ERROR("Message is not a SUGGEST_PIECE message");
        return false;
    }

    if (message.payload.size() != 4) {
        LOG_ERROR("SUGGEST_PIECE message payload must be 4 bytes, got {}", message.payload.size());
        return false;
    }

    // Parse piece index (big-endian)
    result.piece_index = (static_cast<uint32_t>(message.payload[0]) << 24) |
                         (static_cast<uint32_t>(message.payload[1]) << 16) |
                         (static_cast<uint32_t>(message.payload[2]) << 8) |
                         static_cast<uint32_t>(message.payload[3]);

    LOG_DEBUG("Parsed SUGGEST_PIECE message: piece_index={}", result.piece_index);
    return true;
}

bool PeerConnection::parseRejectRequest(const PeerMessage& message, RejectRequestMessage& result) {
    if (message.type != MessageType::REJECT_REQUEST) {
        LOG_ERROR("Message is not a REJECT_REQUEST message");
        return false;
    }

    if (message.payload.size() != 12) {
        LOG_ERROR("REJECT_REQUEST message payload must be 12 bytes, got {}", message.payload.size());
        return false;
    }

    // Parse piece index (big-endian, bytes 0-3)
    result.piece_index = (static_cast<uint32_t>(message.payload[0]) << 24) |
                         (static_cast<uint32_t>(message.payload[1]) << 16) |
                         (static_cast<uint32_t>(message.payload[2]) << 8) |
                         static_cast<uint32_t>(message.payload[3]);

    // Parse offset (big-endian, bytes 4-7)
    result.offset = (static_cast<uint32_t>(message.payload[4]) << 24) |
                    (static_cast<uint32_t>(message.payload[5]) << 16) |
                    (static_cast<uint32_t>(message.payload[6]) << 8) |
                    static_cast<uint32_t>(message.payload[7]);

    // Parse length (big-endian, bytes 8-11)
    result.length = (static_cast<uint32_t>(message.payload[8]) << 24) |
                    (static_cast<uint32_t>(message.payload[9]) << 16) |
                    (static_cast<uint32_t>(message.payload[10]) << 8) |
                    static_cast<uint32_t>(message.payload[11]);

    LOG_DEBUG("Parsed REJECT_REQUEST message: piece_index={}, offset={}, length={}",
              result.piece_index, result.offset, result.length);
    return true;
}

bool PeerConnection::parseAllowedFast(const PeerMessage& message, AllowedFastMessage& result) {
    if (message.type != MessageType::ALLOWED_FAST) {
        LOG_ERROR("Message is not an ALLOWED_FAST message");
        return false;
    }

    if (message.payload.size() != 4) {
        LOG_ERROR("ALLOWED_FAST message payload must be 4 bytes, got {}", message.payload.size());
        return false;
    }

    // Parse piece index (big-endian)
    result.piece_index = (static_cast<uint32_t>(message.payload[0]) << 24) |
                         (static_cast<uint32_t>(message.payload[1]) << 16) |
                         (static_cast<uint32_t>(message.payload[2]) << 8) |
                         static_cast<uint32_t>(message.payload[3]);

    LOG_DEBUG("Parsed ALLOWED_FAST message: piece_index={}", result.piece_index);
    return true;
}

// Bitfield management methods

void PeerConnection::initializePeerBitfield(size_t num_pieces) {
    peer_bitfield_.clear();
    peer_bitfield_.resize(num_pieces, false);
    std::cout << "Initialized peer bitfield with " << num_pieces << " pieces\n";
}

bool PeerConnection::peerHasPiece(uint32_t piece_index) const {
    if (piece_index >= peer_bitfield_.size()) {
        return false;
    }
    return peer_bitfield_[piece_index];
}

size_t PeerConnection::getPeerPieceCount() const {
    size_t count = 0;
    for (bool has_piece : peer_bitfield_) {
        if (has_piece) {
            count++;
        }
    }
    return count;
}

bool PeerConnection::isPeerSeeder() const {
    if (peer_bitfield_.empty()) {
        return false;
    }

    // A peer is a seeder if it has all pieces
    for (bool has_piece : peer_bitfield_) {
        if (!has_piece) {
            return false;
        }
    }
    return true;
}

// Piece request management methods

bool PeerConnection::requestPiece(uint32_t piece_index, const std::vector<Block>& blocks) {
    if (!isReadyForRequests()) {
        std::cerr << "Cannot request piece: not ready (choked=" << peer_choking_
                  << ", interested=" << am_interested_ << ")\n";
        return false;
    }

    if (!peerHasPiece(piece_index)) {
        std::cerr << "Cannot request piece #" << piece_index << ": peer doesn't have it\n";
        return false;
    }

    std::cout << "Requesting piece #" << piece_index << " (" << blocks.size() << " blocks)\n";

    // Request all blocks for this piece
    int requested = 0;
    for (const auto& block : blocks) {
        if (requestBlock(block.piece_index, block.offset, block.length)) {
            requested++;
        }
    }

    std::cout << "Successfully requested " << requested << "/" << blocks.size() << " blocks\n";
    return requested > 0;
}

bool PeerConnection::requestBlock(uint32_t piece_index, uint32_t offset, uint32_t length) {
    // With Fast Extension, we can request even when choked if piece is in allowed_fast_set
    bool can_request = isReadyForRequests();
    if (!can_request && peer_supports_fast_extension_ && isAllowedFast(piece_index)) {
        can_request = true;
        LOG_DEBUG("Requesting allowed fast piece {} even though choked", piece_index);
    }

    if (!can_request) {
        std::cerr << "Cannot request block: not ready for requests\n";
        return false;
    }

    // Check if already requested
    std::stringstream key;
    key << piece_index << ":" << offset;
    std::string key_str = key.str();

    if (pending_requests_.find(key_str) != pending_requests_.end()) {
        std::cerr << "Block " << key_str << " already requested\n";
        return false;
    }

    // Send REQUEST message
    if (!sendRequest(piece_index, offset, length)) {
        std::cerr << "Failed to send REQUEST message for block " << key_str << "\n";
        return false;
    }

    // Track the request
    pending_requests_.emplace(key_str, PendingRequest(piece_index, offset, length));

    std::cout << "Requested block: piece=" << piece_index
              << " offset=" << offset
              << " length=" << length
              << " (pending: " << pending_requests_.size() << ")\n";

    return true;
}

void PeerConnection::clearPendingRequests() {
    size_t count = pending_requests_.size();
    pending_requests_.clear();
    if (count > 0) {
        std::cout << "Cleared " << count << " pending requests\n";
    }
}

bool PeerConnection::isPendingRequest(uint32_t piece_index, uint32_t offset) const {
    std::stringstream key;
    key << piece_index << ":" << offset;
    return pending_requests_.find(key.str()) != pending_requests_.end();
}

std::vector<PendingRequest> PeerConnection::getTimedOutRequests(int timeout_seconds) {
    std::vector<PendingRequest> timed_out;
    auto now = std::chrono::steady_clock::now();

    for (auto it = pending_requests_.begin(); it != pending_requests_.end(); ) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - it->second.request_time
        ).count();

        if (elapsed >= timeout_seconds) {
            std::cout << "Request timed out: piece=" << it->second.piece_index
                      << " offset=" << it->second.offset
                      << " (waited " << elapsed << "s)\n";
            timed_out.push_back(it->second);
            it = pending_requests_.erase(it);
        } else {
            ++it;
        }
    }

    return timed_out;
}

void PeerConnection::removeRequest(uint32_t piece_index, uint32_t offset) {
    std::stringstream key;
    key << piece_index << ":" << offset;
    std::string key_str = key.str();

    auto it = pending_requests_.find(key_str);
    if (it != pending_requests_.end()) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - it->second.request_time
        ).count();

        std::cout << "Completed request: piece=" << piece_index
                  << " offset=" << offset
                  << " (took " << elapsed << "ms)\n";

        pending_requests_.erase(it);
    }
}

// Upload tracking methods

bool PeerConnection::addPendingUpload(uint32_t piece_index, uint32_t offset, uint32_t length) {
    std::stringstream key;
    key << piece_index << ":" << offset;
    std::string key_str = key.str();

    if (pending_uploads_.find(key_str) != pending_uploads_.end()) {
        return false; // Already pending
    }

    pending_uploads_.emplace(key_str, PendingUpload(piece_index, offset, length));
    return true;
}

void PeerConnection::removePendingUpload(uint32_t piece_index, uint32_t offset) {
    std::stringstream key;
    key << piece_index << ":" << offset;
    pending_uploads_.erase(key.str());
}

void PeerConnection::clearPendingUploads() {
    pending_uploads_.clear();
}

bool PeerConnection::isPendingUpload(uint32_t piece_index, uint32_t offset) const {
    std::stringstream key;
    key << piece_index << ":" << offset;
    return pending_uploads_.find(key.str()) != pending_uploads_.end();
}

// ============================================================================
// MSE/PE Encryption Support
// ============================================================================

bool PeerConnection::performMSEHandshake(bool is_initiator, const std::vector<uint8_t>& info_hash) {
    LOG_INFO("PeerConnection: Starting MSE handshake (initiator={})", is_initiator);

    // Default to prefer encrypted
    MSEHandshake mse(MSEHandshake::Mode::PREFER_ENCRYPTED, info_hash);

    MSEHandshake::Result result;
    if (is_initiator) {
        result = mse.performHandshakeInitiator(*this);
    } else {
        result = mse.performHandshakeResponder(*this);
    }

    if (!result.success) {
        LOG_ERROR("PeerConnection: MSE handshake failed: {}", result.error_message);
        return false;
    }

    // Create encrypted stream
    encrypted_stream_ = std::make_unique<EncryptedStream>();
    encrypted_stream_->init(
        result.selected_method,
        std::move(mse.outgoing_cipher_),
        std::move(mse.incoming_cipher_)
    );

    LOG_INFO("PeerConnection: MSE handshake successful, method: {}",
            result.selected_method == MSEHandshake::CryptoMethod::RC4 ? "RC4" : "plaintext");

    return true;
}

bool PeerConnection::isEncrypted() const {
    return encrypted_stream_ && encrypted_stream_->isEncrypted();
}

// ============================================================================
// PEX (Peer Exchange) Support
// ============================================================================

void PeerConnection::enablePex() {
    if (pex_manager_) {
        LOG_DEBUG("PEX already enabled for peer {}:{}", ip_, port_);
        return;
    }

    // Create PEX manager
    pex_manager_ = std::make_unique<PexManager>();

    // Ensure extension protocol is initialized
    if (!extension_protocol_) {
        extension_protocol_ = std::make_unique<ExtensionProtocol>();
    }

    // Register PEX extension handler
    extension_protocol_->registerExtension(
        EXT_NAME_PEX,
        [this](uint8_t ext_id, const std::vector<uint8_t>& payload) {
            // Parse incoming PEX message
            if (!pex_manager_) {
                LOG_WARN("Received PEX message but PEX manager not initialized");
                return;
            }

            try {
                bencode::BencodeValue data = bencode::decode(payload);
                std::vector<PexPeer> new_peers;
                int count = pex_manager_->parsePexMessage(data, new_peers);

                if (count > 0) {
                    LOG_INFO("PEX: Discovered {} new peers from {}:{}", count, ip_, port_);
                    // New peers will be available via pex_manager_->getKnownPeers()
                }
            } catch (const std::exception& e) {
                LOG_ERROR("Failed to parse PEX message: {}", e.what());
            }
        }
    );

    LOG_INFO("PEX enabled for peer {}:{}", ip_, port_);
}

bool PeerConnection::sendPexMessage() {
    if (!pex_manager_) {
        LOG_WARN("Cannot send PEX message: PEX not enabled");
        return false;
    }

    if (!extension_protocol_) {
        LOG_WARN("Cannot send PEX message: extension protocol not initialized");
        return false;
    }

    if (!extension_protocol_->peerSupportsExtension(EXT_NAME_PEX)) {
        LOG_DEBUG("Cannot send PEX message: peer doesn't support PEX");
        return false;
    }

    // Build PEX message
    bencode::BencodeValue pex_data = pex_manager_->buildPexMessage();

    // Send via extension protocol
    std::vector<uint8_t> message = extension_protocol_->buildExtensionMessage(
        EXT_NAME_PEX,
        pex_data
    );

    if (message.empty()) {
        LOG_WARN("Failed to build PEX extension message");
        return false;
    }

    // Send the message
    if (!sendData(message.data(), message.size())) {
        LOG_ERROR("Failed to send PEX message to {}:{}", ip_, port_);
        return false;
    }

    pex_manager_->markUpdateSent();
    LOG_DEBUG("Sent PEX message to {}:{}", ip_, port_);
    return true;
}

// ============================================================================
// Fast Extension Support Methods
// ============================================================================

bool PeerConnection::isAllowedFast(uint32_t piece_index) const {
    return allowed_fast_set_.find(piece_index) != allowed_fast_set_.end();
}

void PeerConnection::generateAllowedFastSet(size_t num_pieces, size_t k) {
    if (!supports_fast_extension_) {
        return;
    }

    // BEP 6 specifies generating allowed fast set using a hash function
    // For simplicity, we'll use a deterministic random selection based on IP and info_hash

    // Clear existing set
    allowed_fast_set_.clear();

    if (num_pieces == 0 || k == 0) {
        return;
    }

    // Limit k to reasonable value
    k = std::min(k, std::min(num_pieces, size_t(10)));

    // Generate k random piece indices
    // In a full implementation, this should use the algorithm from BEP 6
    // which uses SHA1(IP || info_hash) to generate deterministic allowed pieces
    // For now, we'll use a simple approach

    std::hash<std::string> hasher;
    size_t seed = hasher(ip_ + std::to_string(port_));

    for (size_t i = 0; i < k && allowed_fast_set_.size() < k; ++i) {
        // Simple pseudo-random piece selection
        uint32_t piece = (seed + i * 37) % num_pieces;
        allowed_fast_set_.insert(piece);
    }

    LOG_INFO("Generated allowed fast set with {} pieces for peer {}:{}",
             allowed_fast_set_.size(), ip_, port_);

    // Send ALLOWED_FAST messages to peer if they support it
    if (peer_supports_fast_extension_) {
        for (uint32_t piece : allowed_fast_set_) {
            sendAllowedFast(piece);
        }
    }
}

} // namespace torrent
