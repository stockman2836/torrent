#pragma once

#include "torrent/network/peer/peer_connection.h"
#include "torrent/network/tracker/tracker_client.h"
#include <chrono>
#include <memory>

namespace torrent {

// Peer performance tracking
struct ActivePeerInfo {
    std::unique_ptr<PeerConnection> connection;
    Peer peer_data;

    // Performance metrics
    int64_t bytes_downloaded = 0;
    int64_t bytes_uploaded = 0;
    std::chrono::steady_clock::time_point connect_time;
    std::chrono::steady_clock::time_point last_activity;

    // Current state
    uint32_t current_piece = UINT32_MAX;  // UINT32_MAX means not downloading
    bool is_choking = true;
    bool is_interested = false;
    // Accessed under the session's peers_mutex_.
    std::vector<bool> available_pieces;
    bool finished = false;

    // Statistics
    int pieces_completed = 0;
    int failed_requests = 0;

    ActivePeerInfo(std::unique_ptr<PeerConnection> conn, const Peer& peer)
        : connection(std::move(conn)), peer_data(peer),
          connect_time(std::chrono::steady_clock::now()),
          last_activity(std::chrono::steady_clock::now()) {}

    double downloadSpeed() const {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - connect_time
        ).count();
        return elapsed > 0 ? static_cast<double>(bytes_downloaded) / elapsed : 0.0;
    }

    bool isStale(int timeout_seconds = 60) const {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - last_activity
        ).count();
        return elapsed >= timeout_seconds;
    }
};

} // namespace torrent
