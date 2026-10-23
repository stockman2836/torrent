#include "torrent/session/download_manager.h"
#include "torrent/core/logger.h"
#include "torrent/core/utils.h"
#include "torrent/network/discovery/pex_manager.h"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>


namespace torrent {

void DownloadManager::trackerLoop() {
    while (running_) {
        waitForStop(std::chrono::seconds(30));

        if (!running_) {
            break;
        }

        updateTracker();
    }
}

void DownloadManager::updateTracker() {
    int64_t left = torrent_.totalLength() - total_downloaded_;

    // Determine event type
    std::string event = "";
    if (first_announce_) {
        event = "started";
        first_announce_ = false;
    }
    // Note: "completed" event is sent directly from downloadLoop when transitioning to seeding

    try {
        TrackerResponse response = tracker_client_->announce(
            total_uploaded_,
            total_downloaded_,
            left,
            listen_port_,
            event
        );

        if (!response.isSuccess()) {
            tracker_failures_++;
            std::cerr << "Tracker error (" << tracker_failures_ << " failures): "
                      << response.failure_reason << "\n";

            // Exponential backoff for retries
            if (tracker_failures_ <= 10) {
                int backoff_ms = utils::calculateBackoffDelay(tracker_failures_ - 1, 1000, 60000);
                std::cout << "Will retry tracker in " << (backoff_ms / 1000) << " seconds\n";
                // Note: retry happens in next trackerLoop iteration
            } else {
                std::cerr << "Too many tracker failures, continuing with existing peers\n";
            }
            return;
        }

        // Success - reset failure counter
        tracker_failures_ = 0;

        std::cout << "Tracker response: " << response.peers.size() << " peers, "
                  << response.complete << " seeders, "
                  << response.incomplete << " leechers\n";

        available_peers_ = response.peers;
        connectToPeers();

    } catch (const std::exception& e) {
        tracker_failures_++;
        std::cerr << "Tracker exception (" << tracker_failures_ << " failures): "
                  << e.what() << "\n";

        if (tracker_failures_ <= 10) {
            int backoff_ms = utils::calculateBackoffDelay(tracker_failures_ - 1, 1000, 60000);
            std::cout << "Will retry tracker in " << (backoff_ms / 1000) << " seconds\n";
        }
    }
}

} // namespace torrent
