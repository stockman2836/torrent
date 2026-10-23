#include "torrent/session/magnet_download_manager.h"
#include "torrent/network/peer/peer_connection.h"
#include "torrent/core/logger.h"
#include "torrent/core/utils.h"
#include <algorithm>
#include <chrono>

namespace torrent {

MagnetDownloadManager::MagnetDownloadManager(const MagnetURI& magnet, dht::DHTManager* dht, uint16_t port)
    : magnet_uri_(magnet), dht_manager_(dht), listen_port_(port), peer_id_(utils::generatePeerId()),
      running_(false), metadata_complete_(false) {}

MagnetDownloadManager::~MagnetDownloadManager() { stop(); }

void MagnetDownloadManager::start(MetadataCallback callback) {
    if (running_.exchange(true)) return;
    if (worker_thread_.joinable()) worker_thread_.join();
    on_metadata_ready_ = std::move(callback);
    metadata_complete_ = false;
    progress_ = 0;
    available_peers_.clear();
    working_tracker_.clear();
    worker_thread_ = std::thread(&MagnetDownloadManager::metadataDownloadLoop, this);
}

void MagnetDownloadManager::stop() {
    running_ = false;
    if (worker_thread_.joinable()) worker_thread_.join();
}

bool MagnetDownloadManager::isMetadataComplete() const { return metadata_complete_; }
double MagnetDownloadManager::progress() const { return progress_; }

void MagnetDownloadManager::metadataDownloadLoop() {
    try {
        findPeersViaTrackers();
        if (running_ && available_peers_.empty()) findPeersViaDHT();
        if (running_) connectToPeers();
    } catch (const std::exception& error) {
        LOG_ERROR("Magnet discovery failed: {}", error.what());
    }
    running_ = false;
}

void MagnetDownloadManager::findPeersViaDHT() {
    if (!dht_manager_ || dht_manager_->getNodeCount() == 0) return;
    for (const auto& peer : dht_manager_->getPeers(magnet_uri_.infoHash(), 30)) {
        available_peers_.emplace_back(peer.ip, peer.port);
    }
}

void MagnetDownloadManager::findPeersViaTrackers() {
    for (const auto& url : magnet_uri_.trackers()) {
        if (!running_) return;
        try {
            TrackerClient tracker(url, magnet_uri_.infoHash(), peer_id_);
            const auto response = tracker.announce(0, 0, 0, listen_port_, "started");
            if (!response.isSuccess()) continue;
            if (!response.peers.empty() && working_tracker_.empty()) working_tracker_ = url;
            for (const auto& peer : response.peers) {
                const auto duplicate = std::find_if(available_peers_.begin(), available_peers_.end(),
                    [&](const Peer& known) { return known.ip == peer.ip && known.port == peer.port; });
                if (duplicate == available_peers_.end()) available_peers_.push_back(peer);
            }
        } catch (const std::exception& error) {
            LOG_WARN("Magnet tracker {} failed: {}", url, error.what());
        }
    }
}

void MagnetDownloadManager::connectToPeers() {
    const auto count = std::min<size_t>(5, available_peers_.size());
    for (size_t i = 0; i < count && running_ && !metadata_complete_; ++i) {
        const auto& peer = available_peers_[i];
        try {
            // Destroy the connection before the exchange captured by its handler.
            std::unique_ptr<MetadataExchange> exchange;
            PeerConnection connection(peer.ip, peer.port, magnet_uri_.infoHash(), peer_id_);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            connection.setCancellationCallback([this, deadline] {
                return !running_ || std::chrono::steady_clock::now() >= deadline;
            });
            if (!connection.connect() || !connection.performHandshake()) continue;

            auto& extensions = connection.extensions();
            extensions.registerExtension(EXT_NAME_METADATA,
                [&](uint8_t, const std::vector<uint8_t>& payload) {
                    if (exchange) exchange->handleMessage(payload);
                });
            if (!connection.sendExtendedHandshake()) continue;

            bool handshake = false;
            while (running_) {
                const auto message = connection.receiveMessage();
                if (!message) break;
                if (message->type == MessageType::EXTENDED && !message->payload.empty() &&
                    message->payload[0] == EXT_HANDSHAKE) {
                    handshake = true;
                    break;
                }
            }
            if (!handshake || !extensions.peerSupportsExtension(EXT_NAME_METADATA)) continue;

            exchange = std::make_unique<MetadataExchange>(extensions.getPeerMetadataSize(),
                [this](const std::vector<uint8_t>& bytes) { onMetadataComplete(bytes); });
            for (size_t index = 0; index < exchange->totalPieces() && running_; ++index) {
                exchange->markPieceRequested(index);
                const auto request = extensions.buildExtensionMessage(EXT_NAME_METADATA,
                    exchange->buildRequestMessage(index));
                if (!connection.sendExtendedMessage(extensions.getPeerExtensionId(EXT_NAME_METADATA), request)) break;
            }
            while (running_ && !exchange->isComplete() && !exchange->hasFailed()) {
                if (!connection.receiveMessage()) break;
                progress_ = exchange->progress();
            }
            // A complete exchange can still contain metadata with the wrong hash.
            // Only onMetadataComplete publishes a verified TorrentFile.
            if (!metadata_complete_) progress_ = 0;
        } catch (const std::exception& error) {
            LOG_WARN("Magnet peer {}:{} failed: {}", peer.ip, peer.port, error.what());
        }
    }
}

void MagnetDownloadManager::onMetadataComplete(const std::vector<uint8_t>& bytes) {
    try {
        auto trackers = magnet_uri_.trackers();
        if (!working_tracker_.empty()) {
            trackers.erase(std::remove(trackers.begin(), trackers.end(), working_tracker_), trackers.end());
            trackers.insert(trackers.begin(), working_tracker_);
        }
        const auto torrent = TorrentFile::fromMetadata(magnet_uri_.infoHash(), bytes, trackers);
        if (on_metadata_ready_) on_metadata_ready_(torrent);
        metadata_complete_ = true;
        progress_ = 100;
    } catch (const std::exception& error) {
        LOG_WARN("Magnet metadata verification failed: {}", error.what());
    }
}

} // namespace torrent
