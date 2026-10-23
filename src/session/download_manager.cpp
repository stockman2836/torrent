#include "torrent/session/download_manager.h"
#include "peer_state.h"
#include "torrent/core/logger.h"
#include "torrent/core/utils.h"
#include "torrent/network/discovery/pex_manager.h"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>


namespace torrent {

DownloadManager::DownloadManager(TorrentFile torrent_file, const Config& config,
                                std::unique_ptr<dht::DHTManager> existing_dht)
    : torrent_(std::move(torrent_file))
    , download_dir_(config.download_dir)
    , peer_id_(utils::generatePeerId())
    , listen_port_(config.listen_port)
    , running_(false)
    , paused_(false)
    , endgame_mode_(false)
    , seeding_mode_(false)
    , enable_dht_(config.enable_dht)
    , enable_pex_(config.enable_pex)
    , enable_lsd_(config.enable_lsd)
    , enable_webseeds_(config.enable_webseeds)
    , enable_utp_(config.enable_utp)
    , total_downloaded_(0)
    , total_uploaded_(0)
    , download_limiter_(config.max_download_speed)
    , upload_limiter_(config.max_upload_speed)
    , download_tracker_(20)  // 20 second window
    , upload_tracker_(20)
    , max_peers_(config.max_peers) {

    torrent_.printInfo();

    // Initialize managers
    piece_manager_ = std::make_unique<PieceManager>(
        torrent_.numPieces(),
        torrent_.pieceLength(),
        torrent_.totalLength(),
        torrent_.pieces()
    );

    piece_manager_->setSequentialMode(config.sequential_download);

    file_manager_ = std::make_unique<FileManager>(torrent_, download_dir_);

    tracker_client_ = std::make_unique<TrackerClient>(
        torrent_.announce(),
        torrent_.infoHash(),
        peer_id_
    );

    // Use existing DHT or create new one
    if (existing_dht) {
        dht_manager_ = std::move(existing_dht);
    } else if (enable_dht_) {
        dht_manager_ = std::make_unique<dht::DHTManager>(listen_port_ + 1); // DHT on port + 1
    }

    // Initialize LSD if enabled
    if (enable_lsd_) {
        lsd_manager_ = std::make_unique<LSD>(listen_port_, false);  // IPv4 for now
    }

    // Initialize Web Seed Manager if enabled and torrent has web seeds
    if (enable_webseeds_ && torrent_.hasWebSeeds()) {
        webseed_manager_ = std::make_unique<WebSeedManager>(
            torrent_,
            torrent_.pieceLength(),
            torrent_.totalLength()
        );

        // Add all web seed URLs
        for (const auto& url : torrent_.webSeeds()) {
            webseed_manager_->addWebSeed(url);
        }
    }

    // Initialize uTP Manager if enabled
    if (enable_utp_ && utp::UtpManager::isAvailable()) {
        utp_manager_ = std::make_unique<utp::UtpManager>(listen_port_);
        std::cout << "uTP: Initialized on port " << listen_port_ << "\n";
    } else if (enable_utp_) {
        std::cout << "uTP: Requested but libutp not available\n";
    }
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

    // Load resume state if available
    std::cout << "Checking for resume data...\n";
    loadResumeState();

    running_ = true;
    paused_ = false;

    std::cout << "Contacting tracker...\n";
    updateTracker();

    // Start DHT if enabled
    if (enable_dht_ && dht_manager_) {
        std::cout << "Starting DHT...\n";
        dht_manager_->start();

        // Bootstrap DHT
        std::vector<dht::BootstrapNode> bootstrap_nodes = {
            {"router.bittorrent.com", 6881},
            {"dht.transmissionbt.com", 6881},
            {"router.utorrent.com", 6881}
        };
        dht_manager_->bootstrap(bootstrap_nodes);
    }

    // Start LSD if enabled
    if (enable_lsd_ && lsd_manager_) {
        std::cout << "Starting LSD...\n";
        lsd_manager_->start();
        // Set callback for discovered peers
        lsd_manager_->setPeerCallback([this](const std::vector<LSDPeer>& peers) {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            for (const auto& lsd_peer : peers) {
                Peer peer(lsd_peer.ip, lsd_peer.port);
                available_peers_.push_back(peer);
            }
        });
        // Announce our torrent
        lsd_manager_->announce(torrent_.infoHash());
    }

    // Start Web Seeding if enabled
    if (enable_webseeds_ && webseed_manager_) {
        std::cout << "Starting Web Seeding (" << webseed_manager_->getTotalWebSeeds() << " seeds)...\n";
        webseed_manager_->start();

        // Set callback for piece downloads
        webseed_manager_->setCallback([this](const WebSeedDownload& download) {
            if (download.success) {
                // Add downloaded block to piece manager
                piece_manager_->addBlock(download.piece_index, download.offset, download.data);

                // Check if piece is complete
                if (piece_manager_->isPieceInProgress(download.piece_index)) {
                    auto* piece = piece_manager_->getPieceInProgress(download.piece_index);
                    if (piece && piece->isComplete()) {
                        piece_manager_->completePiece(download.piece_index, file_manager_.get());
                    }
                }
            }
        });
    }

    // Start uTP if enabled
    if (enable_utp_ && utp_manager_) {
        std::cout << "Starting uTP on port " << listen_port_ << "...\n";
        utp_manager_->startListening(listen_port_);
    }

    std::cout << "Starting download...\n";

    // Start worker threads
    worker_threads_.emplace_back(&DownloadManager::downloadLoop, this);
    worker_threads_.emplace_back(&DownloadManager::trackerLoop, this);
    worker_threads_.emplace_back(&DownloadManager::coordinatorLoop, this);
    worker_threads_.emplace_back(&DownloadManager::resumeLoop, this);

    if (enable_dht_ && dht_manager_) {
        worker_threads_.emplace_back(&DownloadManager::dhtLoop, this);
    }

    if (enable_pex_) {
        worker_threads_.emplace_back(&DownloadManager::pexLoop, this);
    }

    if (enable_lsd_ && lsd_manager_) {
        worker_threads_.emplace_back(&DownloadManager::lsdLoop, this);
    }

    if (enable_webseeds_ && webseed_manager_) {
        worker_threads_.emplace_back(&DownloadManager::webseedLoop, this);
    }

    if (enable_utp_ && utp_manager_) {
        worker_threads_.emplace_back(&DownloadManager::utpLoop, this);
    }
}

void DownloadManager::stop() {
    if (!running_) {
        return;
    }

    std::cout << "Stopping client...\n";

    // Notify tracker we're stopping
    int64_t left = torrent_.totalLength() - total_downloaded_;
    tracker_client_->announce(
        total_uploaded_,
        total_downloaded_,
        left,
        listen_port_,
        "stopped"
    );

    // Save final resume state before shutdown
    std::cout << "Saving resume state...\n";
    saveResumeState();

    running_ = false;

    stop_cv_.notify_all();

    // Stop DHT
    if (enable_dht_ && dht_manager_) {
        std::cout << "Stopping DHT...\n";
        dht_manager_->stop();
    }

    // Stop LSD
    if (enable_lsd_ && lsd_manager_) {
        std::cout << "Stopping LSD...\n";
        lsd_manager_->stop();
    }

    // Stop Web Seeding
    if (enable_webseeds_ && webseed_manager_) {
        std::cout << "Stopping Web Seeding...\n";
        webseed_manager_->stop();
    }

    // Stop uTP
    if (enable_utp_ && utp_manager_) {
        std::cout << "Stopping uTP...\n";
        utp_manager_->stopListening();
    }

    // Wait for threads to finish
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    worker_threads_.clear();
    for (auto& thread : peer_threads_) {
        if (thread.joinable()) thread.join();
    }
    peer_threads_.clear();

    // Disconnect all peers
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        active_peers_.clear();
    }

