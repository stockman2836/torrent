#include "torrent/network/dht/dht_manager.h"
#include "torrent/core/logger.h"
#include "torrent/core/utils.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <fcntl.h>
#endif

namespace torrent::dht {
namespace {
const std::vector<BootstrapNode> default_routers = {
    {"router.bittorrent.com", 6881}, {"dht.transmissionbt.com", 6881}, {"router.utorrent.com", 6881}
};
void closeSocket(std::intptr_t socket) {
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}
}

void DHTManager::PeerStorage::addPeer(const std::vector<uint8_t>& hash, const DHTpeer& peer) {
    std::lock_guard<std::mutex> lock(mutex);
    auto& list = peers[hash];
    if (std::none_of(list.begin(), list.end(), [&](const auto& p) { return p.ip == peer.ip && p.port == peer.port; })) {
        list.push_back(peer);
        if (list.size() > 200) list.erase(list.begin());
    }
}

std::vector<DHTpeer> DHTManager::PeerStorage::getPeers(const std::vector<uint8_t>& hash) const {
    std::lock_guard<std::mutex> lock(mutex);
    const auto entry = peers.find(hash);
    return entry == peers.end() ? std::vector<DHTpeer>{} : entry->second;
}

DHTManager::DHTManager(uint16_t port)
    : our_id_(NodeDistance::generateRandomID()), port_(port), socket_fd_(-1),
      routing_table_(std::make_unique<RoutingTable>(our_id_)),
      token_secret_(utils::generatePeerId().substr(8)), last_token_rotation_(std::chrono::steady_clock::now()),
      running_(false), queries_sent_(0), responses_received_(0), errors_received_(0) {
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("Cannot initialize DHT sockets");
#endif
}

DHTManager::~DHTManager() {
    stop();
#ifdef _WIN32
    WSACleanup();
#endif
}

void DHTManager::start() {
    if (running_) return;
    pending_bootstrap_ = 0;
    socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ == -1) throw std::runtime_error("Cannot create DHT socket");
    sockaddr_in address{};
    address.sin_family = AF_INET; address.sin_port = htons(port_); address.sin_addr.s_addr = INADDR_ANY;
#ifdef _WIN32
    BOOL exclusive = TRUE;
    const bool reserved = setsockopt(socket_fd_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
        reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == 0;
    u_long mode = 1;
    const bool nonblocking = reserved && ioctlsocket(socket_fd_, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(socket_fd_, F_GETFL, 0);
    const bool nonblocking = flags >= 0 && fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
    if (!nonblocking || bind(socket_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        closeSocket(socket_fd_); socket_fd_ = -1;
        throw std::runtime_error("Cannot bind DHT UDP port " + std::to_string(port_));
    }
    socklen_t length = sizeof(address);
    getsockname(socket_fd_, reinterpret_cast<sockaddr*>(&address), &length);
    port_ = ntohs(address.sin_port);
    running_ = true;
    network_thread_ = std::thread(&DHTManager::networkLoop, this);
    maintenance_thread_ = std::thread(&DHTManager::maintenanceLoop, this);
    LOG_INFO("DHT started on UDP port {}", port_);
}

void DHTManager::stop() {
    if (!running_.exchange(false)) return;
    if (network_thread_.joinable()) network_thread_.join();
    if (maintenance_thread_.joinable()) maintenance_thread_.join();
    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        closeSocket(socket_fd_); socket_fd_ = -1;
    }
    std::lock_guard<std::mutex> lock(transactions_mutex_);
    pending_transactions_.clear();
}

void DHTManager::bootstrap(const std::vector<BootstrapNode>& routers, const std::function<bool()>& cancelled) {
    for (const auto& router : routers.empty() ? default_routers : routers) {
        if (!running_ || (cancelled && cancelled())) break;
        addrinfo hints{}, *addresses = nullptr;
        hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(router.host.c_str(), std::to_string(router.port).c_str(), &hints, &addresses) != 0) {
            LOG_WARN("Cannot resolve DHT router {}", router.host); continue;
        }
        for (auto* address = addresses; address && running_ && !(cancelled && cancelled()); address = address->ai_next) {
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(address->ai_addr)->sin_addr, ip, sizeof(ip));
            ++pending_bootstrap_;
            findNode(Node(NodeDistance::generateRandomID(), ip, router.port), our_id_,
                [this](const KRPCResponse& response) {
                    if (response.nodes) for (const auto& node : *response.nodes) if (node.port()) routing_table_->addNode(node);
                    --pending_bootstrap_;
                }, [this] { --pending_bootstrap_; });
        }
        freeaddrinfo(addresses);
    }
}

