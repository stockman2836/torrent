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

void DownloadManager::peerLoop(std::shared_ptr<ActivePeerInfo> peer_info) {
    if (!peer_info || !peer_info->connection) {
        return;
    }

    PeerConnection* conn = peer_info->connection.get();
    const Peer peer = peer_info->peer_data;
    std::cout << "Starting peer loop for " << peer.ip << ":" << peer.port << "\n";

    PeerConnection* conn_ptr = conn;

    // Main communication loop
    auto last_keepalive = std::chrono::steady_clock::now();
    auto last_activity = std::chrono::steady_clock::now();

    while (running_ && !paused_ && conn_ptr->isConnected()) {
        flushPeerCommands(*peer_info);
        requestNextPiece(*peer_info);
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

        // Handle timed out requests
        auto timed_out = conn_ptr->getTimedOutRequests(30);
        if (!timed_out.empty()) {
            std::cout << "Detected " << timed_out.size() << " timed out requests from " << peer.ip << "\n";

            // Retry timed out requests
            for (const auto& req : timed_out) {
                {
                    std::lock_guard<std::mutex> lock(peers_mutex_);
                    if (!peer_info->pending_blocks.count({req.piece_index, req.offset})) continue;
                }
                std::cout << "Retrying timed out request: piece=" << req.piece_index
                          << " offset=" << req.offset << " length=" << req.length << "\n";

                // Retry the request
                if (conn_ptr->requestBlock(req.piece_index, req.offset, req.length)) {
                    std::lock_guard<std::mutex> lock(peers_mutex_);
                    const auto request = peer_info->pending_blocks.find({req.piece_index, req.offset});
                    if (request != peer_info->pending_blocks.end()) request->second.requested_at = now;
                    std::cout << "Successfully re-requested block\n";
                } else {
                    std::cerr << "Failed to re-request block (peer may be choking us)\n";
                    releasePeerPiece(*peer_info);
                    conn_ptr->clearPendingRequests();
                    break;
                }
            }
        }

        // Poll while idle so this worker can issue endgame requests and CANCELs.
        if (!conn_ptr->waitForData(100)) continue;
        // Receive and process messages
        auto message = conn_ptr->receiveMessage();
        if (!message) {
            std::cerr << "Connection closed or timeout from peer " << peer.ip << ":" << peer.port << "\n";
            break;
        }

        // Update activity timestamp on message receive
        last_activity = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            peer_info->last_activity = last_activity;
            peer_info->available_pieces = conn_ptr->peerBitfield();
            peer_info->is_choking = conn_ptr->peerChoking();
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

        if (message->type == MessageType::PIECE) {
            PieceMessage block;
            if (conn_ptr->parsePiece(*message, block)) {
                bool requested = false, valid_size = false;
                {
                    std::lock_guard<std::mutex> lock(peers_mutex_);
                    const auto request = peer_info->pending_blocks.find({block.piece_index, block.offset});
                    requested = request != peer_info->pending_blocks.end();
                    valid_size = requested && request->second.block.length == block.data.size();
                }
                // Late responses to cancelled requests must not alter assembly or statistics.
                if (!requested) continue;
                if (!valid_size) break;
                if (receiveBlock(block.piece_index, block.offset, block.data)) {
                    std::lock_guard<std::mutex> lock(peers_mutex_);
                    peer_info->bytes_downloaded += block.data.size();
                }
            }
        }
        if (message->type == MessageType::CHOKE) releasePeerPiece(*peer_info);

        // Handle REQUEST messages (peer wants to download from us)
        if (message->type == MessageType::REQUEST) {
            RequestMessage req_msg;
            if (conn_ptr->parseRequest(*message, req_msg)) {
                std::cout << "Peer " << peer.ip << " requests: piece=" << req_msg.piece_index
                          << " offset=" << req_msg.offset
                          << " length=" << req_msg.length << "\n";

                // Check if we're choking this peer
                if (req_msg.piece_index >= torrent_.numPieces() || req_msg.length == 0 || req_msg.length > 16384) {
                    if (conn_ptr->peerSupportsFastExtension()) conn_ptr->sendRejectRequest(req_msg.piece_index, req_msg.offset, req_msg.length);
                }
                else if (conn_ptr->amChoking()) {
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
                    else if (req_msg.offset >= piece_data.size() || req_msg.length > piece_data.size() - req_msg.offset) {
                        std::cerr << "Invalid REQUEST: offset=" << req_msg.offset
                                  << " length=" << req_msg.length
                                  << " exceeds piece size " << piece_data.size() << "\n";
                    }
                    else if (!std::equal(torrent_.pieces().begin() + req_msg.piece_index * 20,
                                         torrent_.pieces().begin() + req_msg.piece_index * 20 + 20,
                                         utils::sha1(piece_data).begin())) {
                        LOG_WARN("Refusing to upload corrupted piece {}", req_msg.piece_index);
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
                        if (!upload_limiter_.waitAndConsume(block_data.size(), [this] { return !running_.load(); },
                            global_limits_ ? &global_limits_->upload : nullptr)) break;

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
                                peer_info->bytes_uploaded += block_data.size();
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
        waitForStop(std::chrono::milliseconds(10));
    }

    // Deliver final CANCELs even if completion has already stopped the session.
    flushPeerCommands(*peer_info);
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

    releasePeerPiece(*peer_info);
    conn_ptr->disconnect();
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        peer_info->finished = true;
    }

    std::cout << "Peer loop ended for " << peer.ip << ":" << peer.port << "\n";
}

void DownloadManager::connectToPeers() {
    std::unique_lock<std::mutex> connecting(connect_mutex_, std::try_to_lock);
    if (!connecting.owns_lock()) return;
    cleanupStalePeers();
    if (!running_) return;
    for (size_t i = 0; i < max_peers_ && running_; ++i) {
        Peer peer("", 0);
        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            if (active_peers_.size() >= max_peers_ || available_peers_.empty()) break;
            peer = available_peers_.back();
            available_peers_.pop_back();
        }

        std::cout << "Connecting to: " << peer.ip << ":" << peer.port << "\n";

        // Create connection
        auto connection = std::make_unique<PeerConnection>(
            peer.ip, peer.port, torrent_.infoHash(), peer_id_
        );

        connection->setCancellationCallback([this] { return !running_; });
        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            connecting_peers_ = {{peer.ip, peer.port, PeerState::Connecting}};
        }
        const auto failed = [&](const char* reason) {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            connecting_peers_.clear();
            if (running_) {
                ++failed_connections_;
                last_peer_error_ = peer.ip + ":" + std::to_string(peer.port) + " — " + reason;
            }
        };
        if (!connection->connect()) {
            failed(u8"Не удалось установить TCP-соединение");
            std::cerr << "Failed to connect to " << peer.ip << "\n";
            continue;
        }

        connection->initializePeerBitfield(torrent_.numPieces());
        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            connecting_peers_.front().state = PeerState::Handshaking;
        }

        std::vector<bool> our_bitfield = piece_manager_->getBitfield();
        if (!connection->performHandshake(our_bitfield)) {
            failed(u8"Не удалось завершить обмен приветствиями BitTorrent");
            std::cerr << "Failed handshake with " << peer.ip << "\n";
            continue;
        }

        // Availability was sent once by the handshake; advertise allowed pieces
        // only after it, never a second bitfield/HAVE_ALL/HAVE_NONE.
        if (connection->peerSupportsFastExtension()) {
            connection->generateAllowedFastSet(torrent_.numPieces(), 10);
        }

        // Enable PEX if configured
        if (enable_pex_) {
            connection->enablePex();
            connection->sendExtendedHandshake();
        }

        // Add to active peers
        std::lock_guard<std::mutex> lock(peers_mutex_);
        connecting_peers_.clear();
        if (!running_ || active_peers_.size() >= max_peers_) continue;
        auto peer_info = std::make_shared<ActivePeerInfo>(std::move(connection), peer);
        active_peers_.push_back(peer_info);

        // Start peer thread
        peer_threads_.push_back({peer_info, std::thread(&DownloadManager::peerLoop, this, peer_info)});

        std::cout << "Connected to " << peer.ip << " (total active: " << active_peers_.size() << ")\n";
    }
}

void DownloadManager::broadcastHave(uint32_t piece_index) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    for (auto& peer_info : active_peers_) {
        if (!peer_info->finished) peer_info->pending_haves.push_back(piece_index);
    }
}

} // namespace torrent
