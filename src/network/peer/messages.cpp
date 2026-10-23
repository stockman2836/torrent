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

bool PeerConnection::sendMessage(const PeerMessage& message) {
    std::lock_guard<std::mutex> lock(send_mutex_);
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

} // namespace torrent
