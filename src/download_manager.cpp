#include "download_manager.h"
#include "utils.h"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>

namespace torrent {

DownloadManager::DownloadManager(const std::string& torrent_path,
                                const std::string& download_dir,
                                uint16_t listen_port,
                                int64_t max_download_speed,
                                int64_t max_upload_speed)
    : download_dir_(download_dir)
    , peer_id_(utils::generatePeerId())
    , listen_port_(listen_port)
    , running_(false)
    , paused_(false)
    , endgame_mode_(false)
    , seeding_mode_(false)
    , total_downloaded_(0)
    , total_uploaded_(0)
    , download_limiter_(max_download_speed)
    , upload_limiter_(max_upload_speed)
    , download_tracker_(20)  // 20 second window
    , upload_tracker_(20) {

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
    worker_threads_.emplace_back(&DownloadManager::coordinatorLoop, this);
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

    running_ = false;

    // Wait for threads to finish
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    worker_threads_.clear();

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
            if (peer.current_piece != UINT32_MAX) {
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
    std::this_thread::sleep_for(std::chrono::seconds(2));

    while (running_) {
        if (paused_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
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

void DownloadManager::peerLoop(size_t peer_index) {
    // Get peer info
    PeerInfo* peer_info = nullptr;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        if (peer_index >= active_peers_.size()) {
            return;
        }
        peer_info = &active_peers_[peer_index];
    }

    if (!peer_info || !peer_info->connection) {
        return;
    }

    PeerConnection* conn = peer_info->connection.get();
    const Peer& peer = peer_info->peer_data;
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
    auto last_keepalive = std::chrono::steady_clock::now();
    auto last_activity = std::chrono::steady_clock::now();

    while (running_ && !paused_ && conn_ptr->isConnected()) {
        // Send keep-alive periodically (every 2 minutes)
        auto now = std::chrono::steady_clock::now();
        auto since_keepalive = std::chrono::duration_cast<std::chrono::seconds>(now - last_keepalive).count();
        if (since_keepalive >= 120) {
            conn_ptr->sendKeepAlive();
            last_keepalive = now;
        }

        // Check for connection timeout (5 minutes no activity)
        auto since_activity = std::chrono::duration_cast<std::chrono::seconds>(now - last_activity).count();
        if (since_activity >= 300) {
            std::cerr << "Connection timeout: no activity for " << since_activity << " seconds from "
                      << peer.ip << ":" << peer.port << "\n";
            break;
        }

        // Receive and process messages
        auto message = conn_ptr->receiveMessage();
        if (!message) {
            std::cerr << "Connection closed or timeout from peer " << peer.ip << ":" << peer.port << "\n";
            break;
        }

        // Update activity timestamp on message receive
        last_activity = std::chrono::steady_clock::now();

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
            std::cout << "Detected " << timed_out.size() << " timed out requests from " << peer.ip << "\n";

            // Retry timed out requests
            for (const auto& req : timed_out) {
                std::cout << "Retrying timed out request: piece=" << req.piece_index
                          << " offset=" << req.offset << " length=" << req.length << "\n";

                // Retry the request
                if (conn_ptr->requestBlock(req.piece_index, req.offset, req.length)) {
                    std::cout << "Successfully re-requested block\n";
                } else {
                    std::cerr << "Failed to re-request block (peer may be choking us)\n";
                    // Note: If retry fails, coordinatorLoop will eventually reassign to another peer
                }
            }
        }

        // Process received pieces
        if (message->type == MessageType::PIECE) {
            PieceMessage piece_msg;
            if (conn_ptr->parsePiece(*message, piece_msg)) {
                std::cout << "Received piece data: piece=" << piece_msg.piece_index
                          << " offset=" << piece_msg.offset
                          << " size=" << piece_msg.data.size() << "\n";

                // Apply download rate limiting
                download_limiter_.waitAndConsume(piece_msg.data.size());

                // Add block to piece manager
                piece_manager_->addBlock(piece_msg.piece_index, piece_msg.offset, piece_msg.data);

                // Update statistics
                total_downloaded_ += piece_msg.data.size();
                download_tracker_.addBytes(piece_msg.data.size());

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
                            std::cerr << "Marking piece for re-download\n";

                            // Remove from pieces_in_download so it can be reassigned
                            {
                                std::lock_guard<std::mutex> pieces_lock(pieces_mutex_);
                                pieces_in_download_.erase(piece_msg.piece_index);
                            }

                            // Clear current_piece assignment from peer
                            {
                                std::lock_guard<std::mutex> peers_lock(peers_mutex_);
                                for (auto& peer_info : active_peers_) {
                                    if (peer_info.current_piece == piece_msg.piece_index) {
                                        peer_info.current_piece = UINT32_MAX;
                                        peer_info.failed_requests++;
                                        break;
                                    }
                                }
                            }

                            std::cout << "Piece " << piece_msg.piece_index << " will be re-requested from another peer\n";
                        }
                    }
                }
            }
        }

        // Handle REQUEST messages (peer wants to download from us)
        if (message->type == MessageType::REQUEST) {
            RequestMessage req_msg;
            if (conn_ptr->parseRequest(*message, req_msg)) {
                std::cout << "Peer " << peer.ip << " requests: piece=" << req_msg.piece_index
                          << " offset=" << req_msg.offset
                          << " length=" << req_msg.length << "\n";

                // Check if we're choking this peer
                if (conn_ptr->amChoking()) {
                    std::cout << "Ignoring REQUEST: we are choking this peer\n";
                }
                // Check if we have the requested piece
                else if (!piece_manager_->hasPiece(req_msg.piece_index)) {
                    std::cout << "Ignoring REQUEST: we don't have piece " << req_msg.piece_index << "\n";
                }
                // Validate request parameters
                else if (req_msg.length == 0 || req_msg.length > 16384) {
                    std::cerr << "Invalid REQUEST: length=" << req_msg.length << " (max 16384)\n";
                }
                else {
                    // Read the complete piece from disk
                    std::vector<uint8_t> piece_data = file_manager_->readPiece(req_msg.piece_index);

                    if (piece_data.empty()) {
                        std::cerr << "Failed to read piece " << req_msg.piece_index << " from disk\n";
                    }
                    // Validate offset and length
                    else if (req_msg.offset + req_msg.length > piece_data.size()) {
                        std::cerr << "Invalid REQUEST: offset=" << req_msg.offset
                                  << " length=" << req_msg.length
                                  << " exceeds piece size " << piece_data.size() << "\n";
                    }
                    else {
                        // Extract the requested block
                        std::vector<uint8_t> block_data(
                            piece_data.begin() + req_msg.offset,
                            piece_data.begin() + req_msg.offset + req_msg.length
                        );

                        // Track this upload
                        conn_ptr->addPendingUpload(req_msg.piece_index, req_msg.offset, req_msg.length);

                        // Apply upload rate limiting
                        upload_limiter_.waitAndConsume(block_data.size());

                        // Send PIECE message
                        if (conn_ptr->sendPiece(req_msg.piece_index, req_msg.offset, block_data)) {
                            std::cout << "Uploaded block: piece=" << req_msg.piece_index
                                      << " offset=" << req_msg.offset
                                      << " size=" << block_data.size() << " bytes to " << peer.ip << "\n";

                            // Update upload statistics
                            total_uploaded_ += block_data.size();
                            upload_tracker_.addBytes(block_data.size());

                            // Update peer statistics
                            {
                                std::lock_guard<std::mutex> lock(peers_mutex_);
                                if (peer_index < active_peers_.size()) {
                                    active_peers_[peer_index].bytes_uploaded += block_data.size();
                                }
                            }

                            // Remove from pending uploads
                            conn_ptr->removePendingUpload(req_msg.piece_index, req_msg.offset);
                        } else {
                            std::cerr << "Failed to send PIECE message to " << peer.ip << "\n";
                            conn_ptr->removePendingUpload(req_msg.piece_index, req_msg.offset);
                        }
                    }
                }
            }
        }

        // Handle CANCEL messages (peer cancels a previous request)
        if (message->type == MessageType::CANCEL) {
            CancelMessage cancel_msg;
            if (conn_ptr->parseCancel(*message, cancel_msg)) {
                std::cout << "Peer " << peer.ip << " cancels: piece=" << cancel_msg.piece_index
                          << " offset=" << cancel_msg.offset
                          << " length=" << cancel_msg.length << "\n";

                // Remove from pending uploads if present
                if (conn_ptr->isPendingUpload(cancel_msg.piece_index, cancel_msg.offset)) {
                    conn_ptr->removePendingUpload(cancel_msg.piece_index, cancel_msg.offset);
                    std::cout << "Cancelled pending upload: piece=" << cancel_msg.piece_index
                              << " offset=" << cancel_msg.offset << "\n";
                } else {
                    std::cout << "No pending upload found for cancellation\n";
                }
            }
        }

        // Small sleep to prevent busy loop
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Graceful cleanup on peer disconnection
    std::cout << "Peer loop ending for " << peer.ip << ":" << peer.port;
    if (!conn_ptr->isConnected()) {
        std::cout << " (disconnected)";
    }
    std::cout << "\n";

    // Clear pending requests - blocks will be re-requested by other peers
    if (conn_ptr->hasPendingRequests()) {
        std::cout << "Clearing " << conn_ptr->numPendingRequests() << " pending requests from disconnected peer\n";
        conn_ptr->clearPendingRequests();
    }

    // Mark piece assignment as free (will be cleaned up by cleanupStalePeers)
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (auto& peer_info : active_peers_) {
            if (peer_info.connection.get() == conn_ptr) {
                if (peer_info.current_piece != UINT32_MAX) {
                    std::cout << "Freeing piece assignment " << peer_info.current_piece << " from disconnected peer\n";

                    // Remove from pieces_in_download so it can be reassigned
                    {
                        std::lock_guard<std::mutex> pieces_lock(pieces_mutex_);
                        pieces_in_download_.erase(peer_info.current_piece);
                    }

                    peer_info.current_piece = UINT32_MAX;
                }
                break;
            }
        }
    }

