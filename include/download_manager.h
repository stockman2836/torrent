#pragma once

#include "torrent_file.h"
#include "tracker_client.h"
#include "peer_connection.h"
#include "piece_manager.h"
#include "file_manager.h"
#include "utils.h"
#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <set>

namespace torrent {

// Peer performance tracking
struct PeerInfo {
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

    // Statistics
    int pieces_completed = 0;
    int failed_requests = 0;

    PeerInfo(std::unique_ptr<PeerConnection> conn, const Peer& peer)
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

class DownloadManager {
public:
    DownloadManager(const std::string& torrent_path,
                   const std::string& download_dir,
                   uint16_t listen_port = 6881,
                   int64_t max_download_speed = 0,  // 0 = unlimited (bytes/sec)
                   int64_t max_upload_speed = 0);    // 0 = unlimited (bytes/sec)

    ~DownloadManager();

    // Control
    void start();
    void stop();
    void pause();
    void resume();

    // Status
    bool isRunning() const { return running_; }
    double progress() const;
    int64_t downloadSpeed() const;
    int64_t uploadSpeed() const;

    void printStatus() const;

private:
    void downloadLoop();
    void trackerLoop();
    void peerLoop(size_t peer_index);
    void coordinatorLoop();
    void resumeLoop();  // Periodic state saving

    void connectToPeers();
    void updateTracker();
    void broadcastHave(uint32_t piece_index);

    // Resume capability
    std::string getResumeFilePath() const;
    void loadResumeState();
    void saveResumeState();
    void verifyExistingPieces();

    // Multi-peer coordination
    void assignPiecesToPeers();
    PeerInfo* selectPeerForPiece(uint32_t piece_index);
    void cleanupStalePeers();
    int32_t selectNextPiece(PeerInfo* peer);
    bool isEndgameMode() const;

    TorrentFile torrent_;
    std::string download_dir_;
    std::string peer_id_;
    uint16_t listen_port_;

    std::unique_ptr<PieceManager> piece_manager_;
    std::unique_ptr<FileManager> file_manager_;
    std::unique_ptr<TrackerClient> tracker_client_;

    std::vector<Peer> available_peers_;
    std::vector<PeerInfo> active_peers_;
    std::mutex peers_mutex_;

    // Piece tracking
    std::set<uint32_t> pieces_in_download_;  // Pieces currently being downloaded
    std::mutex pieces_mutex_;

    std::atomic<bool> running_;
    std::atomic<bool> paused_;
    std::atomic<bool> endgame_mode_;
    std::atomic<bool> seeding_mode_;

    std::vector<std::thread> worker_threads_;

    // Configuration
    const size_t max_peers_ = 50;
    const size_t min_peers_ = 5;

    // Statistics
    std::atomic<int64_t> total_downloaded_;
    std::atomic<int64_t> total_uploaded_;

    // Tracker state
    bool first_announce_ = true;
    int tracker_failures_ = 0;

    // Speed limiting and tracking
    utils::TokenBucket download_limiter_;
    utils::TokenBucket upload_limiter_;
    utils::SpeedTracker download_tracker_;
    utils::SpeedTracker upload_tracker_;
};

} // namespace torrent
