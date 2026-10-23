#pragma once

#include "torrent/metainfo/magnet_uri.h"
#include "torrent/network/peer/metadata_exchange.h"
#include "torrent/network/peer/extension_protocol.h"
#include "torrent/network/dht/dht_manager.h"
#include "torrent/network/tracker/tracker_client.h"
#include "torrent/metainfo/torrent_file.h"
#include <memory>
#include <vector>
#include <string>
#include <atomic>
#include <thread>
#include <functional>

namespace torrent {

// Manager for downloading metadata from magnet links
class MagnetDownloadManager {
public:
    using MetadataCallback = std::function<void(const TorrentFile&)>;

    MagnetDownloadManager(const MagnetURI& magnet_uri,
                         dht::DHTManager* dht_manager,
                         uint16_t listen_port);

    ~MagnetDownloadManager();

    // Start metadata download
    void start(MetadataCallback on_metadata_ready);

    // Stop metadata download
    void stop();

    // Check if metadata download is running
    bool isRunning() const { return running_; }

    // Check if metadata is complete
    bool isMetadataComplete() const;

    // Get download progress (0-100%)
    double progress() const;

    // Get info hash
    const std::vector<uint8_t>& infoHash() const { return magnet_uri_.infoHash(); }

private:
    void metadataDownloadLoop();
    void findPeersViaDHT();
    void findPeersViaTrackers();
    void connectToPeers();

    void onMetadataComplete(const std::vector<uint8_t>& metadata);

    MagnetURI magnet_uri_;
    dht::DHTManager* dht_manager_;
    uint16_t listen_port_;
    std::string peer_id_;

    std::vector<Peer> available_peers_;
    std::string working_tracker_;
    std::atomic<double> progress_{0.0};

    std::atomic<bool> running_;
    std::atomic<bool> metadata_complete_;

    std::thread worker_thread_;

    MetadataCallback on_metadata_ready_;
};

} // namespace torrent
