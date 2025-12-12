#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <functional>
#include <queue>
#include <chrono>
#include <map>
#include "utp_socket.h"

namespace torrent {

// Forward declarations
struct Block;
class ExtensionProtocol;
class MSEHandshake;
class EncryptedStream;
class PexManager;

// Transport type for peer connections
enum class TransportType {
    TCP,  // Traditional TCP connection
    UTP   // uTP (Micro Transport Protocol) over UDP
};

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
    // Fast Extension (BEP 6)
    SUGGEST_PIECE = 13,
    HAVE_ALL = 14,
    HAVE_NONE = 15,
    REJECT_REQUEST = 16,
    ALLOWED_FAST = 17,
    // Extension protocol (BEP 10)
    EXTENDED = 20,
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

// Fast Extension messages (BEP 6)
struct SuggestPieceMessage {
    uint32_t piece_index;
};

struct RejectRequestMessage {
    uint32_t piece_index;
    uint32_t offset;
    uint32_t length;
};

struct AllowedFastMessage {
    uint32_t piece_index;
};

// Structure to track pending uploads to peer
struct PendingUpload {
    uint32_t piece_index;
    uint32_t offset;
    uint32_t length;
    std::chrono::steady_clock::time_point request_time;

    PendingUpload(uint32_t pi, uint32_t off, uint32_t len)
        : piece_index(pi), offset(off), length(len),
          request_time(std::chrono::steady_clock::now()) {}
};

// Structure to track pending block requests
struct PendingRequest {
    uint32_t piece_index;
    uint32_t offset;
    uint32_t length;
    std::chrono::steady_clock::time_point request_time;

    PendingRequest(uint32_t pi, uint32_t off, uint32_t len)
        : piece_index(pi), offset(off), length(len),
          request_time(std::chrono::steady_clock::now()) {}
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

    // uTP support
    void setUtpSocket(std::shared_ptr<utp::UtpSocket> utp_socket);
    TransportType getTransportType() const { return transport_type_; }
    bool isUsingUtp() const { return transport_type_ == TransportType::UTP; }

    // Handshake
    bool performHandshake();
    bool performHandshake(const std::vector<bool>& our_bitfield);

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

    // Fast Extension messages (BEP 6)
    bool sendHaveAll();
    bool sendHaveNone();
    bool sendSuggestPiece(uint32_t piece_index);
    bool sendRejectRequest(uint32_t piece_index, uint32_t offset, uint32_t length);
    bool sendAllowedFast(uint32_t piece_index);

    // Extension protocol support
    bool sendExtendedMessage(uint8_t ext_id, const std::vector<uint8_t>& payload);
    bool sendExtendedHandshake();

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

    // Parse Fast Extension messages
    bool parseSuggestPiece(const PeerMessage& message, SuggestPieceMessage& result);
    bool parseRejectRequest(const PeerMessage& message, RejectRequestMessage& result);
    bool parseAllowedFast(const PeerMessage& message, AllowedFastMessage& result);

    // State
    bool amChoking() const { return am_choking_; }
    bool amInterested() const { return am_interested_; }
    bool peerChoking() const { return peer_choking_; }
    bool peerInterested() const { return peer_interested_; }

    const std::vector<bool>& peerBitfield() const { return peer_bitfield_; }
    const std::string& remotePeerId() const { return remote_peer_id_; }

    // Bitfield management
    void initializePeerBitfield(size_t num_pieces);
    bool peerHasPiece(uint32_t piece_index) const;
    size_t getPeerPieceCount() const;
    size_t getPeerBitfieldSize() const { return peer_bitfield_.size(); }
    bool isPeerSeeder() const;

