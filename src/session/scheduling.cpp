#include "torrent/session/download_manager.h"
#include "peer_state.h"
#include <algorithm>
#include <iostream>
#include <optional>

namespace torrent {

void DownloadManager::coordinatorLoop() {
    while (running_) {
        if (!paused_) cleanupStalePeers();
        waitForStop(std::chrono::seconds(1));
    }
}

void DownloadManager::requestNextPiece(ActivePeerInfo& peer) {
    if (!peer.connection->canDownload() || peer.connection->hasPendingRequests()) return;
    std::lock_guard<std::mutex> lock(peers_mutex_);
    if (!running_) return;
    if (peer.current_piece == UINT32_MAX) {
        const auto index = selectNextPiece(&peer);
        if (index >= 0) {
            peer.current_piece = index;
            {
                std::lock_guard<std::mutex> pieces_lock(pieces_mutex_);
                pieces_in_download_.insert(index);
            }
            const auto blocks = piece_manager_->getMissingBlocks(index);
            peer.connection->requestPiece(index, blocks);
            for (const auto& block : blocks) {
                if (peer.connection->isPendingRequest(block.piece_index, block.offset)) {
                    peer.pending_blocks.emplace(ActivePeerInfo::BlockKey{block.piece_index, block.offset},
                        ActivePeerInfo::PendingBlock{block, std::chrono::steady_clock::now()});
                }
            }
            if (peer.connection->hasPendingRequests()) return;
            std::lock_guard<std::mutex> pieces_lock(pieces_mutex_);
            pieces_in_download_.erase(index);
            peer.current_piece = UINT32_MAX;
        }
    }
    requestEndgameBlock(peer);
}

void DownloadManager::requestEndgameBlock(ActivePeerInfo& peer) {
    if (!isEndgameMode()) return;
    std::lock_guard<std::mutex> lock(pieces_mutex_);
    const auto bitfield = piece_manager_->getBitfield();
    const auto now = std::chrono::steady_clock::now();
    std::optional<Block> unassigned, duplicate;
    bool all_requested = true;
    for (uint32_t index = 0; index < bitfield.size(); ++index) {
        if (bitfield[index]) continue;
        for (const auto& block : piece_manager_->getMissingBlocks(index)) {
            const ActivePeerInfo::BlockKey key{index, block.offset};
            size_t owners = 0;
            auto oldest = now;
            for (const auto& other : active_peers_) {
                const auto request = other->pending_blocks.find(key);
                if (!other->finished && request != other->pending_blocks.end()) {
                    ++owners;
                    oldest = std::min(oldest, request->second.requested_at);
                }
            }
            const bool available = index < peer.available_pieces.size() && peer.available_pieces[index];
            if (owners == 0) {
                all_requested = false;
                if (available && pieces_in_download_.count(index) && !unassigned) unassigned = block;
            } else if (available && owners < 2 && !peer.pending_blocks.count(key) && !duplicate &&
                       now - oldest >= std::chrono::milliseconds(500)) {
                duplicate = block;
            }
        }
    }
    const auto candidate = unassigned ? unassigned : (all_requested ? duplicate : std::nullopt);
    if (!candidate) return;
    const auto& block = *candidate;
    if (peer.connection->requestBlock(block.piece_index, block.offset, block.length)) {
        peer.pending_blocks.emplace(ActivePeerInfo::BlockKey{block.piece_index, block.offset},
            ActivePeerInfo::PendingBlock{block, now});
        endgame_mode_ = true;
        std::cout << "Endgame: requested block " << block.piece_index << ':' << block.offset << '\n';
    }
}

void DownloadManager::releasePeerPiece(ActivePeerInfo& peer) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    std::lock_guard<std::mutex> pieces_lock(pieces_mutex_);
    std::set<uint32_t> affected;
    if (peer.current_piece != UINT32_MAX) affected.insert(peer.current_piece);
    for (const auto& request : peer.pending_blocks) affected.insert(request.first.first);
    peer.current_piece = UINT32_MAX;
    peer.pending_blocks.clear();
    peer.pending_cancels.clear();
    for (const auto index : affected) {
        const bool shared = std::any_of(active_peers_.begin(), active_peers_.end(), [&](const auto& other) {
            if (other.get() == &peer || other->finished) return false;
            if (other->current_piece == index) return true;
            return std::any_of(other->pending_blocks.begin(), other->pending_blocks.end(),
                [&](const auto& request) { return request.first.first == index; });
        });
        if (!shared) {
            pieces_in_download_.erase(index);
            piece_manager_->discardPiece(index);
        }
    }
}

int32_t DownloadManager::selectNextPiece(ActivePeerInfo* peer) {
    const auto& available = peer->available_pieces;
    std::lock_guard<std::mutex> lock(pieces_mutex_);
    if (piece_manager_->isSequentialMode()) {
        return piece_manager_->getNextPieceSequential(available, pieces_in_download_);
    }
    const auto random = piece_manager_->getNextPieceRandomFirst(available, pieces_in_download_);
    if (random >= 0) return random;
    std::vector<std::vector<bool>> bitfields;
    for (const auto& other : active_peers_) {
        if (!other->finished) bitfields.push_back(other->available_pieces);
    }
    return piece_manager_->getNextPieceRarestFirst(bitfields, available, pieces_in_download_);
}

bool DownloadManager::isEndgameMode() const {
    const auto remaining = torrent_.numPieces() - piece_manager_->numPiecesDownloaded();
    return remaining > 0 && remaining <= 5;
}

void DownloadManager::cleanupStalePeers() {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    active_peers_.erase(std::remove_if(active_peers_.begin(), active_peers_.end(),
        [](const auto& peer) { return peer->finished; }), active_peers_.end());
}

} // namespace torrent
