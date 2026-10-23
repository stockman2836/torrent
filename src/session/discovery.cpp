#include "torrent/session/download_manager.h"
#include "peer_state.h"
#include "torrent/core/logger.h"
#include "torrent/core/utils.h"
#include "torrent/network/discovery/pex_manager.h"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>


namespace torrent {

void DownloadManager::dhtLoop() {
    if (!enable_dht_ || !dht_manager_) {
        return;
    }

    std::cout << "DHT loop started\n";

    // Wait for DHT to bootstrap
    waitForStop(std::chrono::seconds(5));

    // Announce ourselves in DHT
    if (dht_manager_->getNodeCount() > 0) {
        std::cout << "DHT: Announcing ourselves for info_hash\n";
        dht_manager_->announcePeer(torrent_.infoHash(), listen_port_);
    }

    while (running_) {
        waitForStop(std::chrono::seconds(60));

        if (!running_) {
            break;
        }

        // Get peers from DHT if we don't have enough
        size_t active_peer_count = 0;
        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            active_peer_count = active_peers_.size();
        }

        if (active_peer_count < min_peers_ && !seeding_mode_) {
            updateDHT();
        }

        // Periodically re-announce
        if (dht_manager_->getNodeCount() > 0) {
            dht_manager_->announcePeer(torrent_.infoHash(), listen_port_);
        }

        // Log DHT statistics
        auto stats = dht_manager_->getRoutingStats();
        std::cout << "DHT: " << stats.total_nodes << " nodes in routing table ("
                  << stats.good_nodes << " good)\n";
    }

    std::cout << "DHT loop ended\n";
}

void DownloadManager::updateDHT() {
    if (!enable_dht_ || !dht_manager_) {
        return;
    }

    if (dht_manager_->getNodeCount() == 0) {
        std::cout << "DHT: No nodes in routing table yet, skipping peer lookup\n";
        return;
    }

    std::cout << "DHT: Looking up peers for info_hash...\n";

    auto dht_peers = dht_manager_->getPeers(torrent_.infoHash(), 15);

    if (dht_peers.empty()) {
        std::cout << "DHT: No peers found\n";
        return;
    }

    std::cout << "DHT: Found " << dht_peers.size() << " peers\n";

    // Add DHT peers to available peers
    std::lock_guard<std::mutex> lock(peers_mutex_);
    for (const auto& dht_peer : dht_peers) {
        // Check if we already have this peer
        bool already_have = false;
        for (const auto& peer : available_peers_) {
            if (peer.ip == dht_peer.ip && peer.port == dht_peer.port) {
                already_have = true;
                break;
            }
        }

        if (!already_have) {
            available_peers_.emplace_back(dht_peer.ip, dht_peer.port);
            std::cout << "DHT: Added peer " << dht_peer.ip << ":" << dht_peer.port << "\n";
        }
    }
}

void DownloadManager::pexLoop() {
    if (!enable_pex_) {
        return;
    }

    std::cout << "PEX loop started\n";

    // Wait a bit before starting PEX
    waitForStop(std::chrono::seconds(10));

    while (running_) {
        waitForStop(std::chrono::seconds(60));

        if (!running_) {
            break;
        }

        // Send PEX messages and collect new peers
        updatePEX();
    }

    std::cout << "PEX loop ended\n";
}

void DownloadManager::updatePEX() {
    if (!enable_pex_) {
        return;
    }

    std::lock_guard<std::mutex> lock(peers_mutex_);

    // Collect all peers to share via PEX
    std::vector<Peer> all_peers;

    // Add active peers
    for (const auto& peer_info : active_peers_) {
        all_peers.push_back(peer_info->peer_data);
    }

    // Add available peers
    for (const auto& peer : available_peers_) {
        all_peers.push_back(peer);
    }

    int new_peers_count = 0;

    // Process each active peer
    for (auto& peer_info : active_peers_) {
        if (!peer_info->connection || !peer_info->connection->isPexEnabled()) {
            continue;
        }

        auto* pex_manager = peer_info->connection->pexManager();
        if (!pex_manager) {
            continue;
        }

        // Add all known peers to this peer's PEX manager
        for (const auto& peer : all_peers) {
            // Don't share the peer with itself
            if (peer.ip == peer_info->peer_data.ip && peer.port == peer_info->peer_data.port) {
                continue;
            }

            // Determine flags
            uint8_t flags = 0;
            if (seeding_mode_) {
                flags |= PEX_SEED;
            }

            pex_manager->addPeer(peer.ip, peer.port, flags);
        }

        // Send PEX message if needed
        if (pex_manager->shouldSendUpdate()) {
            if (peer_info->connection->sendPexMessage()) {
                std::cout << "PEX: Sent update to " << peer_info->peer_data.ip
                          << ":" << peer_info->peer_data.port << "\n";
            }
        }

        // Collect new peers discovered via PEX
        const auto& known_peers = pex_manager->getKnownPeers();
        for (const auto& pex_peer : known_peers) {
            // Check if we already have this peer
            bool already_have = false;
            for (const auto& peer : available_peers_) {
                if (peer.ip == pex_peer.ip && peer.port == pex_peer.port) {
                    already_have = true;
                    break;
                }
            }
            for (const auto& peer_info2 : active_peers_) {
                if (peer_info2->peer_data.ip == pex_peer.ip &&
                    peer_info2->peer_data.port == pex_peer.port) {
                    already_have = true;
                    break;
                }
            }

            if (!already_have) {
                available_peers_.emplace_back(pex_peer.ip, pex_peer.port);
                std::cout << "PEX: Discovered new peer " << pex_peer.ip
                          << ":" << pex_peer.port << "\n";
                new_peers_count++;
            }
        }
    }

    if (new_peers_count > 0) {
        std::cout << "PEX: Discovered " << new_peers_count << " new peers total\n";
    }
}

