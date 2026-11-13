#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <chrono>
#include "torrent/network/peer/listener_status.h"
#include "torrent/network/port_mapping/port_mapper.h"

namespace torrent {

enum class PeerState { Connecting, Handshaking, Connected };
enum class TrackerState { None, Waiting, Announcing, Working, Error };
enum class TrackerEventResult { Success, Failure, Cancelled };
constexpr size_t TRACKER_HISTORY_LIMIT = 16;

struct TrackerEvent {
    int64_t time_ms = 0;
    std::string event;
    TrackerEventResult result = TrackerEventResult::Success;
    size_t peers_returned = 0;
    std::string error;
    int64_t duration_ms = 0;
};

struct TrackerDetails {
    std::string url;
    size_t tier = 0; // One-based original tier; stable across promotion/shuffling.
    TrackerState state = TrackerState::Waiting;
    size_t requests = 0;
    size_t peers_returned = 0;
    size_t consecutive_failures = 0;
    int64_t last_response_ms = 0;
    std::string error;
    std::chrono::steady_clock::time_point retry_after{};
    std::vector<TrackerEvent> history; // Oldest first, bounded per URL.
};

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
    size_t total_trackers = 0;
    size_t attempt = 0;
    std::string error;
    bool retry_pending = false;
    std::chrono::steady_clock::time_point retry_after{};
    std::chrono::steady_clock::time_point next_announce{};
};

// Copied values only. Socket and protocol state remain on their owning workers.
struct NetworkStatus {
    ListenerStatus listener;
    PortMappingStatus port_mapping;
    size_t incoming_handshakes = 0;
    size_t loopback_handshakes = 0;
    size_t outgoing_handshakes = 0;
    size_t rejected_incoming = 0;
    size_t failed_incoming_handshakes = 0;
    size_t max_peers = 0;
    int64_t download_limit = 0;
    int64_t upload_limit = 0;
    int64_t global_download_limit = 0;
    int64_t global_upload_limit = 0;
    bool pex_enabled = false;
    bool lsd_enabled = false;
    size_t web_seeds = 0;
    std::vector<PeerStatus> peers;
    size_t queued_peers = 0;
    size_t failed_connections = 0;
    std::string last_peer_error;
    bool dht_enabled = false;
    size_t dht_nodes = 0;
    TrackerStatus tracker;
    std::vector<TrackerDetails> trackers;
};

} // namespace torrent