    std::cout << "Peer loop ended for " << peer.ip << ":" << peer.port << "\n";
}

void DownloadManager::connectToPeers() {
    std::lock_guard<std::mutex> lock(peers_mutex_);

    std::cout << "Connecting to peers (have " << available_peers_.size()
              << ", active " << active_peers_.size() << "/" << max_peers_ << ")...\n";

    // Connect to more peers if below max
    size_t to_connect = std::min(
        available_peers_.size(),
        max_peers_ - active_peers_.size()
    );

    for (size_t i = 0; i < to_connect && !available_peers_.empty(); ++i) {
        Peer peer = available_peers_.back();
        available_peers_.pop_back();

        std::cout << "Connecting to: " << peer.ip << ":" << peer.port << "\n";

        // Create connection
        auto connection = std::make_unique<PeerConnection>(
            peer.ip, peer.port, torrent_.infoHash(), peer_id_
        );

        if (!connection->connect()) {
            std::cerr << "Failed to connect to " << peer.ip << "\n";
            continue;
        }

        connection->initializePeerBitfield(torrent_.numPieces());

        std::vector<bool> our_bitfield = piece_manager_->getBitfield();
        if (!connection->performHandshake(our_bitfield)) {
            std::cerr << "Failed handshake with " << peer.ip << "\n";
            continue;
        }

        // Add to active peers
        active_peers_.emplace_back(std::move(connection), peer);
        size_t peer_idx = active_peers_.size() - 1;

        // Start peer thread
        worker_threads_.emplace_back(&DownloadManager::peerLoop, this, peer_idx);

        std::cout << "Connected to " << peer.ip << " (total active: " << active_peers_.size() << ")\n";
    }
}

