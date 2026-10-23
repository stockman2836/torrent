#pragma once

#include "torrent/metainfo/torrent_file.h"
#include "torrent/core/config.h"
#include "torrent/network/tracker/tracker_client.h"
#include "torrent/network/peer/peer_connection.h"
#include "torrent/storage/piece_manager.h"
#include "torrent/storage/file_manager.h"
#include "torrent/core/utils.h"
#include "torrent/network/dht/dht_manager.h"
#include "torrent/network/discovery/lsd.h"
#include "torrent/network/transport/web_seed.h"
#include "torrent/network/transport/utp_socket.h"
#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <set>
#include <condition_variable>

namespace torrent {

struct ActivePeerInfo;

class DownloadManager {
public:
    DownloadManager(TorrentFile torrent_file, const Config& config,
                    std::unique_ptr<dht::DHTManager> existing_dht = nullptr);

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
    void waitForStop(std::chrono::milliseconds delay);
    std::mutex stop_mutex_;
    std::condition_variable stop_cv_;
    void downloadLoop();
    void trackerLoop();
    void peerLoop(std::shared_ptr<ActivePeerInfo> peer_info);
    void coordinatorLoop();
    void resumeLoop();  // Periodic state saving
    void dhtLoop();     // DHT operations
    void pexLoop();     // PEX operations
    void lsdLoop();     // LSD operations
    void webseedLoop(); // Web seeding operations
    void utpLoop();     // uTP operations

    void connectToPeers();
    void updateTracker();
    void updateDHT();   // Get peers from DHT
    void updatePEX();   // Exchange peers via PEX
    void updateWebSeeds();  // Download from web seeds
    void broadcastHave(uint32_t piece_index);

    // Resume capability
    std::string getResumeFilePath() const;
    void loadResumeState();
    void saveResumeState();
    void verifyExistingPieces();

    // Multi-peer coordination
    void assignPiecesToPeers();
    void cleanupStalePeers();
    int32_t selectNextPiece(ActivePeerInfo* peer);
    bool isEndgameMode() const;

    TorrentFile torrent_;
    std::string download_dir_;
    std::string peer_id_;
    uint16_t listen_port_;

    std::unique_ptr<PieceManager> piece_manager_;
    std::unique_ptr<FileManager> file_manager_;
    std::unique_ptr<TrackerClient> tracker_client_;
    std::unique_ptr<dht::DHTManager> dht_manager_;
    std::unique_ptr<LSD> lsd_manager_;
    std::unique_ptr<WebSeedManager> webseed_manager_;
    std::unique_ptr<utp::UtpManager> utp_manager_;

    std::vector<Peer> available_peers_;
    std::vector<std::shared_ptr<ActivePeerInfo>> active_peers_;
    mutable std::mutex peers_mutex_;

    // Piece tracking
    std::set<uint32_t> pieces_in_download_;  // Pieces currently being downloaded
    std::mutex pieces_mutex_;

    std::atomic<bool> running_;
    std::atomic<bool> paused_;
    std::atomic<bool> endgame_mode_;
    std::atomic<bool> seeding_mode_;
    bool enable_dht_;
    bool enable_pex_;
    bool enable_lsd_;
    bool enable_webseeds_;
    bool enable_utp_;

    std::vector<std::thread> worker_threads_;
    std::vector<std::thread> peer_threads_;

    // Configuration
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
    const size_t max_peers_;
};

} // namespace torrent
