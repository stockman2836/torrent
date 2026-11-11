#include "torrent/session/download_manager.h"
#include <iostream>

namespace torrent {

void DownloadManager::trackerLoop() {
    while (running_) {
        updateTracker();
        waitForStop(std::chrono::seconds(30));
    }
}

TrackerResponse DownloadManager::announceTracker(const std::string& event) {
    // Completion and periodic announces can originate on different workers.
    std::lock_guard<std::mutex> announce_lock(tracker_mutex_);
    TrackerResponse response;
    if (!running_) return response;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        tracker_status_.state = TrackerState::Announcing;
    }
    try {
        response = tracker_client_->announce(total_uploaded_, total_downloaded_,
            bytesRemaining(), listen_port_, event);
    } catch (const std::exception& error) { response.failure_reason = error.what(); }
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        tracker_status_.state = response.isSuccess() ? TrackerState::Working : TrackerState::Error;
        tracker_status_.error = response.failure_reason;
        if (response.isSuccess()) {
            tracker_status_.peers_returned = response.peers.size();
            tracker_status_.consecutive_failures = 0;
        } else ++tracker_status_.consecutive_failures;
    }
    return response;
}

void DownloadManager::updateTracker() {
    if (!tracker_client_ || !running_) return;
    const std::string event = first_announce_ ? "started" : "";
    first_announce_ = false;
    const auto response = announceTracker(event);
    if (!response.isSuccess()) {
        std::cerr << "Tracker error: " << response.failure_reason << '\n';
        return;
    }
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        available_peers_ = response.peers;
    }
    if (running_) connectToPeers();
}

} // namespace torrent