void DownloadManager::updateTracker() {
    int64_t left = torrent_.totalLength() - total_downloaded_;

    // Determine event type
    std::string event = "";
    if (first_announce_) {
        event = "started";
        first_announce_ = false;
    }
    // Note: "completed" event is sent directly from downloadLoop when transitioning to seeding

    try {
        TrackerResponse response = tracker_client_->announce(
            total_uploaded_,
            total_downloaded_,
            left,
            listen_port_,
            event
        );

        if (!response.isSuccess()) {
            tracker_failures_++;
            std::cerr << "Tracker error (" << tracker_failures_ << " failures): "
                      << response.failure_reason << "\n";

            // Exponential backoff for retries
            if (tracker_failures_ <= 10) {
                int backoff_ms = utils::calculateBackoffDelay(tracker_failures_ - 1, 1000, 60000);
                std::cout << "Will retry tracker in " << (backoff_ms / 1000) << " seconds\n";
                // Note: retry happens in next trackerLoop iteration
            } else {
                std::cerr << "Too many tracker failures, continuing with existing peers\n";
            }
            return;
        }

        // Success - reset failure counter
        tracker_failures_ = 0;

        std::cout << "Tracker response: " << response.peers.size() << " peers, "
                  << response.complete << " seeders, "
                  << response.incomplete << " leechers\n";

        available_peers_ = response.peers;
        connectToPeers();

    } catch (const std::exception& e) {
        tracker_failures_++;
        std::cerr << "Tracker exception (" << tracker_failures_ << " failures): "
                  << e.what() << "\n";

        if (tracker_failures_ <= 10) {
            int backoff_ms = utils::calculateBackoffDelay(tracker_failures_ - 1, 1000, 60000);
            std::cout << "Will retry tracker in " << (backoff_ms / 1000) << " seconds\n";
        }
    }
}

void DownloadManager::broadcastHave(uint32_t piece_index) {
    std::lock_guard<std::mutex> lock(peers_mutex_);

    std::cout << "Broadcasting HAVE message for piece " << piece_index
              << " to " << active_peers_.size() << " peers\n";

    size_t sent = 0;
    for (auto& peer_info : active_peers_) {
        if (peer_info.connection && peer_info.connection->isConnected()) {
            if (peer_info.connection->sendHave(piece_index)) {
                sent++;
            }
        }
    }

    std::cout << "HAVE message sent to " << sent << " peers\n";
}

