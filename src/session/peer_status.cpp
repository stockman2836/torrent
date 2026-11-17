#include "torrent/session/download_manager.h"
#include "peer_state.h"
#include <algorithm>

namespace torrent {

PeerStatus DownloadManager::peerStatus(const ActivePeerInfo& peer) const {
    PeerStatus result{peer.peer_data.ip, peer.peer_data.port,
        peer.handshaking ? PeerState::Handshaking : PeerState::Connected,
        peer.incoming, peer.is_choking, peer.pending_blocks.size(), peer.bytes_downloaded, peer.bytes_uploaded};
    result.id = peer.id;
    result.client = peer.client;
    result.download_speed = static_cast<int64_t>(peer.download_rate.getSpeed());
    result.upload_speed = static_cast<int64_t>(peer.upload_rate.getSpeed());
    result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - peer.connect_time).count();
    return result;
}

void DownloadManager::finishPeer(ActivePeerInfo& peer, PeerEndReason reason) {
    if (peer.finished) return;
    auto result = peerStatus(peer);
    result.state = PeerState::Disconnected;
    result.download_speed = result.upload_speed = 0;
    result.pending_requests = 0;
    result.finished_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    result.end_reason = !running_ ? PeerEndReason::Stopped : paused_ ? PeerEndReason::Paused :
        reason != PeerEndReason::None ? reason : peer.connection->endReason();
    if (result.end_reason == PeerEndReason::None) result.end_reason = PeerEndReason::Unknown;
    result.error_code = result.end_reason == PeerEndReason::Stopped || result.end_reason == PeerEndReason::Paused ? 0 :
        peer.connection->errorCode();
    if (peer_history_.size() == PEER_HISTORY_LIMIT) peer_history_.erase(peer_history_.begin());
    peer_history_.push_back(std::move(result));
    peer.finished = true;
}

} // namespace torrent
