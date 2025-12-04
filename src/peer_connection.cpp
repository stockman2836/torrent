#include "peer_connection.h"
#include "utils.h"
#include <cstring>
#include <iostream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
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
    // TODO: Implement socket connection
    // This is a stub for now
    std::cout << "Connecting to peer " << ip_ << ":" << port_ << "\n";
    return false;
}

void PeerConnection::disconnect() {
    if (socket_fd_ != INVALID_SOCKET) {
        closesocket(socket_fd_);
        socket_fd_ = INVALID_SOCKET;
    }
    connected_ = false;
}

bool PeerConnection::performHandshake() {
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

    // TODO: Send handshake and receive response
    return false;
}

bool PeerConnection::sendMessage(const PeerMessage& message) {
    std::vector<uint8_t> data = serializeMessage(message);
    // TODO: Send via socket
    return false;
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
    // TODO: Receive and parse message
    return nullptr;
}

bool PeerConnection::sendKeepAlive() {
    // Keep-alive: <len=0000>
    return false;
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

} // namespace torrent
