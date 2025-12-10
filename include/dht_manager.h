#pragma once

#include "dht_node.h"
#include "dht_routing_table.h"
#include "dht_krpc.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include <chrono>

namespace torrent {
namespace dht {

// Bootstrap node information
struct BootstrapNode {
    std::string host;
    uint16_t port;

    BootstrapNode(const std::string& h, uint16_t p) : host(h), port(p) {}
};

// Pending transaction
struct Transaction {
    std::string id;
    QueryType type;
    NodeID target_node;
    std::chrono::steady_clock::time_point sent_time;
    int retry_count = 0;

    // Callback for response
    std::function<void(const KRPCResponse&)> on_response;
    std::function<void()> on_timeout;
};

// Peer info from get_peers
struct DHTpeer {
    std::string ip;
    uint16_t port;
};

// DHT Manager
class DHTManager {
public:
    DHTManager(uint16_t port = 6881);
    ~DHTManager();

    // Lifecycle
    void start();
    void stop();
    bool isRunning() const { return running_; }

    // Bootstrap DHT network
    void bootstrap(const std::vector<BootstrapNode>& bootstrap_nodes);

    // Find peers for info_hash
    std::vector<DHTpeer> getPeers(const std::vector<uint8_t>& info_hash,
                                  int timeout_seconds = 30);

    // Announce ourselves as a peer for info_hash
    void announcePeer(const std::vector<uint8_t>& info_hash, uint16_t port);

    // Statistics
    size_t getNodeCount() const;
    RoutingTable::Stats getRoutingStats() const;

    // Get our node ID
    const NodeID& getNodeId() const { return our_id_; }

private:
    // Network operations
    void networkLoop();
    bool sendMessage(const KRPCMessage& message, const std::string& ip, uint16_t port);
    void handleIncomingMessage(const std::vector<uint8_t>& data,
                              const std::string& from_ip,
                              uint16_t from_port);

    // Message handlers
    void handleQuery(const KRPCQuery& query,
                    const std::string& from_ip,
                    uint16_t from_port);
    void handleResponse(const KRPCResponse& response);
    void handleError(const KRPCError& error);

    // Query handlers
    void handlePing(const KRPCQuery& query,
                   const std::string& from_ip,
                   uint16_t from_port);
    void handleFindNode(const KRPCQuery& query,
                       const std::string& from_ip,
                       uint16_t from_port);
    void handleGetPeers(const KRPCQuery& query,
                       const std::string& from_ip,
                       uint16_t from_port);
    void handleAnnouncePeer(const KRPCQuery& query,
                           const std::string& from_ip,
                           uint16_t from_port);

    // DHT operations
    void ping(const Node& node,
             std::function<void(const KRPCResponse&)> on_response = nullptr,
             std::function<void()> on_timeout = nullptr);

    void findNode(const Node& node,
                 const NodeID& target,
                 std::function<void(const KRPCResponse&)> on_response = nullptr,
                 std::function<void()> on_timeout = nullptr);

    void getPeersFromNode(const Node& node,
                         const std::vector<uint8_t>& info_hash,
                         std::function<void(const KRPCResponse&)> on_response = nullptr,
                         std::function<void()> on_timeout = nullptr);

    void announcePeerToNode(const Node& node,
                           const std::vector<uint8_t>& info_hash,
                           uint16_t port,
                           const std::string& token,
                           std::function<void(const KRPCResponse&)> on_response = nullptr,
                           std::function<void()> on_timeout = nullptr);

    // Transaction management
    void addTransaction(const std::string& tid,
                       QueryType type,
                       const NodeID& target_node,
                       std::function<void(const KRPCResponse&)> on_response,
                       std::function<void()> on_timeout);
    void removeTransaction(const std::string& tid);
    void checkTransactionTimeouts();

    // Token management (for announce_peer)
    std::string generateToken(const std::string& ip, const std::vector<uint8_t>& info_hash);
    bool verifyToken(const std::string& token,
                    const std::string& ip,
                    const std::vector<uint8_t>& info_hash);

    // Maintenance
    void maintenanceLoop();
    void refreshBuckets();
    void cleanupBadNodes();

    // Peer storage (for serving get_peers)
    struct PeerStorage {
        std::map<std::vector<uint8_t>, std::vector<DHTpeer>> peers;
        std::mutex mutex;

        void addPeer(const std::vector<uint8_t>& info_hash, const DHTpeer& peer);
        std::vector<DHTpeer> getPeers(const std::vector<uint8_t>& info_hash) const;
    };

    // State
    NodeID our_id_;
    uint16_t port_;
    int socket_fd_;

    std::unique_ptr<RoutingTable> routing_table_;
    PeerStorage peer_storage_;

    // Transaction tracking
    std::map<std::string, Transaction> pending_transactions_;
    std::mutex transactions_mutex_;

    // Token secret (rotated periodically)
    std::string token_secret_;
    std::string old_token_secret_;
    std::chrono::steady_clock::time_point last_token_rotation_;

    // Threading
    std::atomic<bool> running_;
    std::thread network_thread_;
    std::thread maintenance_thread_;

    // Statistics
    std::atomic<int64_t> queries_sent_;
    std::atomic<int64_t> responses_received_;
    std::atomic<int64_t> errors_received_;
};

} // namespace dht
} // namespace torrent
