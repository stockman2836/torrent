#include "pex_manager.h"
#include "logger.h"
#include <sstream>
#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace torrent {

// ============================================================================
// PexPeer Implementation
// ============================================================================

std::string PexPeer::key() const {
    std::stringstream ss;
    ss << ip << ":" << port;
    return ss.str();
}

std::vector<uint8_t> PexPeer::toCompact() const {
    std::vector<uint8_t> result(6);

    // Convert IP string to binary (4 bytes)
    struct in_addr addr;
    if (inet_pton(AF_INET, ip.c_str(), &addr) != 1) {
        LOG_WARN("Failed to convert IP to compact format: {}", ip);
        return {};
    }

    // Copy IP address (network byte order)
    std::memcpy(result.data(), &addr.s_addr, 4);

    // Add port (big-endian)
    result[4] = (port >> 8) & 0xFF;
    result[5] = port & 0xFF;

    return result;
}

PexPeer PexPeer::fromCompact(const uint8_t* data) {
    // Extract IP (4 bytes)
    struct in_addr addr;
    std::memcpy(&addr.s_addr, data, 4);

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr, ip_str, INET_ADDRSTRLEN);

    // Extract port (big-endian)
    uint16_t port = (static_cast<uint16_t>(data[4]) << 8) |
                     static_cast<uint16_t>(data[5]);

    return PexPeer(ip_str, port);
}

// ============================================================================
// PexManager Implementation
// ============================================================================

PexManager::PexManager()
    : last_pex_sent_(std::chrono::steady_clock::now()) {
}

bool PexManager::addPeer(const std::string& ip, uint16_t port, uint8_t flags) {
    PexPeer peer(ip, port, flags);

    // Check if peer already exists
    if (known_peers_.find(peer) != known_peers_.end()) {
        return false; // Already known
    }

    // Add to known peers
    known_peers_.insert(peer);

    // Add to added_peers for next PEX message
    added_peers_.insert(peer);

    LOG_DEBUG("PEX: Added peer {}:{} (flags: 0x{:02x})", ip, port, flags);
    return true;
}

bool PexManager::removePeer(const std::string& ip, uint16_t port) {
    PexPeer peer(ip, port);

    auto it = known_peers_.find(peer);
    if (it == known_peers_.end()) {
        return false; // Not found
    }

    // Remove from known peers
    known_peers_.erase(it);

    // Remove from added_peers if it was there
    added_peers_.erase(peer);

    // Add to dropped_peers for next PEX message
    dropped_peers_.insert(peer);

    LOG_DEBUG("PEX: Removed peer {}:{}", ip, port);
    return true;
}

void PexManager::markPeerDropped(const std::string& ip, uint16_t port) {
    removePeer(ip, port);
}

bencode::BencodeValue PexManager::buildPexMessage() {
    bencode::BencodeDict dict;

    // Limit the number of peers we send
    std::set<PexPeer> peers_to_add;
    size_t count = 0;
    for (const auto& peer : added_peers_) {
        if (count >= MAX_PEERS_PER_MESSAGE) break;
        peers_to_add.insert(peer);
        count++;
    }

    // Build "added" field (compact peer list)
    if (!peers_to_add.empty()) {
        std::vector<uint8_t> added_compact = peersToCompact(peers_to_add);
        dict["added"] = bencode::BencodeValue(
            std::string(added_compact.begin(), added_compact.end())
        );

        // Build "added.f" field (flags for each peer)
        std::vector<uint8_t> added_flags = peersToFlags(peers_to_add);
        dict["added.f"] = bencode::BencodeValue(
            std::string(added_flags.begin(), added_flags.end())
        );

        LOG_DEBUG("PEX: Building message with {} added peers", peers_to_add.size());
    } else {
        // Empty strings for no peers
        dict["added"] = bencode::BencodeValue(std::string(""));
        dict["added.f"] = bencode::BencodeValue(std::string(""));
    }

    // Build "dropped" field (compact peer list)
    if (!dropped_peers_.empty()) {
        std::vector<uint8_t> dropped_compact = peersToCompact(dropped_peers_);
        dict["dropped"] = bencode::BencodeValue(
            std::string(dropped_compact.begin(), dropped_compact.end())
        );

        LOG_DEBUG("PEX: Building message with {} dropped peers", dropped_peers_.size());
    } else {
        dict["dropped"] = bencode::BencodeValue(std::string(""));
    }

    // Clear the added/dropped lists after building message
    added_peers_.clear();
    dropped_peers_.clear();

    return bencode::BencodeValue(dict);
}

