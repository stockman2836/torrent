#include "dht_manager.h"
#include "logger.h"
#include "utils.h"
#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#endif

namespace torrent {
namespace dht {

// Default bootstrap nodes (BitTorrent Inc.)
static const std::vector<BootstrapNode> DEFAULT_BOOTSTRAP_NODES = {
    {"router.bittorrent.com", 6881},
    {"dht.transmissionbt.com", 6881},
    {"router.utorrent.com", 6881}
};

// PeerStorage implementation

void DHTManager::PeerStorage::addPeer(const std::vector<uint8_t>& info_hash,
                                      const DHTpeer& peer) {
    std::lock_guard<std::mutex> lock(mutex);

    auto& peer_list = peers[info_hash];

    // Check if peer already exists
    auto it = std::find_if(peer_list.begin(), peer_list.end(),
        [&](const DHTpeer& p) { return p.ip == peer.ip && p.port == peer.port; });

    if (it == peer_list.end()) {
        peer_list.push_back(peer);

        // Limit to 200 peers per info_hash
        if (peer_list.size() > 200) {
            peer_list.erase(peer_list.begin());
        }
    }
}

std::vector<DHTpeer> DHTManager::PeerStorage::getPeers(
    const std::vector<uint8_t>& info_hash) const {

    std::lock_guard<std::mutex> lock(mutex);

    auto it = peers.find(info_hash);
    if (it != peers.end()) {
        return it->second;
    }
    return {};
}

// DHTManager implementation

DHTManager::DHTManager(uint16_t port)
    : our_id_(NodeDistance::generateRandomID())
    , port_(port)
    , socket_fd_(-1)
    , routing_table_(std::make_unique<RoutingTable>(our_id_))
    , running_(false)
    , queries_sent_(0)
    , responses_received_(0)
    , errors_received_(0) {

    // Initialize token secret
    token_secret_ = utils::generatePeerId().substr(0, 8);
    last_token_rotation_ = std::chrono::steady_clock::now();

#ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif
}

DHTManager::~DHTManager() {
    stop();

#ifdef _WIN32
    WSACleanup();
#endif
}

void DHTManager::start() {
    if (running_) {
        return;
    }

    LOG_INFO("Starting DHT on port {}", port_);

    // Create UDP socket
    socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0) {
        LOG_ERROR("Failed to create DHT socket");
        return;
    }

    // Set socket to non-blocking
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(socket_fd_, FIONBIO, &mode);
#else
    int flags = fcntl(socket_fd_, F_GETFL, 0);
    fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK);
#endif

    // Bind socket
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(socket_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Failed to bind DHT socket to port {}", port_);
#ifdef _WIN32
        closesocket(socket_fd_);
#else
        close(socket_fd_);
#endif
        socket_fd_ = -1;
        return;
    }

    running_ = true;

    // Start threads
    network_thread_ = std::thread(&DHTManager::networkLoop, this);
    maintenance_thread_ = std::thread(&DHTManager::maintenanceLoop, this);

    LOG_INFO("DHT started with node ID: {}", NodeDistance::toHex(our_id_).substr(0, 16) + "...");
}

void DHTManager::stop() {
    if (!running_) {
        return;
    }

    LOG_INFO("Stopping DHT");

    running_ = false;

    if (network_thread_.joinable()) {
        network_thread_.join();
    }

    if (maintenance_thread_.joinable()) {
        maintenance_thread_.join();
    }

    if (socket_fd_ >= 0) {
#ifdef _WIN32
        closesocket(socket_fd_);
#else
        close(socket_fd_);
#endif
        socket_fd_ = -1;
    }

    LOG_INFO("DHT stopped");
}