void DownloadManager::lsdLoop() {
    if (!enable_lsd_ || !lsd_manager_) {
        return;
    }

    std::cout << "LSD loop started\n";

    // LSD announcements are handled automatically by the LSD class
    // This loop just monitors discovered peers
    while (running_) {
        waitForStop(std::chrono::seconds(30));

        if (!running_) {
            break;
        }

        // Log LSD statistics
        size_t announced = lsd_manager_->getAnnouncedTorrentsCount();
        size_t discovered = lsd_manager_->getDiscoveredPeersCount();

        if (announced > 0 || discovered > 0) {
            std::cout << "LSD: Announcing " << announced << " torrents, "
                      << "discovered " << discovered << " peers total\n";
        }
    }

    std::cout << "LSD loop ended\n";
}

void DownloadManager::webseedLoop() {
    if (!enable_webseeds_ || !webseed_manager_) {
        return;
    }

    std::cout << "Web Seed loop started\n";

    // Web seed downloads are handled automatically via callbacks
    // This loop monitors and requests pieces from web seeds
    while (running_) {
        waitForStop(std::chrono::seconds(10));

        if (!running_ || seeding_mode_) {
            break;
        }

        // Request pieces from web seeds
        updateWebSeeds();

        // Log statistics every 30 seconds
        static int counter = 0;
        if (++counter >= 3) {
            counter = 0;

            size_t active_webseeds = webseed_manager_->getActiveWebSeeds();
            int64_t bytes_downloaded = webseed_manager_->getTotalBytesDownloaded();

            if (active_webseeds > 0 || bytes_downloaded > 0) {
                std::cout << "WebSeed: " << active_webseeds << " active seeds, "
                          << utils::formatBytes(bytes_downloaded) << " downloaded\n";
            }
        }
    }

    std::cout << "Web Seed loop ended\n";
}

void DownloadManager::updateWebSeeds() {
    if (!enable_webseeds_ || !webseed_manager_ || !webseed_manager_->hasWebSeeds()) {
        return;
    }

    // Find pieces that need to be downloaded
    // Prefer sequential gaps for web seeds (BEP 19 optimization)
    std::lock_guard<std::mutex> lock(pieces_mutex_);

    // Get bitfield to see what pieces we need
    auto bitfield = piece_manager_->getBitfield();
    size_t num_pieces = bitfield.size();

    // Look for gaps (consecutive missing pieces) to download from web seeds
    // This optimizes HTTP range requests
    uint32_t gap_start = UINT32_MAX;
    uint32_t gap_length = 0;

    for (size_t i = 0; i < num_pieces; ++i) {
        if (!bitfield[i] && pieces_in_download_.find(i) == pieces_in_download_.end()) {
            // Found a missing piece not being downloaded
            if (gap_start == UINT32_MAX) {
                gap_start = i;
                gap_length = 1;
            } else {
                gap_length++;
            }

            // If gap is large enough (4+ pieces), start downloading from web seed
            if (gap_length >= 4) {
                // Download from gap_start
                if (webseed_manager_->downloadFullPiece(gap_start)) {
                    pieces_in_download_.insert(gap_start);
                    LOG_DEBUG("WebSeed: Requested piece {} from web seed", gap_start);
                }
                gap_start++;
                gap_length--;
            }
        } else {
            // Reset gap
            gap_start = UINT32_MAX;
            gap_length = 0;
        }
    }

    // Also download random missing pieces if no gaps found
    // This helps with the last few pieces
    if (gap_start == UINT32_MAX) {
        for (size_t i = 0; i < num_pieces; ++i) {
            if (!bitfield[i] && pieces_in_download_.find(i) == pieces_in_download_.end()) {
                if (webseed_manager_->downloadFullPiece(i)) {
                    pieces_in_download_.insert(i);
                    LOG_DEBUG("WebSeed: Requested piece {} from web seed", i);
                    break;  // Only request one piece at a time
                }
            }
        }
    }
}

void DownloadManager::utpLoop() {
    if (!enable_utp_ || !utp_manager_) {
        return;
    }

    std::cout << "uTP loop started\n";

    // Process uTP events regularly
    while (running_) {
        // Call tick() to process UDP packets and handle timeouts
        utp_manager_->tick();

        // Sleep for a short time (10ms) to allow other threads to run
        // uTP requires frequent ticking for good performance
        waitForStop(std::chrono::milliseconds(10));

        if (!running_) {
            break;
        }
    }

    std::cout << "uTP loop ended\n";
}

} // namespace torrent
