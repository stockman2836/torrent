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

} // namespace torrent