void DHTManager::bootstrap(const std::vector<BootstrapNode>& bootstrap_nodes) {
    LOG_INFO("Bootstrapping DHT with {} nodes", bootstrap_nodes.size());

    auto nodes_to_use = bootstrap_nodes.empty() ? DEFAULT_BOOTSTRAP_NODES : bootstrap_nodes;

    for (const auto& bn : nodes_to_use) {
        // Resolve hostname
        struct addrinfo hints, *result;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;

        std::string port_str = std::to_string(bn.port);
        if (getaddrinfo(bn.host.c_str(), port_str.c_str(), &hints, &result) == 0) {
            struct sockaddr_in* addr = (struct sockaddr_in*)result->ai_addr;
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &addr->sin_addr, ip_str, INET_ADDRSTRLEN);

            // Create a temporary node for bootstrap
            NodeID bootstrap_id = NodeDistance::generateRandomID();
            Node bootstrap_node(bootstrap_id, ip_str, bn.port);

            LOG_DEBUG("Bootstrapping from {}:{}", ip_str, bn.port);

            // Send find_node to discover nodes close to our ID
            findNode(bootstrap_node, our_id_,
                [this](const KRPCResponse& response) {
                    if (response.nodes.has_value()) {
                        for (const auto& node : response.nodes.value()) {
                            routing_table_->addNode(node);
                        }
                        LOG_DEBUG("Bootstrap: received {} nodes", response.nodes.value().size());
                    }
                },
                []() {
                    LOG_DEBUG("Bootstrap: find_node timeout");
                }
            );

            freeaddrinfo(result);
        } else {
            LOG_WARN("Failed to resolve bootstrap node: {}", bn.host);
        }
    }

    // Wait a bit for responses
    std::this_thread::sleep_for(std::chrono::seconds(2));

    LOG_INFO("Bootstrap complete. Routing table size: {}", routing_table_->size());
}

std::vector<DHTpeer> DHTManager::getPeers(const std::vector<uint8_t>& info_hash,
                                          int timeout_seconds) {
    LOG_DEBUG("DHT: Looking up peers for info_hash");

    std::vector<DHTpeer> found_peers;
    std::mutex peers_mutex;
    std::atomic<int> pending_requests(0);

    auto start_time = std::chrono::steady_clock::now();

    // Get closest nodes to info_hash
    NodeID target;
    if (info_hash.size() == NODE_ID_SIZE) {
        std::copy(info_hash.begin(), info_hash.end(), target.begin());
    }

    auto closest_nodes = routing_table_->findClosestNodes(target, 8);

    if (closest_nodes.empty()) {
        LOG_WARN("DHT: No nodes in routing table for get_peers");
        return found_peers;
    }

    pending_requests = closest_nodes.size();

    // Send get_peers to closest nodes
    for (const auto& node : closest_nodes) {
        getPeersFromNode(node, info_hash,
            [&, info_hash](const KRPCResponse& response) {
                // Check for peer values
                if (response.values.has_value()) {
                    std::lock_guard<std::mutex> lock(peers_mutex);
                    for (const auto& val : response.values.value()) {
                        if (val.size() == 6) {
                            // Compact peer format: 4 bytes IP + 2 bytes port
                            DHTpeer peer;
                            struct in_addr addr;
                            addr.s_addr = (val[0] << 0) | (val[1] << 8) |
                                        (val[2] << 16) | (val[3] << 24);
                            char ip_str[INET_ADDRSTRLEN];
                            inet_ntop(AF_INET, &addr, ip_str, INET_ADDRSTRLEN);
                            peer.ip = ip_str;
                            peer.port = (static_cast<uint16_t>(val[4]) << 8) | val[5];
                            found_peers.push_back(peer);
                        }
                    }
                }

                // Check for nodes (continue searching)
                if (response.nodes.has_value()) {
                    for (const auto& node : response.nodes.value()) {
                        routing_table_->addNode(node);
                    }
                }

                pending_requests--;
            },
            [&]() {
                pending_requests--;
            }
        );
    }

    // Wait for responses or timeout
    while (pending_requests > 0) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_time
        ).count();

        if (elapsed >= timeout_seconds) {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    LOG_INFO("DHT: Found {} peers", found_peers.size());
    return found_peers;
}

void DHTManager::announcePeer(const std::vector<uint8_t>& info_hash, uint16_t port) {
    LOG_DEBUG("DHT: Announcing as peer for info_hash on port {}", port);

    // Get closest nodes to info_hash
    NodeID target;
    if (info_hash.size() == NODE_ID_SIZE) {
        std::copy(info_hash.begin(), info_hash.end(), target.begin());
    }

    auto closest_nodes = routing_table_->findClosestNodes(target, 8);

    if (closest_nodes.empty()) {
        LOG_WARN("DHT: No nodes in routing table for announce_peer");
        return;
    }

    // First get_peers to obtain tokens, then announce
    for (const auto& node : closest_nodes) {
        getPeersFromNode(node, info_hash,
            [this, node, info_hash, port](const KRPCResponse& response) {
                if (response.token.has_value()) {
                    // Now announce with the token
                    announcePeerToNode(node, info_hash, port, response.token.value(),
                        [](const KRPCResponse&) {
                            LOG_DEBUG("DHT: announce_peer successful");
                        },
                        []() {
                            LOG_DEBUG("DHT: announce_peer timeout");
                        }
                    );
                }
            },
            []() {}
        );
    }
}

