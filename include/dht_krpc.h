#pragma once

#include "dht_node.h"
#include "bencode.h"
#include <string>
#include <vector>
#include <map>
#include <variant>
#include <optional>

namespace torrent {
namespace dht {

// KRPC Message Types
enum class MessageType {
    QUERY,
    RESPONSE,
    ERROR
};

// Query types
enum class QueryType {
    PING,
    FIND_NODE,
    GET_PEERS,
    ANNOUNCE_PEER
};

// KRPC Error codes (BEP 5)
enum class ErrorCode {
    GENERIC_ERROR = 201,
    SERVER_ERROR = 202,
    PROTOCOL_ERROR = 203,
    METHOD_UNKNOWN = 204
};

// Base KRPC Message
struct KRPCMessage {
    std::string transaction_id;
    MessageType type;

    virtual ~KRPCMessage() = default;
    virtual bencode::BencodeValue toBencode() const = 0;
};

// Query message
struct KRPCQuery : public KRPCMessage {
    QueryType query_type;
    NodeID sender_id;

    // Query-specific arguments
    std::optional<NodeID> target;        // For find_node
    std::optional<std::vector<uint8_t>> info_hash;  // For get_peers, announce_peer
    std::optional<uint16_t> port;        // For announce_peer
    std::optional<std::string> token;    // For announce_peer
    std::optional<int> implied_port;     // For announce_peer

    KRPCQuery() { type = MessageType::QUERY; }

    bencode::BencodeValue toBencode() const override;
    static KRPCQuery fromBencode(const bencode::BencodeValue& value);

    std::string getQueryName() const;
};

// Response message
struct KRPCResponse : public KRPCMessage {
    NodeID sender_id;

    // Response-specific fields
    std::optional<std::vector<Node>> nodes;        // For find_node, get_peers
    std::optional<std::vector<std::string>> values; // For get_peers (peer addresses)
    std::optional<std::string> token;              // For get_peers

    KRPCResponse() { type = MessageType::RESPONSE; }

    bencode::BencodeValue toBencode() const override;
    static KRPCResponse fromBencode(const bencode::BencodeValue& value);
};

// Error message
struct KRPCError : public KRPCMessage {
    ErrorCode error_code;
    std::string error_message;

    KRPCError() { type = MessageType::ERROR; }
    KRPCError(ErrorCode code, const std::string& msg)
        : error_code(code), error_message(msg) {
        type = MessageType::ERROR;
    }

    bencode::BencodeValue toBencode() const override;
    static KRPCError fromBencode(const bencode::BencodeValue& value);
};

// KRPC Message factory and parser
class KRPCMessageFactory {
public:
    // Parse raw bencode data into appropriate message type
    static std::variant<KRPCQuery, KRPCResponse, KRPCError> parse(
        const std::vector<uint8_t>& data);

    // Create ping query
    static KRPCQuery createPing(const NodeID& sender_id, const std::string& tid);

    // Create find_node query
    static KRPCQuery createFindNode(const NodeID& sender_id,
                                    const NodeID& target,
                                    const std::string& tid);

    // Create get_peers query
    static KRPCQuery createGetPeers(const NodeID& sender_id,
                                    const std::vector<uint8_t>& info_hash,
                                    const std::string& tid);

    // Create announce_peer query
    static KRPCQuery createAnnouncePeer(const NodeID& sender_id,
                                        const std::vector<uint8_t>& info_hash,
                                        uint16_t port,
                                        const std::string& token,
                                        const std::string& tid,
                                        bool implied_port = false);

    // Create response for ping/announce_peer
    static KRPCResponse createPingResponse(const NodeID& sender_id,
                                           const std::string& tid);

    // Create response for find_node
    static KRPCResponse createFindNodeResponse(const NodeID& sender_id,
                                               const std::vector<Node>& nodes,
                                               const std::string& tid);

    // Create response for get_peers (with nodes)
    static KRPCResponse createGetPeersResponseNodes(const NodeID& sender_id,
                                                    const std::vector<Node>& nodes,
                                                    const std::string& token,
                                                    const std::string& tid);

    // Create response for get_peers (with peer values)
    static KRPCResponse createGetPeersResponseValues(const NodeID& sender_id,
                                                     const std::vector<std::string>& values,
                                                     const std::string& token,
                                                     const std::string& tid);

    // Create error response
    static KRPCError createError(ErrorCode code,
                                 const std::string& message,
                                 const std::string& tid);

    // Serialize message to bencode bytes
    static std::vector<uint8_t> serialize(const KRPCMessage& message);

    // Generate random transaction ID
    static std::string generateTransactionId();

private:
    // Helper to parse nodes from compact format
    static std::vector<Node> parseCompactNodes(const std::string& compact);

    // Helper to encode nodes to compact format
    static std::string encodeCompactNodes(const std::vector<Node>& nodes);
};

} // namespace dht
} // namespace torrent
