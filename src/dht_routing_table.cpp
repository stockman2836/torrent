#include "dht_routing_table.h"
#include <algorithm>
#include <random>

namespace torrent {
namespace dht {

// Bucket implementation

bool Bucket::addNode(const Node& node) {
    // Check if node already exists
    auto it = std::find_if(nodes_.begin(), nodes_.end(),
        [&](const Node& n) { return n.id() == node.id(); });

    if (it != nodes_.end()) {
        // Node exists, move to back (most recently seen)
        nodes_.erase(it);
        nodes_.push_back(node);
        last_changed_ = std::chrono::steady_clock::now();
        return true;
    }

    // Node doesn't exist
    if (nodes_.size() < BUCKET_SIZE) {
        // Bucket not full, add to back
        nodes_.push_back(node);
        last_changed_ = std::chrono::steady_clock::now();
        return true;
    }

    return false; // Bucket is full
}

bool Bucket::updateNode(const NodeID& id) {
    auto it = std::find_if(nodes_.begin(), nodes_.end(),
        [&](const Node& n) { return n.id() == id; });

    if (it != nodes_.end()) {
        // Move to back (most recently seen)
        Node node = *it;
        node.updateLastSeen();
        nodes_.erase(it);
        nodes_.push_back(node);
        return true;
    }

    return false;
}

bool Bucket::removeNode(const NodeID& id) {
    auto it = std::find_if(nodes_.begin(), nodes_.end(),
        [&](const Node& n) { return n.id() == id; });

    if (it != nodes_.end()) {
        nodes_.erase(it);
        last_changed_ = std::chrono::steady_clock::now();
        return true;
    }

    return false;
}

std::optional<Node> Bucket::findNode(const NodeID& id) const {
    auto it = std::find_if(nodes_.begin(), nodes_.end(),
        [&](const Node& n) { return n.id() == id; });

    if (it != nodes_.end()) {
        return *it;
    }

    return std::nullopt;
}

std::vector<Node> Bucket::getNodes() const {
    return std::vector<Node>(nodes_.begin(), nodes_.end());
}

std::vector<Node> Bucket::getGoodNodes() const {
    std::vector<Node> good_nodes;
    for (const auto& node : nodes_) {
        if (node.isGood()) {
            good_nodes.push_back(node);
        }
    }
    return good_nodes;
}

std::optional<Node> Bucket::getLeastRecentlySeen() const {
    if (nodes_.empty()) {
        return std::nullopt;
    }
    return nodes_.front(); // Front is least recently seen
}

bool Bucket::needsRefresh() const {
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - last_changed_
    ).count();
    return elapsed >= BUCKET_REFRESH_INTERVAL;
}

void Bucket::markAsRefreshed() {
    last_changed_ = std::chrono::steady_clock::now();
}

// RoutingTable implementation

RoutingTable::RoutingTable(const NodeID& our_id)
    : our_id_(our_id) {
    // Initialize all buckets
    for (auto& bucket : buckets_) {
        bucket.markAsRefreshed(); // Start with fresh timestamp
    }
}

void RoutingTable::addNode(const Node& node) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Don't add ourselves
    if (node.id() == our_id_) {
        return;
    }

    // Don't add bad nodes
    if (node.isBad()) {
        return;
    }

    int bucket_idx = getBucketIndex(node.id());
    if (bucket_idx < 0 || bucket_idx >= NUM_BUCKETS) {
        return;
    }

    Bucket& bucket = buckets_[bucket_idx];

    // Try to add node
    if (bucket.addNode(node)) {
        return; // Successfully added or updated
    }

    // Bucket is full, try to replace a bad/questionable node
    tryReplaceNode(bucket_idx, node);
}

void RoutingTable::removeNode(const NodeID& id) {
    std::lock_guard<std::mutex> lock(mutex_);

    int bucket_idx = getBucketIndex(id);
    if (bucket_idx >= 0 && bucket_idx < NUM_BUCKETS) {
        buckets_[bucket_idx].removeNode(id);
    }
}

void RoutingTable::updateNode(const NodeID& id) {
    std::lock_guard<std::mutex> lock(mutex_);

    int bucket_idx = getBucketIndex(id);
    if (bucket_idx >= 0 && bucket_idx < NUM_BUCKETS) {
        buckets_[bucket_idx].updateNode(id);
    }
}

std::optional<Node> RoutingTable::findNode(const NodeID& id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    int bucket_idx = getBucketIndex(id);
    if (bucket_idx >= 0 && bucket_idx < NUM_BUCKETS) {
        return buckets_[bucket_idx].findNode(id);
    }

    return std::nullopt;
}

