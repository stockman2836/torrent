#pragma once

#include "torrent/metainfo/torrent_file.h"
#include "torrent/core/config.h"
#include "torrent/session/network_status.h"
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
class PeerListener;

class DownloadManager {
public:
    DownloadManager(TorrentFile torrent_file, const Config& config,
                    std::unique_ptr<dht::DHTManager> existing_dht = nullptr,
                    std::shared_ptr<utils::BandwidthLimits> global_limits = nullptr);

    ~DownloadManager();

    // Control
    void start();
    void stop();
    void pause();
    void resume();
    void setSpeedLimits(int64_t download_bytes_per_sec, int64_t upload_bytes_per_sec);
    void setPortMappingEnabled(bool enabled);
    bool requestTrackerUpdate(); // Nonblocking mailbox; respects the current cooldown.
    void setFilePriorities(const std::map<size_t, FilePriority>& priorities);

    // Status
    bool isRunning() const { return running_; }
    double progress() const;
    int64_t downloadSpeed() const;
    int64_t uploadSpeed() const;
    int64_t uploadedBytes() const { return total_uploaded_.load(); }
    uint16_t listenPort() const { return listen_port_; }
    uint16_t announcePort() const;
    NetworkStatus networkStatus() const;
    bool canSeed() const;
    void beginSeeding();

    void printStatus() const;

private:
    void waitForStop(std::chrono::milliseconds delay);
    std::mutex stop_mutex_;
    std::condition_variable stop_cv_;
    void downloadLoop();
    void trackerLoop();
    void peerLoop(std::shared_ptr<ActivePeerInfo> peer_info);
    void incomingLoop();
    void coordinatorLoop();
    void resumeLoop();  // Periodic state saving
    void dhtLoop();     // DHT operations
    void pexLoop();     // PEX operations
    void lsdLoop();     // LSD operations
    void webseedLoop(); // Web seeding operations
    void utpLoop();     // uTP operations

    void connectToPeers();
    void configurePeerExtensions(PeerConnection& connection);
    void updateTracker();
    void initializeTrackers(int timeout_seconds);
    std::chrono::steady_clock::time_point beginTrackerQuery(size_t index);
    void recordTrackerResult(size_t index, const std::string& event, const TrackerResponse& response,
                            std::chrono::steady_clock::time_point started, bool cancelled);
    void scheduleTrackerUpdate(const TrackerResponse& response);
    void stopTrackers();
    TrackerResponse announceTracker(const std::string& event);
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
    void requestNextPiece(ActivePeerInfo& peer);
    void requestEndgameBlock(ActivePeerInfo& peer); // Caller holds peers_mutex_.
    void flushPeerCommands(ActivePeerInfo& peer);
    void releasePeerPiece(ActivePeerInfo& peer);
    bool receiveBlock(uint32_t index, uint32_t offset, const std::vector<uint8_t>& data);
    int64_t bytesRemaining() const;
    void cleanupStalePeers();
    int32_t selectNextPiece(ActivePeerInfo* peer);
    bool isEndgameMode() const;

    TorrentFile torrent_;
    std::string download_dir_;
    std::string peer_id_;
    uint16_t listen_port_;

    std::unique_ptr<PieceManager> piece_manager_;
    std::unique_ptr<FileManager> file_manager_;
    std::unique_ptr<dht::DHTManager> dht_manager_;
    std::unique_ptr<LSD> lsd_manager_;
    std::unique_ptr<WebSeedManager> webseed_manager_;
    std::unique_ptr<utp::UtpManager> utp_manager_;
    std::unique_ptr<PeerListener> listener_;
    std::unique_ptr<PortMapper> port_mapper_;
    bool port_mapping_enabled_ = false;
    PortMappingOptions port_mapping_options_;
    std::thread listener_thread_;

    std::vector<Peer> available_peers_;
    std::vector<std::shared_ptr<ActivePeerInfo>> active_peers_;
    mutable std::mutex peers_mutex_;
    std::mutex connect_mutex_;
    std::mutex tracker_mutex_;
    TrackerStatus tracker_status_; // Protected by peers_mutex_, like peer diagnostics.
    std::vector<TrackerDetails> tracker_details_; // Same mutex; never read protocol objects from the UI.
    std::atomic<bool> tracker_retry_requested_{false};
    ListenerStatus listener_status_;
    size_t incoming_handshakes_ = 0;
    size_t loopback_handshakes_ = 0;
    size_t outgoing_handshakes_ = 0;
    size_t rejected_incoming_ = 0;
    size_t failed_incoming_handshakes_ = 0;
    std::vector<PeerStatus> connecting_peers_;
    size_t failed_connections_ = 0;
    std::string last_peer_error_;

    // Piece tracking
    std::set<uint32_t> pieces_in_download_;  // Pieces currently being downloaded
    std::mutex pieces_mutex_;
    std::mutex selection_mutex_;

    std::atomic<bool> running_;
    std::atomic<bool> paused_;
    std::atomic<bool> endgame_mode_;
    std::atomic<bool> seeding_mode_;
    bool enable_dht_;
    std::vector<dht::BootstrapNode> dht_bootstrap_nodes_;
    bool enable_pex_;
    bool enable_lsd_;
    bool enable_webseeds_;
    bool enable_utp_;
    bool listen_ipv6_;

    std::vector<std::thread> worker_threads_;
    struct PeerWorker {
        std::shared_ptr<ActivePeerInfo> peer;
        std::thread thread;
    };
    std::vector<PeerWorker> peer_threads_;

    // Configuration
    const size_t min_peers_ = 5;

    // Statistics
    std::atomic<int64_t> total_downloaded_;
    std::atomic<int64_t> total_uploaded_;

    // Tracker state
    struct TrackerEntry {
        std::string url;
        bool started = false;
        bool completed = false;
        bool completion_attempted = false;
        uint16_t announced_port = 0;
        size_t status_index = 0;
    };
    std::vector<std::vector<TrackerEntry>> tracker_tiers_; // Serialized by tracker_mutex_.
    int tracker_timeout_ = 30;
    bool completion_pending_ = false;
    std::chrono::steady_clock::time_point next_tracker_announce_{};
    uint16_t last_tracker_port_ = 0;
    bool complete_at_start_ = false;

    // Speed limiting and tracking
    utils::TokenBucket download_limiter_;
    utils::TokenBucket upload_limiter_;
    std::shared_ptr<utils::BandwidthLimits> global_limits_;
    utils::SpeedTracker download_tracker_;
    utils::SpeedTracker upload_tracker_;
    const size_t max_peers_;
};

} // namespace torrent
