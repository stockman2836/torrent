#include "torrent/session/download_manager.h"
#include "peer_state.h"

namespace torrent {

NetworkStatus DownloadManager::networkStatus() const {
    NetworkStatus result;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        result.tracker = tracker_status_;
        result.trackers = tracker_details_;
        result.listener = listener_status_;
        result.incoming_handshakes = incoming_handshakes_;
        result.loopback_handshakes = loopback_handshakes_;
        result.outgoing_handshakes = outgoing_handshakes_;
        result.rejected_incoming = rejected_incoming_;
        result.failed_incoming_handshakes = failed_incoming_handshakes_;
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
    result.max_peers = max_peers_;
    if (port_mapper_) result.port_mapping = port_mapper_->status();
    else result.port_mapping.enabled = port_mapping_enabled_;
    result.download_limit = download_limiter_.getRate();
    result.upload_limit = upload_limiter_.getRate();
    if (global_limits_) {
        result.global_download_limit = global_limits_->download.getRate();
        result.global_upload_limit = global_limits_->upload.getRate();
    }
    result.pex_enabled = enable_pex_;
    result.lsd_enabled = enable_lsd_;
    result.web_seeds = enable_webseeds_ ? torrent_.webSeeds().size() : 0;
    result.dht_enabled = enable_dht_;
    if (dht_manager_) result.dht_nodes = dht_manager_->getNodeCount();
    return result;
}

} // namespace torrent