size_t DHTManager::getNodeCount() const {
    return routing_table_->size();
}

RoutingTable::Stats DHTManager::getRoutingStats() const {
    return routing_table_->getStats();
}

// Network operations

void DHTManager::networkLoop() {
    std::vector<uint8_t> buffer(65536);

    while (running_) {
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);

        int received = recvfrom(socket_fd_, (char*)buffer.data(), buffer.size(), 0,
                               (struct sockaddr*)&from_addr, &from_len);

        if (received > 0) {
            std::vector<uint8_t> data(buffer.begin(), buffer.begin() + received);

            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &from_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
            uint16_t from_port = ntohs(from_addr.sin_port);

            handleIncomingMessage(data, ip_str, from_port);
        }

        // Check transaction timeouts
        checkTransactionTimeouts();

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool DHTManager::sendMessage(const KRPCMessage& message,
                            const std::string& ip,
                            uint16_t port) {
    std::vector<uint8_t> data = KRPCMessageFactory::serialize(message);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
        LOG_ERROR("Invalid IP address: {}", ip);
        return false;
    }

    int sent = sendto(socket_fd_, (char*)data.data(), data.size(), 0,
                     (struct sockaddr*)&addr, sizeof(addr));

    return sent == static_cast<int>(data.size());
}

void DHTManager::handleIncomingMessage(const std::vector<uint8_t>& data,
                                      const std::string& from_ip,
                                      uint16_t from_port) {
    try {
        auto message = KRPCMessageFactory::parse(data);

        if (std::holds_alternative<KRPCQuery>(message)) {
            handleQuery(std::get<KRPCQuery>(message), from_ip, from_port);
        } else if (std::holds_alternative<KRPCResponse>(message)) {
            handleResponse(std::get<KRPCResponse>(message));
        } else if (std::holds_alternative<KRPCError>(message)) {
            handleError(std::get<KRPCError>(message));
        }
    } catch (const std::exception& e) {
        LOG_DEBUG("DHT: Failed to parse message from {}:{} - {}",
                 from_ip, from_port, e.what());
    }
}

void DHTManager::handleQuery(const KRPCQuery& query,
                            const std::string& from_ip,
                            uint16_t from_port) {
    // Add querying node to routing table
    Node sender(query.sender_id, from_ip, from_port);
    sender.markGood();
    routing_table_->addNode(sender);

    switch (query.query_type) {
        case QueryType::PING:
            handlePing(query, from_ip, from_port);
            break;
        case QueryType::FIND_NODE:
            handleFindNode(query, from_ip, from_port);
            break;
        case QueryType::GET_PEERS:
            handleGetPeers(query, from_ip, from_port);
            break;
        case QueryType::ANNOUNCE_PEER:
            handleAnnouncePeer(query, from_ip, from_port);
            break;
    }
}

void DHTManager::handleResponse(const KRPCResponse& response) {
    responses_received_++;

    // Update node in routing table
    Node sender(response.sender_id, "0.0.0.0", 0); // IP/port from transaction
    sender.markGood();
    routing_table_->updateNode(response.sender_id);

    // Find and execute callback
    std::lock_guard<std::mutex> lock(transactions_mutex_);
    auto it = pending_transactions_.find(response.transaction_id);
    if (it != pending_transactions_.end()) {
        if (it->second.on_response) {
            it->second.on_response(response);
        }
        pending_transactions_.erase(it);
    }
}

void DHTManager::handleError(const KRPCError& error) {
    errors_received_++;
    LOG_DEBUG("DHT: Received error {} - {}", static_cast<int>(error.error_code),
             error.error_message);

    // Remove transaction
    removeTransaction(error.transaction_id);
}

// Query handlers

void DHTManager::handlePing(const KRPCQuery& query,
                           const std::string& from_ip,
                           uint16_t from_port) {
    auto response = KRPCMessageFactory::createPingResponse(our_id_, query.transaction_id);
    sendMessage(response, from_ip, from_port);
}

void DHTManager::handleFindNode(const KRPCQuery& query,
                               const std::string& from_ip,
                               uint16_t from_port) {
    if (!query.target.has_value()) {
        return;
    }

    auto closest = routing_table_->findClosestNodes(query.target.value(), BUCKET_SIZE);
    auto response = KRPCMessageFactory::createFindNodeResponse(our_id_, closest,
                                                               query.transaction_id);
    sendMessage(response, from_ip, from_port);
}

