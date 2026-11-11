#include "torrent/session/download_manager.h"
#include "peer_state.h"

namespace torrent {

NetworkStatus DownloadManager::networkStatus() const {
    NetworkStatus result;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        result.tracker = tracker_status_;
        result.queued_peers = available_peers_.size();
        result.failed_connections = failed_connections_;
        result.last_peer_error = last_peer_error_;
        result.peers = connecting_peers_;
        for (const auto& peer : active_peers_) {
            if (peer->finished) continue;
            result.peers.push_back({peer->peer_data.ip, peer->peer_data.port,
                peer->handshaking ? PeerState::Handshaking : PeerState::Connected,
                peer->incoming, peer->is_choking, peer->pending_blocks.size(),
                peer->bytes_downloaded, peer->bytes_uploaded});
        }
    }
    result.dht_enabled = enable_dht_;
    if (dht_manager_) result.dht_nodes = dht_manager_->getNodeCount();
    return result;
}

} // namespace torrent
