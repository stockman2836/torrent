#include "torrent/session/download_manager.h"
#include "peer_state.h"
#include <algorithm>

namespace torrent {

void DownloadManager::receiveBlock(uint32_t index, uint32_t offset, const std::vector<uint8_t>& data) {
    const auto result = piece_manager_->receiveBlock(index, offset, data, file_manager_.get());
    if (result == PieceManager::BlockResult::Rejected) return;
    total_downloaded_ += data.size();
    download_tracker_.addBytes(data.size());
    if (result == PieceManager::BlockResult::Stored) return;

    {
        std::lock_guard<std::mutex> peers_lock(peers_mutex_);
        std::lock_guard<std::mutex> pieces_lock(pieces_mutex_);
        pieces_in_download_.erase(index);
        for (auto& peer : active_peers_) {
            if (peer->current_piece == index) {
                peer->current_piece = UINT32_MAX;
                if (result == PieceManager::BlockResult::Failed) peer->failed_requests++;
            }
        }
    }
    if (result == PieceManager::BlockResult::Complete) broadcastHave(index);
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
