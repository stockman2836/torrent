#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <array>
#include <chrono>

namespace torrent {
namespace dht {

// DHT Node ID is 160-bit (20 bytes)
constexpr size_t NODE_ID_SIZE = 20;
using NodeID = std::array<uint8_t, NODE_ID_SIZE>;

// Node status based on BEP 5
enum class NodeStatus {
    GOOD,         // Responded to our query within last 15 minutes
    QUESTIONABLE, // Haven't heard from in last 15 minutes
    BAD           // Failed to respond to multiple queries
};

// DHT Node representation
class Node {
public:
    Node(const NodeID& id, const std::string& ip, uint16_t port);
    Node(const std::vector<uint8_t>& id_bytes, const std::string& ip, uint16_t port);

    // Getters
    const NodeID& id() const { return id_; }
    const std::string& ip() const { return ip_; }
    uint16_t port() const { return port_; }
    NodeStatus status() const { return status_; }

    // Status management
    void markGood();
    void markQuestionable();
    void markBad();
    void incrementFailures();
    void resetFailures();

    // Activity tracking
    void updateLastSeen();
    std::chrono::steady_clock::time_point lastSeen() const { return last_seen_; }
    bool isGood() const;
    bool isQuestionable() const;
    bool isBad() const;

    // Comparison for sorting
    bool operator==(const Node& other) const;
    bool operator!=(const Node& other) const;

    // Convert to compact format (26 bytes: 20 bytes ID + 4 bytes IP + 2 bytes port)
    std::vector<uint8_t> toCompact() const;
    static Node fromCompact(const std::vector<uint8_t>& compact);

    // String representation for debugging
    std::string toString() const;

private:
    NodeID id_;
    std::string ip_;
    uint16_t port_;
    NodeStatus status_;
    std::chrono::steady_clock::time_point last_seen_;
    int consecutive_failures_;
};

// Distance calculation using XOR metric (Kademlia)
class NodeDistance {
public:
    // Calculate XOR distance between two node IDs
    static NodeID distance(const NodeID& a, const NodeID& b);

    // Compare distances: returns true if a is closer to target than b
    static bool isCloser(const NodeID& target, const NodeID& a, const NodeID& b);

    // Get the bucket index for a node ID relative to our ID
    // Returns value from 0 to 159 (bit position of first differing bit)
    static int bucketIndex(const NodeID& our_id, const NodeID& other_id);

    // Convert NodeID to hex string for debugging
    static std::string toHex(const NodeID& id);

    // Generate random NodeID
    static NodeID generateRandomID();

    // Parse NodeID from hex string
    static NodeID fromHex(const std::string& hex);

    // Compare two NodeIDs (for std::set, std::map)
    static bool less(const NodeID& a, const NodeID& b);
};

} // namespace dht
} // namespace torrent
