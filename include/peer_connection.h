#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <functional>
#include <queue>

namespace torrent {

// BitTorrent protocol message types
enum class MessageType : uint8_t {
    CHOKE = 0,
    UNCHOKE = 1,
    INTERESTED = 2,
    NOT_INTERESTED = 3,
    HAVE = 4,
    BITFIELD = 5,
    REQUEST = 6,
    PIECE = 7,
    CANCEL = 8,
    PORT = 9,
    KEEP_ALIVE = 255  // Special type for keep-alive messages
};

struct PeerMessage {
    MessageType type;
    std::vector<uint8_t> payload;
};

// Parsed message structures for specific message types
struct BitfieldMessage {
    std::vector<bool> bitfield;
};

struct HaveMessage {
    uint32_t piece_index;
};

struct PieceMessage {
    uint32_t piece_index;
    uint32_t offset;
    std::vector<uint8_t> data;
};

struct RequestMessage {
    uint32_t piece_index;
    uint32_t offset;
    uint32_t length;
};

struct CancelMessage {
    uint32_t piece_index;
    uint32_t offset;
    uint32_t length;
};

class PeerConnection {
public:
    PeerConnection(const std::string& ip,
                  uint16_t port,
                  const std::vector<uint8_t>& info_hash,
                  const std::string& peer_id);

    ~PeerConnection();

    // Connection management
    bool connect();
    void disconnect();
    bool isConnected() const { return connected_; }

    // Handshake
    bool performHandshake();

    // Message sending
    bool sendKeepAlive();
    bool sendChoke();
    bool sendUnchoke();
    bool sendInterested();
    bool sendNotInterested();
    bool sendHave(uint32_t piece_index);
    bool sendBitfield(const std::vector<bool>& bitfield);
    bool sendRequest(uint32_t piece_index, uint32_t offset, uint32_t length);
    bool sendPiece(uint32_t piece_index, uint32_t offset, const std::vector<uint8_t>& data);
    bool sendCancel(uint32_t piece_index, uint32_t offset, uint32_t length);

    // Message receiving
    std::unique_ptr<PeerMessage> receiveMessage();

    // Check if there are messages in the queue
    bool hasMessages() const { return !message_queue_.empty(); }

    // Get the next message from the queue
    std::unique_ptr<PeerMessage> popMessage();

    // Parse specific message types
    bool parseBitfield(const PeerMessage& message, BitfieldMessage& result);
    bool parseHave(const PeerMessage& message, HaveMessage& result);
    bool parsePiece(const PeerMessage& message, PieceMessage& result);
    bool parseRequest(const PeerMessage& message, RequestMessage& result);
    bool parseCancel(const PeerMessage& message, CancelMessage& result);

    // State
    bool amChoking() const { return am_choking_; }
    bool amInterested() const { return am_interested_; }
    bool peerChoking() const { return peer_choking_; }
    bool peerInterested() const { return peer_interested_; }

    const std::vector<bool>& peerBitfield() const { return peer_bitfield_; }
    const std::string& remotePeerId() const { return remote_peer_id_; }

private:
    bool sendMessage(const PeerMessage& message);
    std::vector<uint8_t> serializeMessage(const PeerMessage& message);

    // Low-level socket operations
    bool sendData(const void* data, size_t length);
    bool receiveData(void* buffer, size_t length);
    bool receiveDataWithTimeout(void* buffer, size_t length, int timeout_ms);
    bool setNonBlocking(bool non_blocking);
    bool setSocketTimeout(int timeout_ms);

    std::string ip_;
    uint16_t port_;
    std::vector<uint8_t> info_hash_;
    std::string peer_id_;
    std::string remote_peer_id_;  // Peer ID received during handshake

    int socket_fd_;
    bool connected_;
    bool handshake_completed_;

    // Peer state
    bool am_choking_;
    bool am_interested_;
    bool peer_choking_;
    bool peer_interested_;

    std::vector<bool> peer_bitfield_;

    // Message queue for processing messages in order
    std::queue<std::unique_ptr<PeerMessage>> message_queue_;
};

} // namespace torrent
