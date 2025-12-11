#include "magnet_download_manager.h"
#include "peer_connection.h"
#include "logger.h"
#include "utils.h"
#include <chrono>
#include <thread>
#include <algorithm>

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
    LOG_INFO("Magnet: Connecting to peers and requesting metadata...");

    connectToPeers();

    // Check if metadata was successfully downloaded
    if (metadata_complete_) {
        LOG_INFO("Magnet: Metadata download complete!");
    } else {
        LOG_ERROR("Magnet: Failed to download metadata");
    }

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
    std::vector<Peer> peers_to_try;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        peers_to_try = available_peers_;
    }

    // Try up to 5 peers
    size_t max_peers = std::min<size_t>(5, peers_to_try.size());

    for (size_t i = 0; i < max_peers && running_ && !metadata_complete_; ++i) {
        const auto& peer = peers_to_try[i];

        LOG_INFO("Magnet: Trying peer {}:{}", peer.ip, peer.port);

        try {
            // Create peer connection
            PeerConnection conn(peer.ip, peer.port, magnet_uri_.infoHash(), peer_id_);

            // Connect
            if (!conn.connect()) {
                LOG_WARN("Magnet: Failed to connect to {}:{}", peer.ip, peer.port);
                continue;
            }

            // Perform handshake
            if (!conn.performHandshake()) {
                LOG_WARN("Magnet: Handshake failed with {}:{}", peer.ip, peer.port);
                conn.disconnect();
                continue;
            }

            LOG_INFO("Magnet: Handshake successful with {}:{}", peer.ip, peer.port);

            // Send extended handshake
            if (!conn.sendExtendedHandshake()) {
                LOG_WARN("Magnet: Failed to send extended handshake to {}:{}", peer.ip, peer.port);
                conn.disconnect();
                continue;
            }

            // Wait for peer's extended handshake
            LOG_INFO("Magnet: Waiting for peer's extended handshake...");
            auto start_time = std::chrono::steady_clock::now();
            bool got_handshake = false;

            while (running_ && !got_handshake) {
                auto msg = conn.receiveMessage();
                if (msg && msg->type == MessageType::EXTENDED) {
                    got_handshake = true;
                    break;
                }

                // Timeout after 10 seconds
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - start_time
                ).count();
                if (elapsed > 10) {
                    LOG_WARN("Magnet: Timeout waiting for extended handshake from {}:{}", peer.ip, peer.port);
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            if (!got_handshake) {
                conn.disconnect();
                continue;
            }

            // Check if peer supports ut_metadata
            auto* ext_proto = conn.extensionProtocol();
            if (!ext_proto || !ext_proto->peerSupportsExtension("ut_metadata")) {
                LOG_WARN("Magnet: Peer {}:{} doesn't support ut_metadata", peer.ip, peer.port);
                conn.disconnect();
                continue;
            }

            int64_t metadata_size = ext_proto->getPeerMetadataSize();
            if (metadata_size <= 0) {
                LOG_WARN("Magnet: Peer {}:{} didn't provide metadata size", peer.ip, peer.port);
                conn.disconnect();
                continue;
            }

            LOG_INFO("Magnet: Peer supports ut_metadata, metadata size: {} bytes", metadata_size);

            // Initialize metadata exchange
            metadata_exchange_ = std::make_unique<MetadataExchange>(
                metadata_size,
                [this](const std::vector<uint8_t>& metadata) {
                    this->onMetadataComplete(metadata);
                }
            );

            // Request metadata pieces
            while (running_ && !metadata_complete_) {
                int piece_to_request = metadata_exchange_->getNextPieceToRequest();

                if (piece_to_request < 0) {
                    // All pieces requested, wait for completion
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));

                    // Check for incoming messages
                    auto msg = conn.receiveMessage();
                    if (msg && msg->type == MessageType::EXTENDED) {
                        // Message already handled in receiveMessage
                    }

                    continue;
                }

                // Build and send request
                LOG_DEBUG("Magnet: Requesting metadata piece {}", piece_to_request);
                auto request_dict = metadata_exchange_->buildRequestMessage(piece_to_request);
                auto request_payload = ext_proto->buildExtensionMessage("ut_metadata", request_dict);

                if (!conn.sendExtendedMessage(ext_proto->getPeerExtensionId("ut_metadata"), request_payload)) {
                    LOG_ERROR("Magnet: Failed to send metadata request");
                    break;
                }

                metadata_exchange_->markPieceRequested(piece_to_request);

                // Wait for response
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }

            conn.disconnect();

            if (metadata_complete_) {
                LOG_INFO("Magnet: Successfully downloaded metadata from {}:{}", peer.ip, peer.port);
                break;
            }

        } catch (const std::exception& e) {
            LOG_ERROR("Magnet: Error with peer {}:{}: {}", peer.ip, peer.port, e.what());
        }
    }

    if (!metadata_complete_) {
        LOG_ERROR("Magnet: Failed to download metadata from any peer");
    }
}

void MagnetDownloadManager::onMetadataComplete(const std::vector<uint8_t>& metadata) {
    LOG_INFO("Magnet: Metadata received ({} bytes)", metadata.size());

    try {
        // Create TorrentFile from metadata
        TorrentFile torrent_file = TorrentFile::fromMetadata(
            magnet_uri_.infoHash(),
            metadata,
            magnet_uri_.trackers()
        );

        LOG_INFO("Magnet: Successfully created TorrentFile from metadata");
        LOG_INFO("Magnet: Torrent name: {}", torrent_file.name());
        LOG_INFO("Magnet: Total size: {}", torrent_file.totalLength());
        LOG_INFO("Magnet: Number of pieces: {}", torrent_file.numPieces());

        metadata_complete_ = true;

        // Call callback with TorrentFile
        if (on_metadata_ready_) {
            on_metadata_ready_(torrent_file);
        }

    } catch (const std::exception& e) {
        LOG_ERROR("Magnet: Failed to create TorrentFile from metadata: {}", e.what());
    }
}

} // namespace torrent