std::vector<Node> RoutingTable::findClosestNodes(const NodeID& target, size_t count) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<Node> all_nodes;

    // Collect all good nodes
    for (const auto& bucket : buckets_) {
        auto nodes = bucket.getGoodNodes();
        all_nodes.insert(all_nodes.end(), nodes.begin(), nodes.end());
    }

    // If we don't have enough good nodes, add questionable ones
    if (all_nodes.size() < count) {
        for (const auto& bucket : buckets_) {
            auto nodes = bucket.getNodes();
            for (const auto& node : nodes) {
                if (node.isQuestionable()) {
                    all_nodes.push_back(node);
                }
            }
        }
    }

    // Sort by distance to target
    std::sort(all_nodes.begin(), all_nodes.end(),
        [&](const Node& a, const Node& b) {
            return NodeDistance::isCloser(target, a.id(), b.id());
        });

    // Return up to 'count' closest nodes
    if (all_nodes.size() > count) {
        all_nodes.resize(count);
    }

    return all_nodes;
}

std::vector<Node> RoutingTable::getAllNodes() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<Node> all_nodes;
    for (const auto& bucket : buckets_) {
        auto nodes = bucket.getNodes();
        all_nodes.insert(all_nodes.end(), nodes.begin(), nodes.end());
    }

    return all_nodes;
}

std::vector<Node> RoutingTable::getGoodNodes() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<Node> good_nodes;
    for (const auto& bucket : buckets_) {
        auto nodes = bucket.getGoodNodes();
        good_nodes.insert(good_nodes.end(), nodes.begin(), nodes.end());
    }

    return good_nodes;
}

size_t RoutingTable::size() const {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t total = 0;
    for (const auto& bucket : buckets_) {
        total += bucket.size();
    }

    return total;
}

std::vector<int> RoutingTable::getBucketsNeedingRefresh() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<int> buckets_to_refresh;

    for (size_t i = 0; i < NUM_BUCKETS; ++i) {
        if (buckets_[i].needsRefresh()) {
            buckets_to_refresh.push_back(static_cast<int>(i));
        }
    }

    return buckets_to_refresh;
}

void RoutingTable::markBucketRefreshed(int bucket_index) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (bucket_index >= 0 && bucket_index < NUM_BUCKETS) {
        buckets_[bucket_index].markAsRefreshed();
    }
}

NodeID RoutingTable::getRandomIDInBucket(int bucket_index) const {
    if (bucket_index < 0 || bucket_index >= NUM_BUCKETS) {
        return NodeDistance::generateRandomID();
    }

    // Generate random ID that falls in this bucket
    NodeID random_id = NodeDistance::generateRandomID();

    // Set bits to ensure it falls in the correct bucket
    // The bucket index represents which bit differs first
    int byte_idx = bucket_index / 8;
    int bit_idx = bucket_index % 8;

    // Copy our ID up to the differing bit
    NodeID result = our_id_;
    for (int i = 0; i < byte_idx; ++i) {
        result[i] = our_id_[i];
    }

    // Flip the differing bit
    result[byte_idx] = our_id_[byte_idx] ^ (1 << (7 - bit_idx));

    // Randomize remaining bits
    for (int i = byte_idx; i < NODE_ID_SIZE; ++i) {
        if (i == byte_idx) {
            // Keep the flipped bit, randomize the rest
            uint8_t mask = (1 << (7 - bit_idx)) - 1;
            result[i] = (result[i] & ~mask) | (random_id[i] & mask);
        } else {
            result[i] = random_id[i];
        }
    }

    return result;
}

void RoutingTable::cleanupBadNodes() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& bucket : buckets_) {
        auto nodes = bucket.getNodes();
        for (const auto& node : nodes) {
            if (node.isBad()) {
                bucket.removeNode(node.id());
            }
        }
    }
}

RoutingTable::Stats RoutingTable::getStats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    Stats stats;

    for (const auto& bucket : buckets_) {
        auto nodes = bucket.getNodes();
        if (!nodes.empty()) {
            stats.filled_buckets++;
        }

        for (const auto& node : nodes) {
            stats.total_nodes++;
            if (node.isGood()) {
                stats.good_nodes++;
            } else if (node.isQuestionable()) {
                stats.questionable_nodes++;
            } else if (node.isBad()) {
                stats.bad_nodes++;
            }
        }
    }

    return stats;
}

int RoutingTable::getBucketIndex(const NodeID& id) const {
    return NodeDistance::bucketIndex(our_id_, id);
}

bool RoutingTable::tryReplaceNode(int bucket_index, const Node& new_node) {
    Bucket& bucket = buckets_[bucket_index];

    // Get all nodes in bucket
    auto nodes = bucket.getNodes();

    // Try to find a bad node to replace
    for (const auto& node : nodes) {
        if (node.isBad()) {
            bucket.removeNode(node.id());
            bucket.addNode(new_node);
            return true;
        }
    }

    // Try to find a questionable node to replace
    for (const auto& node : nodes) {
        if (node.isQuestionable()) {
            // In real implementation, we should ping the questionable node first
            // For now, just replace it
            bucket.removeNode(node.id());
            bucket.addNode(new_node);
            return true;
        }
    }

    return false; // No node to replace
}

} // namespace dht
} // namespace torrent
