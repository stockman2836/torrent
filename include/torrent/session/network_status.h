#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace torrent {

enum class PeerState { Connecting, Handshaking, Connected };
enum class TrackerState { None, Waiting, Announcing, Working, Error };

struct PeerStatus {
    std::string ip;
    uint16_t port = 0;
    PeerState state = PeerState::Connecting;
    bool incoming = false;
    bool choked = true;
    size_t pending_requests = 0;
    int64_t downloaded_bytes = 0;
    int64_t uploaded_bytes = 0;
};

struct TrackerStatus {
    std::string url;
    TrackerState state = TrackerState::None;
    size_t peers_returned = 0;
    size_t consecutive_failures = 0;
    std::string error;
};

// Copied values only. Socket and protocol state remain on their owning workers.
struct NetworkStatus {
    std::vector<PeerStatus> peers;
    size_t queued_peers = 0;
    size_t failed_connections = 0;
    std::string last_peer_error;
    bool dht_enabled = false;
    size_t dht_nodes = 0;
    TrackerStatus tracker;
};

} // namespace torrent
