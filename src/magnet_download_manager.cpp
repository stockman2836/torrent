#include "magnet_download_manager.h"
#include "logger.h"
#include "utils.h"
#include <chrono>
#include <thread>

namespace torrent {

MagnetDownloadManager::MagnetDownloadManager(const MagnetURI& magnet_uri,
                                             dht::DHTManager* dht_manager,
                                             uint16_t listen_port)
    : magnet_uri_(magnet_uri)
    , dht_manager_(dht_manager)
    , listen_port_(listen_port)
    , peer_id_(utils::generatePeerId())
    , running_(false)
    , metadata_complete_(false) {

    // Initialize tracker client if magnet has trackers
    if (!magnet_uri_.trackers().empty()) {
        tracker_client_ = std::make_unique<TrackerClient>(
            magnet_uri_.trackers()[0],
            magnet_uri_.infoHash(),
            peer_id_
        );
    }
}

MagnetDownloadManager::~MagnetDownloadManager() {
    stop();
}

void MagnetDownloadManager::start(MetadataCallback on_metadata_ready) {
    if (running_) {
        return;
    }

    on_metadata_ready_ = on_metadata_ready;
    running_ = true;

    LOG_INFO("Starting magnet download for: {}",
            magnet_uri_.displayName().empty() ? magnet_uri_.infoHashHex() : magnet_uri_.displayName());

    worker_thread_ = std::thread(&MagnetDownloadManager::metadataDownloadLoop, this);
}

void MagnetDownloadManager::stop() {
    if (!running_) {
        return;
    }

    running_ = false;

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

bool MagnetDownloadManager::isMetadataComplete() const {
    return metadata_complete_;
}

double MagnetDownloadManager::progress() const {
    if (!metadata_exchange_) {
        return 0.0;
    }
    return metadata_exchange_->progress();
}

void MagnetDownloadManager::metadataDownloadLoop() {
    LOG_INFO("Magnet: Starting metadata download");

    // Step 1: Find peers via DHT and trackers
    LOG_INFO("Magnet: Looking for peers...");

    findPeersViaDHT();
    findPeersViaTrackers();

    // Wait a bit for peers
    std::this_thread::sleep_for(std::chrono::seconds(5));

    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        LOG_INFO("Magnet: Found {} potential peers", available_peers_.size());

        if (available_peers_.empty()) {
            LOG_ERROR("Magnet: No peers found, cannot download metadata");
            running_ = false;
            return;
        }
    }

    // Step 2: Connect to peers and request metadata
    // Note: Full implementation would need to integrate with PeerConnection
    // and handle extension protocol handshakes

    LOG_INFO("Magnet: Metadata download would proceed here");
    LOG_INFO("Magnet: (Full integration with PeerConnection needed)");

    // For now, this is a placeholder
    // In a full implementation:
    // 1. Connect to peers using PeerConnection
    // 2. Send extended handshake with ut_metadata support
    // 3. Request metadata pieces
    // 4. Assemble metadata
    // 5. Create TorrentFile
    // 6. Call on_metadata_ready_ callback

    running_ = false;
}

void MagnetDownloadManager::findPeersViaDHT() {
    if (!dht_manager_ || dht_manager_->getNodeCount() == 0) {
        LOG_WARN("Magnet: DHT not available or not ready");
        return;
    }

    LOG_INFO("Magnet: Searching DHT for peers...");

    auto dht_peers = dht_manager_->getPeers(magnet_uri_.infoHash(), 30);

    std::lock_guard<std::mutex> lock(peers_mutex_);
    for (const auto& dht_peer : dht_peers) {
        available_peers_.emplace_back(dht_peer.ip, dht_peer.port);
        LOG_DEBUG("Magnet: Found peer via DHT: {}:{}", dht_peer.ip, dht_peer.port);
    }

    LOG_INFO("Magnet: DHT returned {} peers", dht_peers.size());
}

void MagnetDownloadManager::findPeersViaTrackers() {
    if (!tracker_client_) {
        return;
    }

    LOG_INFO("Magnet: Contacting tracker...");

    try {
        auto response = tracker_client_->announce(
            0, // uploaded
            0, // downloaded
            0, // left (unknown for magnet)
            listen_port_,
            "started"
        );

        if (response.isSuccess()) {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            for (const auto& peer : response.peers) {
                available_peers_.push_back(peer);
                LOG_DEBUG("Magnet: Found peer via tracker: {}:{}", peer.ip, peer.port);
            }

            LOG_INFO("Magnet: Tracker returned {} peers", response.peers.size());
        } else {
            LOG_WARN("Magnet: Tracker request failed: {}", response.failure_reason);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Magnet: Tracker error: {}", e.what());
    }
}

void MagnetDownloadManager::connectToPeers() {
    // Placeholder for peer connection logic
    // Full implementation would:
    // 1. Create PeerConnection instances
    // 2. Add extension protocol support
    // 3. Handshake with peers
    // 4. Request metadata via ut_metadata
}

void MagnetDownloadManager::onMetadataComplete(const std::vector<uint8_t>& metadata) {
    LOG_INFO("Magnet: Metadata received ({} bytes)", metadata.size());

    try {
        // Parse metadata as bencode (it's a torrent info dict)
        bencode::BencodeValue value = bencode::decode(metadata);

        // Create TorrentFile from metadata
        // This would need a new constructor in TorrentFile class
        // For now, we'll log success

        LOG_INFO("Magnet: Metadata parsed successfully");
        metadata_complete_ = true;

        // Call callback with TorrentFile
        // if (on_metadata_ready_) {
        //     on_metadata_ready_(torrent_file);
        // }

    } catch (const std::exception& e) {
        LOG_ERROR("Magnet: Failed to parse metadata: {}", e.what());
    }
}

} // namespace torrent
