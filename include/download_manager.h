#pragma once

#include "torrent_file.h"
#include "tracker_client.h"
#include "peer_connection.h"
#include "piece_manager.h"
#include "file_manager.h"
#include <memory>
#include <vector>
#include <thread>
#include <atomic>

namespace torrent {

class DownloadManager {
public:
    DownloadManager(const std::string& torrent_path,
                   const std::string& download_dir,
                   uint16_t listen_port = 6881);

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
    void peerLoop(const Peer& peer);

    void connectToPeers();
    void updateTracker();
    void broadcastHave(uint32_t piece_index);

    TorrentFile torrent_;
    std::string download_dir_;
    std::string peer_id_;
    uint16_t listen_port_;

    std::unique_ptr<PieceManager> piece_manager_;
    std::unique_ptr<FileManager> file_manager_;
    std::unique_ptr<TrackerClient> tracker_client_;

    std::vector<Peer> peers_;
    std::vector<std::unique_ptr<PeerConnection>> connections_;
    std::mutex connections_mutex_;

    std::atomic<bool> running_;
    std::atomic<bool> paused_;

    std::vector<std::thread> worker_threads_;

    // Statistics
    std::atomic<int64_t> total_downloaded_;
    std::atomic<int64_t> total_uploaded_;
};

} // namespace torrent