size_t DHTManager::getNodeCount() const { return routing_table_->size(); }
RoutingTable::Stats DHTManager::getRoutingStats() const { return routing_table_->getStats(); }

void DHTManager::networkLoop() {
    std::vector<uint8_t> buffer(65536);
    while (running_) {
        sockaddr_in sender{}; socklen_t length = sizeof(sender);
        const int received = recvfrom(socket_fd_, reinterpret_cast<char*>(buffer.data()), buffer.size(), 0,
                                      reinterpret_cast<sockaddr*>(&sender), &length);
        if (received > 0) {
            char ip[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &sender.sin_addr, ip, sizeof(ip));
            handleIncomingMessage({buffer.begin(), buffer.begin() + received}, ip, ntohs(sender.sin_port));
        }
        checkTransactionTimeouts();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool DHTManager::sendMessage(const KRPCMessage& message, const std::string& ip, uint16_t port) {
    const auto bytes = KRPCMessageFactory::serialize(message);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port);
    if (!port || inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) return false;
    std::lock_guard<std::mutex> lock(socket_mutex_);
    if (!running_) return false;
    return sendto(socket_fd_, reinterpret_cast<const char*>(bytes.data()), bytes.size(), 0,
                  reinterpret_cast<sockaddr*>(&address), sizeof(address)) == static_cast<int>(bytes.size());
}

void DHTManager::handleIncomingMessage(const std::vector<uint8_t>& bytes, const std::string& ip, uint16_t port) {
    try {
        const auto message = KRPCMessageFactory::parse(bytes);
        if (const auto* query = std::get_if<KRPCQuery>(&message)) handleQuery(*query, ip, port);
        else if (const auto* response = std::get_if<KRPCResponse>(&message)) handleResponse(*response, ip, port);
        else handleError(std::get<KRPCError>(message), ip, port);
    } catch (const std::exception& error) { LOG_DEBUG("Ignoring invalid DHT packet: {}", error.what()); }
}

void DHTManager::handleResponse(const KRPCResponse& response, const std::string& ip, uint16_t port) {
    std::function<void(const KRPCResponse&)> callback;
    {
        std::lock_guard<std::mutex> lock(transactions_mutex_);
        const auto transaction = pending_transactions_.find(response.transaction_id);
        if (transaction == pending_transactions_.end() || transaction->second.ip != ip || transaction->second.port != port) return;
        callback = std::move(transaction->second.on_response);
        pending_transactions_.erase(transaction);
    }
    ++responses_received_;
    Node sender(response.sender_id, ip, port); sender.markGood(); routing_table_->addNode(sender);
    // Callbacks may submit follow-up queries. Never run them under the map lock.
    if (callback) callback(response);
}

void DHTManager::handleError(const KRPCError& error, const std::string& ip, uint16_t port) {
    std::function<void()> callback;
    {
        std::lock_guard<std::mutex> lock(transactions_mutex_);
        const auto transaction = pending_transactions_.find(error.transaction_id);
        if (transaction == pending_transactions_.end() || transaction->second.ip != ip || transaction->second.port != port) return;
        callback = std::move(transaction->second.on_timeout);
        pending_transactions_.erase(transaction);
    }
    ++errors_received_;
    if (callback) callback();
}

void DHTManager::handleQuery(const KRPCQuery& query, const std::string& ip, uint16_t port) {
    Node sender(query.sender_id, ip, port); routing_table_->addNode(sender);
    switch (query.query_type) {
        case QueryType::PING: handlePing(query, ip, port); break;
        case QueryType::FIND_NODE: handleFindNode(query, ip, port); break;
        case QueryType::GET_PEERS: handleGetPeers(query, ip, port); break;
        case QueryType::ANNOUNCE_PEER: handleAnnouncePeer(query, ip, port); break;
    }
}

void DHTManager::handlePing(const KRPCQuery& query, const std::string& ip, uint16_t port) {
    sendMessage(KRPCMessageFactory::createPingResponse(our_id_, query.transaction_id), ip, port);
}
void DHTManager::handleFindNode(const KRPCQuery& query, const std::string& ip, uint16_t port) {
    if (!query.target) return;
    sendMessage(KRPCMessageFactory::createFindNodeResponse(our_id_, routing_table_->findClosestNodes(*query.target, 8), query.transaction_id), ip, port);
}
void DHTManager::handleGetPeers(const KRPCQuery& query, const std::string& ip, uint16_t port) {
    if (!query.info_hash) return;
    const auto token = generateToken(ip, *query.info_hash);
    const auto peers = peer_storage_.getPeers(*query.info_hash);
    if (peers.empty()) {
        NodeID target{}; std::copy(query.info_hash->begin(), query.info_hash->end(), target.begin());
        sendMessage(KRPCMessageFactory::createGetPeersResponseNodes(our_id_, routing_table_->findClosestNodes(target, 8), token, query.transaction_id), ip, port);
    } else {
        std::vector<std::string> values;
        for (const auto& peer : peers) {
            std::string compact(6, '\0'); in_addr address{};
            inet_pton(AF_INET, peer.ip.c_str(), &address);
            std::memcpy(compact.data(), &address.s_addr, 4);
            compact[4] = peer.port >> 8; compact[5] = peer.port & 255;
            values.push_back(std::move(compact));
        }
        sendMessage(KRPCMessageFactory::createGetPeersResponseValues(our_id_, values, token, query.transaction_id), ip, port);
    }
}
void DHTManager::handleAnnouncePeer(const KRPCQuery& query, const std::string& ip, uint16_t port) {
    if (!query.info_hash || !query.token || (!query.port && !query.implied_port.value_or(0))) return;
    if (!verifyToken(*query.token, ip, *query.info_hash)) {
        sendMessage(KRPCMessageFactory::createError(ErrorCode::PROTOCOL_ERROR, "Invalid token", query.transaction_id), ip, port); return;
    }
    const auto peer_port = query.implied_port.value_or(0) ? port : query.port.value_or(0);
    if (!peer_port) return;
    peer_storage_.addPeer(*query.info_hash, {ip, peer_port});
    handlePing(query, ip, port);
}

void DHTManager::ping(const Node& node, std::function<void(const KRPCResponse&)> response, std::function<void()> timeout) {
    const auto id = KRPCMessageFactory::generateTransactionId();
    addTransaction(id, QueryType::PING, node, std::move(response), std::move(timeout));
    sendMessage(KRPCMessageFactory::createPing(our_id_, id), node.ip(), node.port()); ++queries_sent_;
}
void DHTManager::findNode(const Node& node, const NodeID& target, std::function<void(const KRPCResponse&)> response, std::function<void()> timeout) {
    const auto id = KRPCMessageFactory::generateTransactionId();
    addTransaction(id, QueryType::FIND_NODE, node, std::move(response), std::move(timeout));
    sendMessage(KRPCMessageFactory::createFindNode(our_id_, target, id), node.ip(), node.port()); ++queries_sent_;
}
std::string DHTManager::getPeersFromNode(const Node& node, const std::vector<uint8_t>& hash, std::function<void(const KRPCResponse&)> response, std::function<void()> timeout) {
    const auto id = KRPCMessageFactory::generateTransactionId();
    addTransaction(id, QueryType::GET_PEERS, node, std::move(response), std::move(timeout));
    sendMessage(KRPCMessageFactory::createGetPeers(our_id_, hash, id), node.ip(), node.port()); ++queries_sent_;
    return id;
}
void DHTManager::announcePeerToNode(const Node& node, const std::vector<uint8_t>& hash, uint16_t port, const std::string& token,
                                   std::function<void(const KRPCResponse&)> response, std::function<void()> timeout) {
    const auto id = KRPCMessageFactory::generateTransactionId();
    addTransaction(id, QueryType::ANNOUNCE_PEER, node, std::move(response), std::move(timeout));
    sendMessage(KRPCMessageFactory::createAnnouncePeer(our_id_, hash, port, token, id), node.ip(), node.port()); ++queries_sent_;
}

void DHTManager::addTransaction(const std::string& id, QueryType type, const Node& node,
                                std::function<void(const KRPCResponse&)> response, std::function<void()> timeout) {
    std::lock_guard<std::mutex> lock(transactions_mutex_);
    Transaction transaction;
    transaction.id = id; transaction.type = type; transaction.target_node = node.id();
    transaction.ip = node.ip(); transaction.port = node.port(); transaction.sent_time = std::chrono::steady_clock::now();
    transaction.on_response = std::move(response); transaction.on_timeout = std::move(timeout);
    pending_transactions_.emplace(id, std::move(transaction));
}
void DHTManager::removeTransaction(const std::string& id) {
    std::lock_guard<std::mutex> lock(transactions_mutex_); pending_transactions_.erase(id);
}
void DHTManager::checkTransactionTimeouts() {
    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard<std::mutex> lock(transactions_mutex_);
        const auto now = std::chrono::steady_clock::now();
        for (auto it = pending_transactions_.begin(); it != pending_transactions_.end();) {
            if (now - it->second.sent_time < std::chrono::seconds(2)) { ++it; continue; }
            if (it->second.on_timeout) callbacks.push_back(std::move(it->second.on_timeout));
            it = pending_transactions_.erase(it);
        }
    }
    for (const auto& callback : callbacks) callback();
}

