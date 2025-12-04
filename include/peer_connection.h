#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <functional>

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
    PORT = 9
};

struct PeerMessage {
    MessageType type;
    std::vector<uint8_t> payload;
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

    // State
    bool amChoking() const { return am_choking_; }
    bool amInterested() const { return am_interested_; }
    bool peerChoking() const { return peer_choking_; }
    bool peerInterested() const { return peer_interested_; }

    const std::vector<bool>& peerBitfield() const { return peer_bitfield_; }

private:
    bool sendMessage(const PeerMessage& message);
    std::vector<uint8_t> serializeMessage(const PeerMessage& message);

    std::string ip_;
    uint16_t port_;
    std::vector<uint8_t> info_hash_;
    std::string peer_id_;

    int socket_fd_;
    bool connected_;

    // Peer state
    bool am_choking_;
    bool am_interested_;
    bool peer_choking_;
    bool peer_interested_;

    std::vector<bool> peer_bitfield_;
};

} // namespace torrent
