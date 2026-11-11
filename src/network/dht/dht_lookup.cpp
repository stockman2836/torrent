#include "torrent/network/dht/dht_manager.h"
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace torrent::dht {

std::vector<DHTpeer> DHTManager::getPeers(const std::vector<uint8_t>& hash, int timeout_seconds,
                                         const std::function<bool()>& cancelled) {
    if (hash.size() != NODE_ID_SIZE || timeout_seconds < 0) throw std::invalid_argument("Invalid DHT lookup");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    const auto done = [&] { return !running_ || (cancelled && cancelled()) || std::chrono::steady_clock::now() >= deadline; };
    // Bootstrap is asynchronous. Wait for its first valid responder, not a fixed delay.
    while (!getNodeCount() && !done()) {
        {
            std::lock_guard<std::mutex> lock(transactions_mutex_);
            if (pending_transactions_.empty()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    NodeID target{}; std::copy(hash.begin(), hash.end(), target.begin());
    struct Lookup {
        std::mutex mutex;
        std::condition_variable wake;
        std::vector<Node> candidates;
        std::set<std::pair<std::string, uint16_t>> visited;
        std::vector<DHTpeer> peers;
        size_t pending = 0;
        bool closed = false;
    };
    auto lookup = std::make_shared<Lookup>();
    lookup->candidates = routing_table_->findClosestNodes(target, 8);
    std::vector<std::string> transactions;
    while (!done()) {
        std::vector<Node> next;
        {
            std::unique_lock<std::mutex> lock(lookup->mutex);
            if (!lookup->peers.empty()) break;
            std::sort(lookup->candidates.begin(), lookup->candidates.end(), [&](const auto& a, const auto& b) {
                return NodeDistance::isCloser(target, a.id(), b.id());
            });
            while (!lookup->candidates.empty() && lookup->pending < 3 && lookup->visited.size() < 64) {
                auto node = lookup->candidates.front(); lookup->candidates.erase(lookup->candidates.begin());
                if (!node.port() || node.id() == our_id_ || !lookup->visited.emplace(node.ip(), node.port()).second) continue;
                ++lookup->pending; next.push_back(std::move(node));
            }
            if (next.empty()) {
                if (!lookup->pending) {
                    // The first router may fail while another bootstrap reply
                    // is still in flight. Read the counter before the table:
                    // its last decrement publishes the last responding node.
                    const bool bootstrapping = pending_bootstrap_.load() > 0;
                    for (const auto& candidate : routing_table_->findClosestNodes(target, 64)) {
                        if (lookup->visited.size() >= 64) break;
                        if (!lookup->visited.count({candidate.ip(), candidate.port()})) lookup->candidates.push_back(candidate);
                    }
                    if (!lookup->candidates.empty() && lookup->visited.size() < 64) continue;
                    if (!bootstrapping) break;
                }
                lookup->wake.wait_for(lock, std::chrono::milliseconds(20)); continue;
            }
        }
        for (const auto& node : next) {
            transactions.push_back(getPeersFromNode(node, hash,
                [this, lookup](const KRPCResponse& response) {
                    std::lock_guard<std::mutex> lock(lookup->mutex);
                    if (lookup->closed) return;
                    if (response.values) for (const auto& compact : *response.values) {
                        if (compact.size() != 6 || lookup->peers.size() >= 200) continue;
                        in_addr address{}; std::memcpy(&address.s_addr, compact.data(), 4);
                        char ip[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &address, ip, sizeof(ip));
                        const auto port = static_cast<uint16_t>((static_cast<uint8_t>(compact[4]) << 8) | static_cast<uint8_t>(compact[5]));
                        if (!port) continue;
                        if (std::none_of(lookup->peers.begin(), lookup->peers.end(), [&](const auto& peer) { return peer.ip == ip && peer.port == port; }))
                            lookup->peers.push_back({ip, port});
                    }
                    if (response.nodes) for (const auto& found : *response.nodes) {
                        if (!found.port() || lookup->candidates.size() >= 256) continue;
                        routing_table_->addNode(found); lookup->candidates.push_back(found);
                    }
                    --lookup->pending; lookup->wake.notify_all();
                },
                [lookup] {
                    std::lock_guard<std::mutex> lock(lookup->mutex);
                    if (!lookup->closed) { --lookup->pending; lookup->wake.notify_all(); }
                }));
        }
    }
    std::vector<DHTpeer> result;
    {
        std::lock_guard<std::mutex> lock(lookup->mutex);
        lookup->closed = true; result = lookup->peers;
    }
    // Late replies cannot retain local stack references or mutate the returned result.
    for (const auto& transaction : transactions) removeTransaction(transaction);
    return result;
}

void DHTManager::announcePeer(const std::vector<uint8_t>& hash, uint16_t port) {
    if (hash.size() != NODE_ID_SIZE || !port) throw std::invalid_argument("Invalid DHT announce");
    if (!running_) return;
    NodeID target{}; std::copy(hash.begin(), hash.end(), target.begin());
    for (const auto& node : routing_table_->findClosestNodes(target, 8)) {
        getPeersFromNode(node, hash, [this, node, hash, port](const KRPCResponse& response) {
            if (running_ && response.token) announcePeerToNode(node, hash, port, *response.token);
        });
    }
}

} // namespace torrent::dht
