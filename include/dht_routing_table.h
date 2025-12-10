#pragma once

#include "dht_node.h"
#include <vector>
#include <list>
#include <mutex>
#include <optional>

namespace torrent {
namespace dht {

// K-bucket constants
constexpr size_t BUCKET_SIZE = 8;         // K value (max nodes per bucket)
constexpr size_t NUM_BUCKETS = 160;       // 160 bits in Node ID
constexpr int BUCKET_REFRESH_INTERVAL = 15 * 60; // 15 minutes in seconds

// Single K-bucket
class Bucket {
public:
    Bucket() = default;

    // Add node to bucket
    // Returns true if added, false if bucket is full
    bool addNode(const Node& node);

    // Update node's last seen time (move to end of list)
    bool updateNode(const NodeID& id);

    // Remove node from bucket
    bool removeNode(const NodeID& id);

    // Find node by ID
    std::optional<Node> findNode(const NodeID& id) const;

    // Check if bucket is full
    bool isFull() const { return nodes_.size() >= BUCKET_SIZE; }

    // Get all nodes in bucket
    std::vector<Node> getNodes() const;

    // Get good nodes in bucket
    std::vector<Node> getGoodNodes() const;

    // Get the least recently seen node (for replacement)
    std::optional<Node> getLeastRecentlySeen() const;

    // Get bucket size
    size_t size() const { return nodes_.size(); }

    // Check if bucket needs refresh
    bool needsRefresh() const;

    // Update last changed time
    void markAsRefreshed();

private:
    // List maintains least-recently-seen order (front = oldest)
    std::list<Node> nodes_;
    std::chrono::steady_clock::time_point last_changed_;
};

// Routing table with K-buckets
class RoutingTable {
public:
    explicit RoutingTable(const NodeID& our_id);

    // Add or update node in routing table
    void addNode(const Node& node);

    // Remove node from routing table
    void removeNode(const NodeID& id);

    // Update node's last seen time
    void updateNode(const NodeID& id);

    // Find node by ID
    std::optional<Node> findNode(const NodeID& id) const;

    // Find K closest nodes to target ID
    std::vector<Node> findClosestNodes(const NodeID& target, size_t count = BUCKET_SIZE) const;

    // Get all nodes in routing table
    std::vector<Node> getAllNodes() const;

    // Get all good nodes
    std::vector<Node> getGoodNodes() const;

    // Get total number of nodes
    size_t size() const;

    // Get buckets that need refresh
    std::vector<int> getBucketsNeedingRefresh() const;

    // Mark bucket as refreshed
    void markBucketRefreshed(int bucket_index);

    // Get random node ID in bucket's range (for refresh)
    NodeID getRandomIDInBucket(int bucket_index) const;

    // Remove bad nodes from routing table
    void cleanupBadNodes();

    // Get statistics
    struct Stats {
        size_t total_nodes = 0;
        size_t good_nodes = 0;
        size_t questionable_nodes = 0;
        size_t bad_nodes = 0;
        size_t filled_buckets = 0;
    };
    Stats getStats() const;

private:
    // Get bucket index for a node ID
    int getBucketIndex(const NodeID& id) const;

    // Try to replace questionable/bad node in bucket
    bool tryReplaceNode(int bucket_index, const Node& new_node);

    NodeID our_id_;
    std::array<Bucket, NUM_BUCKETS> buckets_;
    mutable std::mutex mutex_;
};

} // namespace dht
} // namespace torrent
