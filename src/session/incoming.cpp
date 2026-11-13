#include "torrent/session/download_manager.h"
#include "torrent/network/peer/peer_listener.h"
#include "torrent/core/logger.h"
#include "peer_state.h"

namespace torrent {

void DownloadManager::incomingLoop() {
    while (running_) {
        try {
            auto connection = listener_->accept(torrent_.infoHash(), peer_id_);
            {
                const auto status = listener_->status();
                std::lock_guard<std::mutex> lock(peers_mutex_);
                listener_status_ = status;
            }
            if (!connection) continue;
            connection->setCancellationCallback([this] { return !running_.load(); });
            connection->initializePeerBitfield(torrent_.numPieces());
            Peer remote(connection->remoteAddress(), connection->remotePort());
            cleanupStalePeers();
            std::lock_guard<std::mutex> lock(peers_mutex_);
            if (!running_) continue;
            if (active_peers_.size() >= max_peers_) { ++rejected_incoming_; continue; }
            auto peer = std::make_shared<ActivePeerInfo>(std::move(connection), remote);
            peer->handshaking = true;
            peer->incoming = true;
            active_peers_.push_back(peer);
            peer_threads_.push_back({peer, std::thread([this, peer] {
                try {
                    if (peer->connection->acceptHandshake(piece_manager_->getBitfield())) {
                        configurePeerExtensions(*peer->connection);
                        {
                            std::lock_guard<std::mutex> lock(peers_mutex_);
                            peer->handshaking = false;
                            ++incoming_handshakes_;
                            if (isLoopbackAddress(peer->peer_data.ip)) ++loopback_handshakes_;
                        }
                        peerLoop(peer);
                        return;
                    }
                } catch (const std::exception& error) { LOG_WARN("Incoming peer failed: {}", error.what()); }
                releasePeerPiece(*peer);
                peer->connection->disconnect();
                std::lock_guard<std::mutex> lock(peers_mutex_);
                peer->finished = true;
                if (running_) {
                    ++failed_connections_;
                    ++failed_incoming_handshakes_;
                    last_peer_error_ = peer->peer_data.ip + ":" + std::to_string(peer->peer_data.port) +
                        u8" — Не удалось принять приветствие BitTorrent";
                }
            })});
        } catch (const std::exception& error) { LOG_WARN("Accepting peer failed: {}", error.what()); }
    }
}

} // namespace torrent
