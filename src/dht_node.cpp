#include "dht_node.h"
#include "utils.h"
#include <sstream>
#include <iomanip>
#include <random>
#include <cstring>
#include <algorithm>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace torrent {
namespace dht {

// Node implementation

Node::Node(const NodeID& id, const std::string& ip, uint16_t port)
    : id_(id)
    , ip_(ip)
    , port_(port)
    , status_(NodeStatus::QUESTIONABLE)
    , last_seen_(std::chrono::steady_clock::now())
    , consecutive_failures_(0) {}

Node::Node(const std::vector<uint8_t>& id_bytes, const std::string& ip, uint16_t port)
    : ip_(ip)
    , port_(port)
    , status_(NodeStatus::QUESTIONABLE)
    , last_seen_(std::chrono::steady_clock::now())
    , consecutive_failures_(0) {

    if (id_bytes.size() != NODE_ID_SIZE) {
        throw std::runtime_error("Invalid node ID size");
    }
    std::copy(id_bytes.begin(), id_bytes.end(), id_.begin());
}

void Node::markGood() {
    status_ = NodeStatus::GOOD;
    consecutive_failures_ = 0;
    updateLastSeen();
}

void Node::markQuestionable() {
    status_ = NodeStatus::QUESTIONABLE;
}

void Node::markBad() {
    status_ = NodeStatus::BAD;
}

void Node::incrementFailures() {
    consecutive_failures_++;
    if (consecutive_failures_ >= 3) {
        markBad();
    }
}

void Node::resetFailures() {
    consecutive_failures_ = 0;
}

void Node::updateLastSeen() {
    last_seen_ = std::chrono::steady_clock::now();
}

bool Node::isGood() const {
    // Good if responded within last 15 minutes
    auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(
        std::chrono::steady_clock::now() - last_seen_
    ).count();
    return status_ == NodeStatus::GOOD && elapsed < 15;
}

bool Node::isQuestionable() const {
    auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(
        std::chrono::steady_clock::now() - last_seen_
    ).count();
    return status_ == NodeStatus::QUESTIONABLE ||
           (status_ == NodeStatus::GOOD && elapsed >= 15);
}

bool Node::isBad() const {
    return status_ == NodeStatus::BAD || consecutive_failures_ >= 3;
}

bool Node::operator==(const Node& other) const {
    return id_ == other.id_;
}

bool Node::operator!=(const Node& other) const {
    return !(*this == other);
}

std::vector<uint8_t> Node::toCompact() const {
    std::vector<uint8_t> compact;
    compact.reserve(26); // 20 bytes ID + 4 bytes IP + 2 bytes port

    // Add node ID
    compact.insert(compact.end(), id_.begin(), id_.end());

    // Add IP address (4 bytes)
    struct in_addr addr;
    if (inet_pton(AF_INET, ip_.c_str(), &addr) == 1) {
        uint32_t ip_network = addr.s_addr; // Already in network byte order
        compact.push_back((ip_network >> 0) & 0xFF);
        compact.push_back((ip_network >> 8) & 0xFF);
        compact.push_back((ip_network >> 16) & 0xFF);
        compact.push_back((ip_network >> 24) & 0xFF);
    } else {
        // Invalid IP, use 0.0.0.0
        compact.insert(compact.end(), {0, 0, 0, 0});
    }

    // Add port (2 bytes, big-endian)
    compact.push_back((port_ >> 8) & 0xFF);
    compact.push_back(port_ & 0xFF);

    return compact;
}

Node Node::fromCompact(const std::vector<uint8_t>& compact) {
    if (compact.size() != 26) {
        throw std::runtime_error("Invalid compact node format");
    }

    // Extract node ID (first 20 bytes)
    NodeID id;
    std::copy(compact.begin(), compact.begin() + 20, id.begin());

    // Extract IP (next 4 bytes)
    struct in_addr addr;
    addr.s_addr = (compact[20] << 0) | (compact[21] << 8) |
                  (compact[22] << 16) | (compact[23] << 24);

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr, ip_str, INET_ADDRSTRLEN);
    std::string ip(ip_str);

    // Extract port (last 2 bytes, big-endian)
    uint16_t port = (static_cast<uint16_t>(compact[24]) << 8) | compact[25];

    return Node(id, ip, port);
}

std::string Node::toString() const {
    std::ostringstream oss;
    oss << NodeDistance::toHex(id_).substr(0, 8) << "... @ "
        << ip_ << ":" << port_
        << " [" << (isGood() ? "GOOD" : isQuestionable() ? "QUESTIONABLE" : "BAD") << "]";
    return oss.str();
}

// NodeDistance implementation

NodeID NodeDistance::distance(const NodeID& a, const NodeID& b) {
    NodeID result;
    for (size_t i = 0; i < NODE_ID_SIZE; ++i) {
        result[i] = a[i] ^ b[i];
    }
    return result;
}

bool NodeDistance::isCloser(const NodeID& target, const NodeID& a, const NodeID& b) {
    NodeID dist_a = distance(target, a);
    NodeID dist_b = distance(target, b);
    return less(dist_a, dist_b);
}

int NodeDistance::bucketIndex(const NodeID& our_id, const NodeID& other_id) {
    NodeID dist = distance(our_id, other_id);

    // Find the first non-zero bit (from left to right)
    for (int byte = 0; byte < NODE_ID_SIZE; ++byte) {
        if (dist[byte] != 0) {
            // Find the highest bit set in this byte
            uint8_t b = dist[byte];
            int bit_pos = 7;
            while (bit_pos >= 0 && !(b & (1 << bit_pos))) {
                --bit_pos;
            }
            return byte * 8 + (7 - bit_pos);
        }
    }
    return 159; // Same node (should not happen in practice)
}

std::string NodeDistance::toHex(const NodeID& id) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t byte : id) {
        oss << std::setw(2) << static_cast<int>(byte);
    }
    return oss.str();
}

NodeID NodeDistance::generateRandomID() {
    NodeID id;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);

    for (size_t i = 0; i < NODE_ID_SIZE; ++i) {
        id[i] = static_cast<uint8_t>(dis(gen));
    }
    return id;
}

NodeID NodeDistance::fromHex(const std::string& hex) {
    if (hex.length() != NODE_ID_SIZE * 2) {
        throw std::runtime_error("Invalid hex string length for NodeID");
    }

    NodeID id;
    for (size_t i = 0; i < NODE_ID_SIZE; ++i) {
        std::string byte_str = hex.substr(i * 2, 2);
        id[i] = static_cast<uint8_t>(std::stoi(byte_str, nullptr, 16));
    }
    return id;
}

bool NodeDistance::less(const NodeID& a, const NodeID& b) {
    for (size_t i = 0; i < NODE_ID_SIZE; ++i) {
        if (a[i] < b[i]) return true;
        if (a[i] > b[i]) return false;
    }
    return false; // Equal
}

} // namespace dht
} // namespace torrent
