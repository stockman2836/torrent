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
        waitForStop(std::chrono::seconds(1));
    }

    std::cout << "Coordinator loop ended\n";
}

void DownloadManager::assignPiecesToPeers() {
    std::lock_guard<std::mutex> lock(peers_mutex_);

    for (auto& peer_info : active_peers_) {
        if (!peer_info->connection || !peer_info->connection->canDownload()) {
            continue;
        }

        // Skip if peer is already downloading something
        if (peer_info->current_piece != UINT32_MAX) {
            continue;
        }

        // Select next piece for this peer
        int32_t next_piece = selectNextPiece(peer_info.get());
        if (next_piece < 0) {
            continue;
        }

        // Assign piece to peer
        peer_info->current_piece = next_piece;

        // Mark piece as being downloaded
        {
            std::lock_guard<std::mutex> pieces_lock(pieces_mutex_);
            pieces_in_download_.insert(next_piece);
        }

        std::cout << "Assigned piece #" << next_piece << " to peer "
                  << peer_info->peer_data.ip << "\n";
    }
}

int32_t DownloadManager::selectNextPiece(ActivePeerInfo* peer) {
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
        // assignPiecesToPeers already holds peers_mutex_.
        for (const auto& peer_info : active_peers_) {
            if (peer_info->connection && peer_info->connection->isConnected()) {
                all_peer_bitfields.push_back(peer_info->connection->peerBitfield());
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
        if (!(*it)->connection || !(*it)->connection->isConnected() || (*it)->isStale(60)) {
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
