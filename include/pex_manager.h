#pragma once

#include "bencode.h"
#include <string>
#include <vector>
#include <set>
#include <map>
#include <cstdint>
#include <chrono>

namespace torrent {

// BEP 11: Peer Exchange (PEX)
// Allows peers to exchange peer lists to discover new peers

// PEX flags for peer capabilities
constexpr uint8_t PEX_PREFER_ENCRYPTION = 0x01;  // Peer prefers encrypted connections
constexpr uint8_t PEX_SEED              = 0x02;  // Peer is a seed (upload only)
constexpr uint8_t PEX_SUPPORTS_UTP      = 0x04;  // Peer supports uTP
constexpr uint8_t PEX_HOLEPUNCH         = 0x08;  // Peer supports holepunch
constexpr uint8_t PEX_OUTGOING          = 0x10;  // Peer was connected outgoing

// Represents a peer for PEX exchange
struct PexPeer {
    std::string ip;
    uint16_t port;
    uint8_t flags;
    std::chrono::steady_clock::time_point added_time;

    PexPeer(const std::string& ip_, uint16_t port_, uint8_t flags_ = 0)
        : ip(ip_), port(port_), flags(flags_),
          added_time(std::chrono::steady_clock::now()) {}

    // Unique key for this peer
    std::string key() const;

    // Convert to compact format (6 bytes: 4 IP + 2 port)
    std::vector<uint8_t> toCompact() const;

    // Parse from compact format
    static PexPeer fromCompact(const uint8_t* data);

    bool operator<(const PexPeer& other) const {
        if (ip != other.ip) return ip < other.ip;
        return port < other.port;
    }
};

// Manages Peer Exchange (PEX) for a torrent
class PexManager {
public:
    PexManager();

    // Add a new peer to the known peers list
    // Returns true if peer was newly added
    bool addPeer(const std::string& ip, uint16_t port, uint8_t flags = 0);

    // Remove a peer from the known peers list
    // Returns true if peer was removed
    bool removePeer(const std::string& ip, uint16_t port);

    // Mark a peer as dropped (will be sent in next PEX message)
    void markPeerDropped(const std::string& ip, uint16_t port);

    // Build a PEX message with added and dropped peers
    // This creates a bencoded dictionary for the ut_pex extension
    bencode::BencodeValue buildPexMessage();

    // Parse an incoming PEX message
    // Returns the number of new peers discovered
    int parsePexMessage(const bencode::BencodeValue& data,
                        std::vector<PexPeer>& new_peers_out);

    // Get list of all known peers
    const std::set<PexPeer>& getKnownPeers() const { return known_peers_; }

    // Get number of known peers
    size_t getKnownPeerCount() const { return known_peers_.size(); }

    // Clear all state (useful for testing or reset)
    void clear();

    // Check if we should send a PEX update
    // PEX updates are typically sent every 60 seconds
    bool shouldSendUpdate() const;

    // Mark that we just sent a PEX update
    void markUpdateSent();

    // Maximum peers to send in a single PEX message
    static constexpr size_t MAX_PEERS_PER_MESSAGE = 50;

    // Minimum interval between PEX messages (seconds)
    static constexpr int PEX_INTERVAL_SECONDS = 60;

private:
    // Currently known peers
    std::set<PexPeer> known_peers_;

    // Peers added since last PEX message
    std::set<PexPeer> added_peers_;

    // Peers dropped since last PEX message
    std::set<PexPeer> dropped_peers_;

    // Last time we sent a PEX message
    std::chrono::steady_clock::time_point last_pex_sent_;

    // Convert peer set to compact binary format
    std::vector<uint8_t> peersToCompact(const std::set<PexPeer>& peers) const;

    // Convert peer set to flags binary format
    std::vector<uint8_t> peersToFlags(const std::set<PexPeer>& peers) const;

    // Parse compact peer format to peer list
    std::vector<PexPeer> compactToPeers(const std::vector<uint8_t>& compact,
                                        const std::vector<uint8_t>& flags) const;
};

} // namespace torrent
