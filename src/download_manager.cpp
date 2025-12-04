#include "download_manager.h"
#include "utils.h"
#include <iostream>
#include <chrono>
#include <thread>

namespace torrent {

DownloadManager::DownloadManager(const std::string& torrent_path,
                                const std::string& download_dir,
                                uint16_t listen_port)
    : download_dir_(download_dir)
    , peer_id_(utils::generatePeerId())
    , listen_port_(listen_port)
    , running_(false)
    , paused_(false)
    , total_downloaded_(0)
    , total_uploaded_(0) {

    // Load torrent file
    torrent_ = TorrentFile::fromFile(torrent_path);
    torrent_.printInfo();

    // Initialize managers
    piece_manager_ = std::make_unique<PieceManager>(
        torrent_.numPieces(),
        torrent_.pieceLength(),
        torrent_.totalLength(),
        torrent_.pieces()
    );

    file_manager_ = std::make_unique<FileManager>(torrent_, download_dir_);

    tracker_client_ = std::make_unique<TrackerClient>(
        torrent_.announce(),
        torrent_.infoHash(),
        peer_id_
    );
}

DownloadManager::~DownloadManager() {
    stop();
}

void DownloadManager::start() {
    if (running_) {
        return;
    }

    std::cout << "Initializing files...\n";
    if (!file_manager_->initialize()) {
        throw std::runtime_error("Failed to initialize files");
    }

    running_ = true;
    paused_ = false;

    std::cout << "Contacting tracker...\n";
    updateTracker();

    std::cout << "Starting download...\n";

    // Start worker threads
    worker_threads_.emplace_back(&DownloadManager::downloadLoop, this);
    worker_threads_.emplace_back(&DownloadManager::trackerLoop, this);
}

void DownloadManager::stop() {
    if (!running_) {
        return;
    }

    running_ = false;

    // Wait for threads to finish
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    worker_threads_.clear();

    // Disconnect all peers
    connections_.clear();

    std::cout << "Download stopped.\n";
}

void DownloadManager::pause() {
    paused_ = true;
}

void DownloadManager::resume() {
    paused_ = false;
}

double DownloadManager::progress() const {
    return piece_manager_->percentComplete();
}

int64_t DownloadManager::downloadSpeed() const {
    // TODO: Calculate actual download speed
    return 0;
}

int64_t DownloadManager::uploadSpeed() const {
    // TODO: Calculate actual upload speed
    return 0;
}

void DownloadManager::printStatus() const {
    std::cout << "\r[Progress: " << std::fixed << std::setprecision(2)
              << progress() << "%] "
              << "Down: " << utils::formatSpeed(downloadSpeed()) << " "
              << "Up: " << utils::formatSpeed(uploadSpeed()) << " "
              << "Peers: " << connections_.size() << "   " << std::flush;
}

void DownloadManager::downloadLoop() {
    while (running_) {
        if (paused_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // TODO: Implement actual download logic
        // For now, just sleep
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // Check if complete
        if (progress() >= 100.0) {
            break;
        }
    }
}

void DownloadManager::trackerLoop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(30));

        if (!running_) {
            break;
        }

        updateTracker();
    }
}

void DownloadManager::peerLoop(const Peer& peer) {
    // TODO: Implement peer communication loop
    std::cout << "Peer loop for " << peer.ip << ":" << peer.port << "\n";
}

void DownloadManager::connectToPeers() {
    // TODO: Connect to peers from tracker
    for (const auto& peer : peers_) {
        std::cout << "Would connect to: " << peer.ip << ":" << peer.port << "\n";
    }
}

void DownloadManager::updateTracker() {
    int64_t left = torrent_.totalLength() - total_downloaded_;

    TrackerResponse response = tracker_client_->announce(
        total_uploaded_,
        total_downloaded_,
        left,
        listen_port_,
        "started"
    );

    if (!response.isSuccess()) {
        std::cerr << "Tracker error: " << response.failure_reason << "\n";
        return;
    }

    std::cout << "Tracker response: " << response.peers.size() << " peers, "
              << response.complete << " seeders, "
              << response.incomplete << " leechers\n";

    peers_ = response.peers;
    connectToPeers();
}

} // namespace torrent
