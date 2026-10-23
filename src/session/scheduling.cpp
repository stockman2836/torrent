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

void DownloadManager::coordinatorLoop() {
    std::cout << "Coordinator loop started\n";

    while (running_) {
        if (paused_) {
            waitForStop(std::chrono::milliseconds(500));
            continue;
        }

        // Cleanup stale peers
        cleanupStalePeers();

        // Check for endgame mode
        if (!endgame_mode_ && isEndgameMode()) {
            endgame_mode_ = true;
            std::cout << "\n*** ENTERING ENDGAME MODE ***\n";
        }

        // Sleep before next coordination cycle
        waitForStop(std::chrono::seconds(1));
    }

    std::cout << "Coordinator loop ended\n";
}

void DownloadManager::requestNextPiece(ActivePeerInfo& peer) {
    if (!peer.connection->canDownload() || peer.connection->hasPendingRequests()) return;
    std::lock_guard<std::mutex> lock(peers_mutex_);
    if (!running_ || peer.current_piece != UINT32_MAX) return;
    const auto index = selectNextPiece(&peer);
    if (index < 0) return;
    peer.current_piece = index;
    {
        std::lock_guard<std::mutex> pieces_lock(pieces_mutex_);
        pieces_in_download_.insert(index);
    }
    if (!peer.connection->requestPiece(index, piece_manager_->getBlocksForPiece(index))) {
        std::lock_guard<std::mutex> pieces_lock(pieces_mutex_);
        pieces_in_download_.erase(index);
        peer.current_piece = UINT32_MAX;
    }
}

void DownloadManager::releasePeerPiece(ActivePeerInfo& peer) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    if (peer.current_piece == UINT32_MAX) return;
    std::lock_guard<std::mutex> pieces_lock(pieces_mutex_);
    pieces_in_download_.erase(peer.current_piece);
    piece_manager_->discardPiece(peer.current_piece);
    peer.current_piece = UINT32_MAX;
}

int32_t DownloadManager::selectNextPiece(ActivePeerInfo* peer) {
    if (!peer || !peer->connection) {
        return -1;
    }

    // Preserve reservations until redundant endgame requests support CANCEL.
    const auto& peer_has_pieces = peer->available_pieces;
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
        // requestNextPiece already holds peers_mutex_.
        for (const auto& peer_info : active_peers_) {
            if (!peer_info->finished) {
                all_peer_bitfields.push_back(peer_info->available_pieces);
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
        if ((*it)->finished) {
            std::cout << "Removing stale/disconnected peer " << (*it)->peer_data.ip << "\n";

            // Remove piece assignment
            if ((*it)->current_piece != UINT32_MAX) {
                std::lock_guard<std::mutex> pieces_lock(pieces_mutex_);
                pieces_in_download_.erase((*it)->current_piece);
            }

            it = active_peers_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace torrent
