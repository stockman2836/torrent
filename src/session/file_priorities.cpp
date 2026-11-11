#include "torrent/session/download_manager.h"
#include "torrent/metainfo/file_selection.h"
#include "peer_state.h"

namespace torrent {

void DownloadManager::setFilePriorities(const std::map<size_t, FilePriority>& priorities) {
    const FileSelection selection(torrent_, {}, priorities);
    std::lock_guard<std::mutex> selection_lock(selection_mutex_);
    std::lock_guard<std::mutex> peers_lock(peers_mutex_);
    std::lock_guard<std::mutex> pieces_lock(pieces_mutex_);
    file_manager_->setSelection(selection, piece_manager_->getBitfield());
    piece_manager_->setPiecePriorities(selection.piecePriorities());
    for (auto& peer : active_peers_) {
        for (auto request = peer->pending_blocks.begin(); request != peer->pending_blocks.end();) {
            if (selection.pieces()[request->first.first]) { ++request; continue; }
            peer->pending_cancels.push_back(request->second.block);
            request = peer->pending_blocks.erase(request);
        }
        if (peer->current_piece != UINT32_MAX && !selection.pieces()[peer->current_piece])
            peer->current_piece = UINT32_MAX;
    }
    for (auto piece = pieces_in_download_.begin(); piece != pieces_in_download_.end();) {
        if (!selection.pieces()[*piece]) piece = pieces_in_download_.erase(piece);
        else ++piece;
    }
}

} // namespace torrent
