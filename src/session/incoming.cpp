#include "torrent/session/download_manager.h"
#include "torrent/network/peer/peer_listener.h"
#include "torrent/core/logger.h"
#include "peer_state.h"

namespace torrent {

void DownloadManager::incomingLoop() {
    while (running_) {
        try {
            auto connection = listener_->accept(torrent_.infoHash(), peer_id_);
            if (!connection) continue;
            connection->setCancellationCallback([this] { return !running_.load(); });
            connection->initializePeerBitfield(torrent_.numPieces());
            Peer remote(connection->remoteAddress(), connection->remotePort());
            cleanupStalePeers();
            std::lock_guard<std::mutex> lock(peers_mutex_);
            if (!running_ || active_peers_.size() >= max_peers_) continue;
            auto peer = std::make_shared<ActivePeerInfo>(std::move(connection), remote);
            peer->handshaking = true;
            peer->incoming = true;
            active_peers_.push_back(peer);
            peer_threads_.push_back({peer, std::thread([this, peer] {
                try {
                    if (peer->connection->acceptHandshake(piece_manager_->getBitfield())) {
                        if (enable_pex_) {
                            peer->connection->enablePex();
                            peer->connection->sendExtendedHandshake();
                        }
                        {
                            std::lock_guard<std::mutex> lock(peers_mutex_);
                            peer->handshaking = false;
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
                    last_peer_error_ = peer->peer_data.ip + ":" + std::to_string(peer->peer_data.port) +
                        u8" — Не удалось принять приветствие BitTorrent";
                }
            })});
        } catch (const std::exception& error) { LOG_WARN("Accepting peer failed: {}", error.what()); }
    }
}

} // namespace torrent