int PexManager::parsePexMessage(const bencode::BencodeValue& data,
                                 std::vector<PexPeer>& new_peers_out) {
    if (!data.isDict()) {
        LOG_WARN("PEX: Invalid message format (not a dictionary)");
        return 0;
    }

    const auto& dict = data.asDict();
    int new_peer_count = 0;

    // Parse "added" peers
    if (dict.find("added") != dict.end() && dict.at("added").isString()) {
        const std::string& added_str = dict.at("added").asString();
        std::vector<uint8_t> added_compact(added_str.begin(), added_str.end());

        // Parse "added.f" flags
        std::vector<uint8_t> added_flags;
        if (dict.find("added.f") != dict.end() && dict.at("added.f").isString()) {
            const std::string& flags_str = dict.at("added.f").asString();
            added_flags.assign(flags_str.begin(), flags_str.end());
        }

        // Convert compact format to peer list
        std::vector<PexPeer> added_peers = compactToPeers(added_compact, added_flags);

        for (const auto& peer : added_peers) {
            if (addPeer(peer.ip, peer.port, peer.flags)) {
                new_peers_out.push_back(peer);
                new_peer_count++;
            }
        }

        if (!added_peers.empty()) {
            LOG_INFO("PEX: Received {} added peers ({} new)",
                     added_peers.size(), new_peer_count);
        }
    }

    // Parse "dropped" peers
    if (dict.find("dropped") != dict.end() && dict.at("dropped").isString()) {
        const std::string& dropped_str = dict.at("dropped").asString();
        std::vector<uint8_t> dropped_compact(dropped_str.begin(), dropped_str.end());

        std::vector<PexPeer> dropped_peers = compactToPeers(dropped_compact, {});

        for (const auto& peer : dropped_peers) {
            removePeer(peer.ip, peer.port);
        }

        if (!dropped_peers.empty()) {
            LOG_DEBUG("PEX: Received {} dropped peers", dropped_peers.size());
        }
    }

    // Parse "added6" (IPv6 peers) - for future support
    if (dict.find("added6") != dict.end()) {
        LOG_DEBUG("PEX: IPv6 peers received but not yet supported");
    }

    return new_peer_count;
}

void PexManager::clear() {
    known_peers_.clear();
    added_peers_.clear();
    dropped_peers_.clear();
    last_pex_sent_ = std::chrono::steady_clock::now();
}

bool PexManager::shouldSendUpdate() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - last_pex_sent_
    ).count();

    // Send if we have changes and enough time has passed
    bool has_changes = !added_peers_.empty() || !dropped_peers_.empty();
    bool interval_passed = elapsed >= PEX_INTERVAL_SECONDS;

    return has_changes && interval_passed;
}

void PexManager::markUpdateSent() {
    last_pex_sent_ = std::chrono::steady_clock::now();
}

// ============================================================================
// Private Helper Methods
// ============================================================================

std::vector<uint8_t> PexManager::peersToCompact(const std::set<PexPeer>& peers) const {
    std::vector<uint8_t> result;
    result.reserve(peers.size() * 6);

    for (const auto& peer : peers) {
        std::vector<uint8_t> compact = peer.toCompact();
        if (!compact.empty()) {
            result.insert(result.end(), compact.begin(), compact.end());
        }
    }

    return result;
}

std::vector<uint8_t> PexManager::peersToFlags(const std::set<PexPeer>& peers) const {
    std::vector<uint8_t> result;
    result.reserve(peers.size());

    for (const auto& peer : peers) {
        result.push_back(peer.flags);
    }

    return result;
}

std::vector<PexPeer> PexManager::compactToPeers(
    const std::vector<uint8_t>& compact,
    const std::vector<uint8_t>& flags) const {

    std::vector<PexPeer> result;

    // Each peer is 6 bytes (4 IP + 2 port)
    if (compact.size() % 6 != 0) {
        LOG_WARN("PEX: Invalid compact peer format (size not multiple of 6)");
        return result;
    }

    size_t peer_count = compact.size() / 6;
    result.reserve(peer_count);

    for (size_t i = 0; i < peer_count; ++i) {
        const uint8_t* data = compact.data() + (i * 6);
        PexPeer peer = PexPeer::fromCompact(data);

        // Apply flags if available
        if (i < flags.size()) {
            peer.flags = flags[i];
        }

        result.push_back(peer);
    }

    return result;
}

} // namespace torrent