// Coordinator loop - manages piece distribution across peers
void DownloadManager::coordinatorLoop() {
    std::cout << "Coordinator loop started\n";

    while (running_) {
        if (paused_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        // Assign pieces to available peers
        assignPiecesToPeers();

        // Cleanup stale peers
        cleanupStalePeers();

        // Check for endgame mode
        if (!endgame_mode_ && isEndgameMode()) {
            endgame_mode_ = true;
            std::cout << "\n*** ENTERING ENDGAME MODE ***\n";
        }

        // Sleep before next coordination cycle
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cout << "Coordinator loop ended\n";
}

void DownloadManager::assignPiecesToPeers() {
    std::lock_guard<std::mutex> lock(peers_mutex_);

    for (auto& peer_info : active_peers_) {
        if (!peer_info.connection || !peer_info.connection->canDownload()) {
            continue;
        }

        // Skip if peer is already downloading something
        if (peer_info.current_piece != UINT32_MAX) {
            continue;
        }

        // Select next piece for this peer
        int32_t next_piece = selectNextPiece(&peer_info);
        if (next_piece < 0) {
            continue;
        }

        // Assign piece to peer
        peer_info.current_piece = next_piece;

        // Mark piece as being downloaded
        {
            std::lock_guard<std::mutex> pieces_lock(pieces_mutex_);
            pieces_in_download_.insert(next_piece);
        }

        std::cout << "Assigned piece #" << next_piece << " to peer "
                  << peer_info.peer_data.ip << "\n";
    }
}

int32_t DownloadManager::selectNextPiece(PeerInfo* peer) {
    if (!peer || !peer->connection) {
        return -1;
    }

    // In endgame mode, request any missing piece
    if (endgame_mode_) {
        for (uint32_t i = 0; i < torrent_.numPieces(); ++i) {
            if (!piece_manager_->hasPiece(i) && peer->connection->peerHasPiece(i)) {
                return i;
            }
        }
        return -1;
    }

    std::vector<bool> peer_has_pieces = peer->connection->peerBitfield();
    std::lock_guard<std::mutex> pieces_lock(pieces_mutex_);

    // Sequential mode - user preference
    if (piece_manager_->isSequentialMode()) {
        return piece_manager_->getNextPieceSequential(peer_has_pieces, pieces_in_download_);
    }

    // Try random-first for initial pieces (improves swarm health)
    int32_t random_piece = piece_manager_->getNextPieceRandomFirst(peer_has_pieces, pieces_in_download_);
    if (random_piece >= 0) {
        return random_piece;
    }

    // Collect all peer bitfields for rarity calculation
    std::vector<std::vector<bool>> all_peer_bitfields;
    {
        std::lock_guard<std::mutex> peers_lock(peers_mutex_);
        for (const auto& peer_info : active_peers_) {
            if (peer_info.connection && peer_info.connection->isConnected()) {
                all_peer_bitfields.push_back(peer_info.connection->peerBitfield());
            }
        }
    }

    // Rarest-first strategy
    return piece_manager_->getNextPieceRarestFirst(all_peer_bitfields, peer_has_pieces, pieces_in_download_);
}

bool DownloadManager::isEndgameMode() const {
    // Enter endgame when less than 5 pieces remaining
    size_t remaining = torrent_.numPieces() - piece_manager_->numPiecesDownloaded();
    return remaining > 0 && remaining <= 5;
}

void DownloadManager::cleanupStalePeers() {
    std::lock_guard<std::mutex> lock(peers_mutex_);

    for (auto it = active_peers_.begin(); it != active_peers_.end(); ) {
        if (!it->connection || !it->connection->isConnected() || it->isStale(60)) {
            std::cout << "Removing stale/disconnected peer " << it->peer_data.ip << "\n";

            // Remove piece assignment
            if (it->current_piece != UINT32_MAX) {
                std::lock_guard<std::mutex> pieces_lock(pieces_mutex_);
                pieces_in_download_.erase(it->current_piece);
            }

            it = active_peers_.erase(it);
        } else {
            ++it;
        }
    }
}

PeerInfo* DownloadManager::selectPeerForPiece(uint32_t piece_index) {
    std::lock_guard<std::mutex> lock(peers_mutex_);

    PeerInfo* best_peer = nullptr;
    double best_speed = 0.0;

    for (auto& peer : active_peers_) {
        if (peer.connection &&
            peer.connection->canDownload() &&
            peer.connection->peerHasPiece(piece_index) &&
            peer.current_piece == UINT32_MAX) {

            double speed = peer.downloadSpeed();
            if (speed > best_speed) {
                best_speed = speed;
                best_peer = &peer;
            }
        }
    }

    return best_peer;
}

} // namespace torrent