std::string DHTManager::generateToken(const std::string& ip, const std::vector<uint8_t>& hash) {
    std::lock_guard<std::mutex> lock(token_mutex_);
    std::string bytes = ip + token_secret_; bytes.append(hash.begin(), hash.end());
    const auto digest = utils::sha1(bytes);
    return {digest.begin(), digest.begin() + 8};
}
bool DHTManager::verifyToken(const std::string& token, const std::string& ip, const std::vector<uint8_t>& hash) {
    std::lock_guard<std::mutex> lock(token_mutex_);
    for (const auto& secret : {token_secret_, old_token_secret_}) {
        if (secret.empty()) continue;
        std::string bytes = ip + secret; bytes.append(hash.begin(), hash.end());
        const auto digest = utils::sha1(bytes);
        if (token == std::string(digest.begin(), digest.begin() + 8)) return true;
    }
    return false;
}
void DHTManager::maintenanceLoop() {
    while (running_) {
        for (int i = 0; i < 600 && running_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!running_) break;
        refreshBuckets(); cleanupBadNodes();
        std::lock_guard<std::mutex> lock(token_mutex_);
        if (std::chrono::steady_clock::now() - last_token_rotation_ >= std::chrono::minutes(5)) {
            old_token_secret_ = token_secret_; token_secret_ = utils::generatePeerId().substr(8);
            last_token_rotation_ = std::chrono::steady_clock::now();
        }
    }
}
void DHTManager::refreshBuckets() {
    for (const auto bucket : routing_table_->getBucketsNeedingRefresh()) {
        const auto target = routing_table_->getRandomIDInBucket(bucket);
        for (const auto& node : routing_table_->findClosestNodes(target, 3)) {
            findNode(node, target, [this](const KRPCResponse& response) {
                if (response.nodes) for (const auto& found : *response.nodes) if (found.port()) routing_table_->addNode(found);
            });
        }
        routing_table_->markBucketRefreshed(bucket);
    }
}
void DHTManager::cleanupBadNodes() { routing_table_->cleanupBadNodes(); }

} // namespace torrent::dht
