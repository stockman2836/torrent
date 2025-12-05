#include "download_manager.h"
#include "utils.h"
#include <iostream>
#include <iomanip>
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
    // Wait for initial peer list from tracker
    std::this_thread::sleep_for(std::chrono::seconds(2));

    while (running_) {
        if (paused_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Print status
        printStatus();

        // Check if complete
        if (progress() >= 100.0) {
            std::cout << "\nDownload complete!\n";
            break;
        }

        // Sleep
        std::this_thread::sleep_for(std::chrono::seconds(1));
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
    std::cout << "Starting peer loop for " << peer.ip << ":" << peer.port << "\n";

    // Create connection
    auto connection = std::make_unique<PeerConnection>(
        peer.ip,
        peer.port,
        torrent_.infoHash(),
        peer_id_
    );

    // Connect
    if (!connection->connect()) {
        std::cerr << "Failed to connect to peer " << peer.ip << ":" << peer.port << "\n";
        return;
    }

    // Initialize peer bitfield
    connection->initializePeerBitfield(torrent_.numPieces());

    // Add to connections list
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connections_.push_back(std::move(connection));
    }

    // Get pointer to connection (last element)
    PeerConnection* conn_ptr = nullptr;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        if (!connections_.empty()) {
            conn_ptr = connections_.back().get();
        }
    }

    if (!conn_ptr) {
        return;
    }

    // Perform handshake with our bitfield
    std::vector<bool> our_bitfield = piece_manager_->getBitfield();
    if (!conn_ptr->performHandshake(our_bitfield)) {
        std::cerr << "Failed to perform handshake with " << peer.ip << ":" << peer.port << "\n";
        return;
    }

    // Main communication loop
    while (running_ && !paused_ && conn_ptr->isConnected()) {
        // Receive and process messages
        auto message = conn_ptr->receiveMessage();
        if (!message) {
            std::cerr << "Connection closed by peer " << peer.ip << ":" << peer.port << "\n";
            break;
        }

        // Check if we're interested in this peer
        if (!conn_ptr->amInterested()) {
            // Check if peer has pieces we need
            bool has_needed_pieces = false;
            for (uint32_t i = 0; i < torrent_.numPieces(); ++i) {
                if (!piece_manager_->hasPiece(i) && conn_ptr->peerHasPiece(i)) {
                    has_needed_pieces = true;
                    break;
                }
            }

            if (has_needed_pieces) {
                std::cout << "Sending INTERESTED to " << peer.ip << "\n";
                conn_ptr->sendInterested();
            }
        }

        // If we can download, request pieces
        if (conn_ptr->canDownload() && !conn_ptr->hasPendingRequests()) {
            // Get next piece to download
            int32_t next_piece = piece_manager_->getNextPiece(conn_ptr->peerBitfield());

            if (next_piece >= 0) {
                std::cout << "Requesting piece #" << next_piece << " from " << peer.ip << "\n";

                // Get blocks for this piece
                std::vector<Block> blocks = piece_manager_->getBlocksForPiece(next_piece);

                // Request the piece
                conn_ptr->requestPiece(next_piece, blocks);
            }
        }

        // Handle timed out requests
        auto timed_out = conn_ptr->getTimedOutRequests(30);
        if (!timed_out.empty()) {
            std::cout << "Detected " << timed_out.size() << " timed out requests\n";
            // Could retry or move to another peer
        }

        // Process received pieces
        if (message->type == MessageType::PIECE) {
            PieceMessage piece_msg;
            if (conn_ptr->parsePiece(*message, piece_msg)) {
                std::cout << "Received piece data: piece=" << piece_msg.piece_index
                          << " offset=" << piece_msg.offset
                          << " size=" << piece_msg.data.size() << "\n";

                // Add block to piece manager
                piece_manager_->addBlock(piece_msg.piece_index, piece_msg.offset, piece_msg.data);

                // Update statistics
                total_downloaded_ += piece_msg.data.size();

                // Check if piece is complete
                if (piece_manager_->isPieceInProgress(piece_msg.piece_index)) {
                    PieceInProgress* piece = piece_manager_->getPieceInProgress(piece_msg.piece_index);
                    if (piece && piece->isComplete()) {
                        std::cout << "\n========== PIECE COMPLETE ==========\n";
                        std::cout << "All blocks received for piece " << piece_msg.piece_index << "\n";

                        // Complete the piece (verify and write to disk)
                        if (piece_manager_->completePiece(piece_msg.piece_index, file_manager_.get())) {
                            std::cout << "SUCCESS: Piece " << piece_msg.piece_index << " verified and saved!\n";

                            // Send HAVE message to all connected peers
                            broadcastHave(piece_msg.piece_index);

                            std::cout << "===================================\n\n";
                        } else {
                            std::cerr << "FAILED: Piece " << piece_msg.piece_index << " verification or write failed\n";
                            std::cerr << "Will re-request this piece\n";
                        }
                    }
                }
            }
        }

        // Small sleep to prevent busy loop
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "Peer loop ended for " << peer.ip << ":" << peer.port << "\n";
}

void DownloadManager::connectToPeers() {
    std::cout << "Connecting to " << peers_.size() << " peers...\n";

    // Connect to a limited number of peers (e.g., 5 at a time)
    size_t max_peers = 5;
    size_t connected = 0;

    for (const auto& peer : peers_) {
        if (connected >= max_peers) {
            break;
        }

        std::cout << "Starting connection to: " << peer.ip << ":" << peer.port << "\n";

        // Start peer loop in separate thread
        worker_threads_.emplace_back(&DownloadManager::peerLoop, this, peer);
        connected++;
    }

    std::cout << "Started " << connected << " peer connections\n";
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

void DownloadManager::broadcastHave(uint32_t piece_index) {
    std::lock_guard<std::mutex> lock(connections_mutex_);

    std::cout << "Broadcasting HAVE message for piece " << piece_index
              << " to " << connections_.size() << " peers\n";

    size_t sent = 0;
    for (const auto& connection : connections_) {
        if (connection && connection->isConnected()) {
            if (connection->sendHave(piece_index)) {
                sent++;
            }
        }
    }

    std::cout << "HAVE message sent to " << sent << " peers\n";
}

} // namespace torrent
