#include "torrent/session/download_manager.h"
#include "peer_state.h"
#include <algorithm>

namespace torrent {

void DownloadManager::receiveBlock(uint32_t index, uint32_t offset, const std::vector<uint8_t>& data) {
    const auto result = piece_manager_->receiveBlock(index, offset, data, file_manager_.get());
    if (result == PieceManager::BlockResult::Rejected) return;
    total_downloaded_ += data.size();
    download_tracker_.addBytes(data.size());
    {
        std::lock_guard<std::mutex> peers_lock(peers_mutex_);
        std::lock_guard<std::mutex> pieces_lock(pieces_mutex_);
        const bool terminal = result != PieceManager::BlockResult::Stored;
        if (terminal) pieces_in_download_.erase(index);
        for (auto& peer : active_peers_) {
            for (auto request = peer->pending_blocks.begin(); request != peer->pending_blocks.end();) {
                if (request->first.first == index && (terminal || request->first.second == offset)) {
                    peer->pending_cancels.push_back(request->second.block);
                    request = peer->pending_blocks.erase(request);
                } else ++request;
            }
            if (terminal && peer->current_piece == index) {
                peer->current_piece = UINT32_MAX;
                if (result == PieceManager::BlockResult::Failed) peer->failed_requests++;
            }
        }
    }
    if (result == PieceManager::BlockResult::Complete) broadcastHave(index);
}

void DownloadManager::flushPeerCommands(ActivePeerInfo& peer) {
    std::vector<Block> cancels;
    std::vector<uint32_t> haves;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        cancels.swap(peer.pending_cancels);
        haves.swap(peer.pending_haves);
    }
    if (!peer.connection->isConnected()) return;
    for (const auto& block : cancels) {
        if (peer.connection->isPendingRequest(block.piece_index, block.offset)) {
            peer.connection->sendCancel(block.piece_index, block.offset, block.length);
            peer.connection->removeRequest(block.piece_index, block.offset);
        }
    }
    for (const auto index : haves) peer.connection->sendHave(index);
}

int64_t DownloadManager::bytesRemaining() const {
    const auto bitfield = piece_manager_->getBitfield();
    int64_t remaining = 0;
    for (size_t i = 0; i < bitfield.size(); ++i) {
        if (!bitfield[i]) remaining += std::min<int64_t>(torrent_.pieceLength(),
            torrent_.totalLength() - static_cast<int64_t>(i) * torrent_.pieceLength());
    }
    return remaining;
}

} // namespace torrent