void DHTManager::handleGetPeers(const KRPCQuery& query,
                               const std::string& from_ip,
                               uint16_t from_port) {
    if (!query.info_hash.has_value()) {
        return;
    }

    std::string token = generateToken(from_ip, query.info_hash.value());

    // Check if we have peers for this info_hash
    auto peers = peer_storage_.getPeers(query.info_hash.value());

    if (!peers.empty()) {
        // Return peer values
        std::vector<std::string> values;
        for (const auto& peer : peers) {
            // Compact format: 4 bytes IP + 2 bytes port
            std::vector<uint8_t> compact(6);
            struct in_addr addr;
            inet_pton(AF_INET, peer.ip.c_str(), &addr);
            uint32_t ip_network = addr.s_addr;
            compact[0] = (ip_network >> 0) & 0xFF;
            compact[1] = (ip_network >> 8) & 0xFF;
            compact[2] = (ip_network >> 16) & 0xFF;
            compact[3] = (ip_network >> 24) & 0xFF;
            compact[4] = (peer.port >> 8) & 0xFF;
            compact[5] = peer.port & 0xFF;
            values.push_back(std::string(compact.begin(), compact.end()));
        }

        auto response = KRPCMessageFactory::createGetPeersResponseValues(our_id_, values,
                                                                         token,
                                                                         query.transaction_id);
        sendMessage(response, from_ip, from_port);
    } else {
        // Return closest nodes
        NodeID target;
        std::copy(query.info_hash->begin(), query.info_hash->end(), target.begin());
        auto closest = routing_table_->findClosestNodes(target, BUCKET_SIZE);

        auto response = KRPCMessageFactory::createGetPeersResponseNodes(our_id_, closest,
                                                                        token,
                                                                        query.transaction_id);
        sendMessage(response, from_ip, from_port);
    }
}

void DHTManager::handleAnnouncePeer(const KRPCQuery& query,
                                   const std::string& from_ip,
                                   uint16_t from_port) {
    if (!query.info_hash.has_value() || !query.token.has_value() || !query.port.has_value()) {
        return;
    }

    // Verify token
    if (!verifyToken(query.token.value(), from_ip, query.info_hash.value())) {
        auto error = KRPCMessageFactory::createError(ErrorCode::PROTOCOL_ERROR,
                                                     "Invalid token",
                                                     query.transaction_id);
        sendMessage(error, from_ip, from_port);
        return;
    }

    // Store peer
    DHTpeer peer;
    peer.ip = from_ip;
    peer.port = query.implied_port.value_or(0) != 0 ? from_port : query.port.value();
    peer_storage_.addPeer(query.info_hash.value(), peer);

    LOG_DEBUG("DHT: Stored peer {}:{} for info_hash", peer.ip, peer.port);

    // Send response
    auto response = KRPCMessageFactory::createPingResponse(our_id_, query.transaction_id);
    sendMessage(response, from_ip, from_port);
}

// DHT operations (continued in next part due to length)

void DHTManager::ping(const Node& node,
                     std::function<void(const KRPCResponse&)> on_response,
                     std::function<void()> on_timeout) {
    std::string tid = KRPCMessageFactory::generateTransactionId();
    auto query = KRPCMessageFactory::createPing(our_id_, tid);

    addTransaction(tid, QueryType::PING, node.id(), on_response, on_timeout);
    sendMessage(query, node.ip(), node.port());
    queries_sent_++;
}

void DHTManager::findNode(const Node& node,
                         const NodeID& target,
                         std::function<void(const KRPCResponse&)> on_response,
                         std::function<void()> on_timeout) {
    std::string tid = KRPCMessageFactory::generateTransactionId();
    auto query = KRPCMessageFactory::createFindNode(our_id_, target, tid);

    addTransaction(tid, QueryType::FIND_NODE, node.id(), on_response, on_timeout);
    sendMessage(query, node.ip(), node.port());
    queries_sent_++;
}

void DHTManager::getPeersFromNode(const Node& node,
                                 const std::vector<uint8_t>& info_hash,
                                 std::function<void(const KRPCResponse&)> on_response,
                                 std::function<void()> on_timeout) {
    std::string tid = KRPCMessageFactory::generateTransactionId();
    auto query = KRPCMessageFactory::createGetPeers(our_id_, info_hash, tid);

    addTransaction(tid, QueryType::GET_PEERS, node.id(), on_response, on_timeout);
    sendMessage(query, node.ip(), node.port());
    queries_sent_++;
}