    std::cout << "Client stopped.\n";
}

void DownloadManager::pause() {
    paused_ = true;
}

void DownloadManager::waitForStop(std::chrono::milliseconds delay) {
    std::unique_lock<std::mutex> lock(stop_mutex_);
    stop_cv_.wait_for(lock, delay, [this] { return !running_; });
}

void DownloadManager::resume() {
    paused_ = false;
}

double DownloadManager::progress() const {
    return piece_manager_->percentComplete();
}

int64_t DownloadManager::downloadSpeed() const {
    return static_cast<int64_t>(download_tracker_.getSpeed());
}

int64_t DownloadManager::uploadSpeed() const {
    return static_cast<int64_t>(upload_tracker_.getSpeed());
}

void DownloadManager::printStatus() const {
    size_t active_peer_count = 0;
    size_t downloading_peers = 0;

    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        active_peer_count = active_peers_.size();
        for (const auto& peer : active_peers_) {
            if (peer->current_piece != UINT32_MAX) {
                downloading_peers++;
            }
        }
    }

    std::cout << "\r[Progress: " << std::fixed << std::setprecision(2)
              << progress() << "%] "
              << "Down: " << utils::formatSpeed(downloadSpeed()) << " "
              << "Up: " << utils::formatSpeed(uploadSpeed()) << " "
              << "Peers: " << active_peer_count << " (" << downloading_peers << " active)"
              << (seeding_mode_ ? " [SEEDING]" : "")
              << (endgame_mode_ ? " [ENDGAME]" : "")
              << "   " << std::flush;
}

void DownloadManager::downloadLoop() {
    // Wait for initial peer list from tracker
    waitForStop(std::chrono::seconds(2));

    while (running_) {
        if (paused_) {
            waitForStop(std::chrono::milliseconds(100));
            continue;
        }

        // Print status
        printStatus();

        // Check if download complete and transition to seeding
        if (!seeding_mode_ && progress() >= 100.0) {
            std::cout << "\n========================================\n";
            std::cout << "Download complete! Transitioning to seeding mode...\n";
            std::cout << "========================================\n";

            // Switch to seeding mode
            seeding_mode_ = true;

            // Notify tracker of completion
            int64_t left = 0; // No bytes left to download
            TrackerResponse response = tracker_client_->announce(
                total_uploaded_,
                total_downloaded_,
                left,
                listen_port_,
                "completed"
            );

            if (response.isSuccess()) {
                std::cout << "Tracker notified of completion. Now seeding to "
                          << response.peers.size() << " peers.\n";
            } else {
                std::cerr << "Failed to notify tracker: " << response.failure_reason << "\n";
            }

            std::cout << "Seeding will continue until client is stopped.\n";
            std::cout << "========================================\n\n";
        }

        // Sleep
        waitForStop(std::chrono::seconds(1));
    }
}

} // namespace torrent