    // Piece request management
    bool requestPiece(uint32_t piece_index, const std::vector<Block>& blocks);
    bool requestBlock(uint32_t piece_index, uint32_t offset, uint32_t length);
    bool hasPendingRequests() const { return !pending_requests_.empty(); }
    size_t numPendingRequests() const { return pending_requests_.size(); }
    void clearPendingRequests();
    bool isPendingRequest(uint32_t piece_index, uint32_t offset) const;
    std::vector<PendingRequest> getTimedOutRequests(int timeout_seconds = 30);
    void removeRequest(uint32_t piece_index, uint32_t offset);

    // Upload request tracking
    bool hasPendingUploads() const { return !pending_uploads_.empty(); }
    size_t numPendingUploads() const { return pending_uploads_.size(); }
    bool addPendingUpload(uint32_t piece_index, uint32_t offset, uint32_t length);
    void removePendingUpload(uint32_t piece_index, uint32_t offset);
    void clearPendingUploads();
    bool isPendingUpload(uint32_t piece_index, uint32_t offset) const;

    // Workflow helpers
    bool canDownload() const { return !peer_choking_ && am_interested_; }
    bool isReadyForRequests() const { return handshake_completed_ && !peer_choking_ && am_interested_; }

    // Extension protocol access
    ExtensionProtocol* extensionProtocol() { return extension_protocol_.get(); }
    const ExtensionProtocol* extensionProtocol() const { return extension_protocol_.get(); }
    bool supportsExtensions() const { return extension_protocol_ != nullptr; }

    // Encryption support (MSE/PE)
    bool performMSEHandshake(bool is_initiator, const std::vector<uint8_t>& info_hash);
    bool isEncrypted() const;
    EncryptedStream* encryptedStream() { return encrypted_stream_.get(); }

    // PEX (Peer Exchange) support
    void enablePex();
    bool isPexEnabled() const { return pex_manager_ != nullptr; }
    PexManager* pexManager() { return pex_manager_.get(); }
    const PexManager* pexManager() const { return pex_manager_.get(); }
    bool sendPexMessage();  // Send PEX update to this peer

    // Fast Extension (BEP 6) support
    bool supportsFastExtension() const { return supports_fast_extension_; }
    bool peerSupportsFastExtension() const { return peer_supports_fast_extension_; }
    const std::set<uint32_t>& getAllowedFastSet() const { return allowed_fast_set_; }
    bool isAllowedFast(uint32_t piece_index) const;
    void generateAllowedFastSet(size_t num_pieces, size_t k = 10);

private:
    // Friend class for MSE handshake access to low-level methods
    friend class MSEHandshake;
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

    // Transport layer
    TransportType transport_type_;
    int socket_fd_;                                    // TCP socket
    std::shared_ptr<utp::UtpSocket> utp_socket_;      // uTP socket
    bool connected_;
    bool handshake_completed_;
    bool is_ipv6_;  // True if this connection uses IPv6

    // Peer state
    bool am_choking_;
    bool am_interested_;
    bool peer_choking_;
    bool peer_interested_;

    std::vector<bool> peer_bitfield_;

    // Message queue for processing messages in order
    std::queue<std::unique_ptr<PeerMessage>> message_queue_;

    // Pending request tracking (key: "piece_index:offset")
    std::map<std::string, PendingRequest> pending_requests_;

    // Pending upload tracking (key: "piece_index:offset")
    std::map<std::string, PendingUpload> pending_uploads_;

    // Extension protocol support (optional, for magnet links)
    std::unique_ptr<ExtensionProtocol> extension_protocol_;

    // Encryption support (optional, for MSE/PE)
    std::unique_ptr<EncryptedStream> encrypted_stream_;

    // PEX support (optional, for peer discovery)
    std::unique_ptr<PexManager> pex_manager_;

    // Fast Extension support (BEP 6)
    bool supports_fast_extension_;        // We support Fast Extension
    bool peer_supports_fast_extension_;   // Peer supports Fast Extension
    std::set<uint32_t> allowed_fast_set_; // Pieces allowed even when choked
    std::vector<uint32_t> suggested_pieces_; // Pieces suggested by peer
};

} // namespace torrent