void DHTManager::announcePeerToNode(const Node& node,
                                   const std::vector<uint8_t>& info_hash,
                                   uint16_t port,
                                   const std::string& token,
                                   std::function<void(const KRPCResponse&)> on_response,
                                   std::function<void()> on_timeout) {
    std::string tid = KRPCMessageFactory::generateTransactionId();
    auto query = KRPCMessageFactory::createAnnouncePeer(our_id_, info_hash, port, token, tid);

    addTransaction(tid, QueryType::ANNOUNCE_PEER, node.id(), on_response, on_timeout);
    sendMessage(query, node.ip(), node.port());
    queries_sent_++;
}

// Transaction management

void DHTManager::addTransaction(const std::string& tid,
                                QueryType type,
                                const NodeID& target_node,
                                std::function<void(const KRPCResponse&)> on_response,
                                std::function<void()> on_timeout) {
    std::lock_guard<std::mutex> lock(transactions_mutex_);

    Transaction trans;
    trans.id = tid;
    trans.type = type;
    trans.target_node = target_node;
    trans.sent_time = std::chrono::steady_clock::now();
    trans.on_response = on_response;
    trans.on_timeout = on_timeout;

    pending_transactions_[tid] = trans;
}

void DHTManager::removeTransaction(const std::string& tid) {
    std::lock_guard<std::mutex> lock(transactions_mutex_);
    pending_transactions_.erase(tid);
}

void DHTManager::checkTransactionTimeouts() {
    std::lock_guard<std::mutex> lock(transactions_mutex_);

    auto now = std::chrono::steady_clock::now();
    std::vector<std::string> to_remove;

    for (auto& [tid, trans] : pending_transactions_) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - trans.sent_time
        ).count();

        if (elapsed >= 15) { // 15 second timeout
            if (trans.on_timeout) {
                trans.on_timeout();
            }
            to_remove.push_back(tid);
        }
    }

    for (const auto& tid : to_remove) {
        pending_transactions_.erase(tid);
    }
}

// Token management

std::string DHTManager::generateToken(const std::string& ip,
                                     const std::vector<uint8_t>& info_hash) {
    std::string data = ip + token_secret_;
    data.insert(data.end(), info_hash.begin(), info_hash.end());

    auto hash = utils::sha1(std::vector<uint8_t>(data.begin(), data.end()));
    return std::string(hash.begin(), hash.begin() + 8); // Use first 8 bytes
}

bool DHTManager::verifyToken(const std::string& token,
                            const std::string& ip,
                            const std::vector<uint8_t>& info_hash) {
    // Check with current secret
    if (token == generateToken(ip, info_hash)) {
        return true;
    }

    // Check with old secret (for rotation tolerance)
    if (!old_token_secret_.empty()) {
        std::string old_token_secret_backup = token_secret_;
        token_secret_ = old_token_secret_;
        bool valid = token == generateToken(ip, info_hash);
        token_secret_ = old_token_secret_backup;
        return valid;
    }

    return false;
}

// Maintenance

void DHTManager::maintenanceLoop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(60));

        if (!running_) break;

        // Refresh buckets
        refreshBuckets();

        // Cleanup bad nodes
        cleanupBadNodes();

        // Rotate token secret every hour
        auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(
            std::chrono::steady_clock::now() - last_token_rotation_
        ).count();

        if (elapsed >= 60) {
            old_token_secret_ = token_secret_;
            token_secret_ = utils::generatePeerId().substr(0, 8);
            last_token_rotation_ = std::chrono::steady_clock::now();
        }

        // Log statistics
        auto stats = routing_table_->getStats();
        LOG_DEBUG("DHT stats: {} nodes ({} good, {} questionable, {} bad), {} buckets",
                 stats.total_nodes, stats.good_nodes, stats.questionable_nodes,
                 stats.bad_nodes, stats.filled_buckets);
    }
}

void DHTManager::refreshBuckets() {
    auto buckets = routing_table_->getBucketsNeedingRefresh();

    for (int bucket_idx : buckets) {
        NodeID random_target = routing_table_->getRandomIDInBucket(bucket_idx);

        auto closest_nodes = routing_table_->findClosestNodes(random_target, 3);

        for (const auto& node : closest_nodes) {
            findNode(node, random_target,
                [this](const KRPCResponse& response) {
                    if (response.nodes.has_value()) {
                        for (const auto& node : response.nodes.value()) {
                            routing_table_->addNode(node);
                        }
                    }
                },
                []() {}
            );
        }

        routing_table_->markBucketRefreshed(bucket_idx);
    }
}

void DHTManager::cleanupBadNodes() {
    routing_table_->cleanupBadNodes();
}

} // namespace dht
} // namespace torrent
