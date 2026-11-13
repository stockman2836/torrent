#include "torrent/session/download_manager.h"
#include "peer_state.h"
#include <algorithm>
#include <iostream>
#include <random>

namespace torrent {

void DownloadManager::initializeTrackers(int timeout_seconds) {
    if (timeout_seconds < 1) throw std::invalid_argument("Tracker timeout must be positive");
    tracker_timeout_ = timeout_seconds;
    std::mt19937 random(std::random_device{}());
    size_t group = 0;
    for (const auto& urls : torrent_.trackerTiers()) {
        ++group;
        std::vector<TrackerEntry> tier;
        for (const auto& url : urls) {
            TrackerEntry entry{url};
            entry.status_index = tracker_details_.size();
            TrackerDetails detail; detail.url = url; detail.tier = group;
            tracker_details_.push_back(std::move(detail));
            tier.push_back(std::move(entry));
        }
        std::shuffle(tier.begin(), tier.end(), random);
        tracker_status_.total_trackers += tier.size();
        tracker_tiers_.push_back(std::move(tier));
    }
    if (!tracker_tiers_.empty()) {
        tracker_status_.url = tracker_tiers_.front().front().url;
        tracker_status_.state = TrackerState::Waiting;
    }
}

void DownloadManager::trackerLoop() {
    while (running_) {
        if (seeding_mode_ && !complete_at_start_ && !completion_pending_ && !tracker_tiers_.empty()) {
            announceTracker("completed");
            continue;
        }
        std::chrono::steady_clock::time_point next;
        { std::lock_guard<std::mutex> lock(tracker_mutex_); next = next_tracker_announce_; }
        const auto now = std::chrono::steady_clock::now();
        bool manual = tracker_retry_requested_.exchange(false);
        if (manual) {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            // A regular/completion response may have refreshed the cooldown
            // after this command was accepted. Coalesce with that fresh round.
            if (now < tracker_status_.retry_after) {
                tracker_status_.retry_pending = false;
                manual = false;
            }
        }
        if (manual || now >= next || (last_tracker_port_ && last_tracker_port_ != announcePort())) updateTracker();
        else {
            std::unique_lock<std::mutex> lock(stop_mutex_);
            stop_cv_.wait_for(lock, std::min(std::chrono::milliseconds(1000),
                std::chrono::duration_cast<std::chrono::milliseconds>(next - now) + std::chrono::milliseconds(1)),
                [this] { return !running_ || tracker_retry_requested_.load(); });
        }
    }
}

TrackerResponse DownloadManager::announceTracker(const std::string& event) {
    // Periodic and completion announces share the same per-tracker lifecycle.
    std::lock_guard<std::mutex> announce_lock(tracker_mutex_);
    TrackerResponse response;
    response.failure_reason = "No tracker responded";
    if (!running_ || tracker_tiers_.empty()) return response;
    const auto public_port = announcePort();
    if (event.empty()) last_tracker_port_ = public_port;
    if (event == "completed") completion_pending_ = true;
    size_t attempt = 0;
    const auto query = [&](TrackerEntry& entry, const std::string& wire_event, uint16_t port) {
        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            tracker_status_.url = entry.url;
            tracker_status_.attempt = attempt;
            tracker_status_.state = TrackerState::Announcing;
            tracker_status_.peers_returned = 0;
            tracker_status_.retry_pending = false;
            tracker_status_.next_announce = {};
        }
        const auto started = beginTrackerQuery(entry.status_index);
        TrackerResponse result;
        if (wire_event == "completed") entry.completion_attempted = true;
        try {
            TrackerClient client(entry.url, torrent_.infoHash(), peer_id_);
            result = client.announce(total_uploaded_, total_downloaded_, bytesRemaining(),
                port, wire_event, tracker_timeout_, [this] { return !running_.load(); });
        } catch (const std::exception& error) { result.failure_reason = error.what(); }
        recordTrackerResult(entry.status_index, wire_event, result, started, !running_);
        // Cancellation is not a new tracker failure and must not trigger fallback.
        if (!running_) return result;
        if (result.isSuccess()) {
            if (wire_event == "started") { entry.started = true; entry.announced_port = port; }
            if (wire_event == "completed") entry.completed = true;
        }
        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            tracker_status_.state = result.isSuccess() ? TrackerState::Working : TrackerState::Error;
            tracker_status_.error = result.failure_reason;
            if (result.isSuccess()) {
                tracker_status_.peers_returned = result.peers.size();
                tracker_status_.consecutive_failures = 0;
            } else ++tracker_status_.consecutive_failures;
        }
        return result;
    };

    if (event == "completed") {
        // Notify all trackers that accepted started. Failures stay pending.
        std::vector<TrackerEntry*> registered;
        for (auto& tier : tracker_tiers_) for (auto& entry : tier)
            if (entry.started && !entry.completed) registered.push_back(&entry);
        if (registered.empty()) {
            next_tracker_announce_ = {}; // Completion can precede the first announce via DHT/web seeds.
            return response;
        }
        std::string current;
        { std::lock_guard<std::mutex> lock(peers_mutex_); current = tracker_status_.url; }
        // Leave the current tracker's result visible after notifying older ones.
        std::stable_partition(registered.begin(), registered.end(), [&](const auto* entry) { return entry->url != current; });
        for (auto* entry : registered) {
            if (!running_) return response;
            ++attempt; response = query(*entry, "completed", entry->announced_port);
        }
    } else {
        for (auto& tier : tracker_tiers_) {
            for (size_t index = 0; index < tier.size() && running_; ++index) {
                auto& entry = tier[index];
                ++attempt;
                if (entry.started && entry.announced_port != public_port) {
                    query(entry, "stopped", entry.announced_port);
                    if (!running_) return response;
                    entry.started = entry.completed = entry.completion_attempted = false;
                }
                response = query(entry, entry.started ? "" : "started", public_port);
                if (!running_) return response;
                if (!response.isSuccess()) continue;
                if (completion_pending_ && !entry.completed) response = query(entry, "completed", public_port);
                if (!running_) return response;
                if (!response.isSuccess()) continue;
                // BEP 12: promote within this tier, preserving tier precedence.
                std::rotate(tier.begin(), tier.begin() + index, tier.begin() + index + 1);
                scheduleTrackerUpdate(response);
                return response; // A successful empty peer list is still a success.
            }
        }
    }
    scheduleTrackerUpdate(response);
    return response;
}

void DownloadManager::stopTrackers() {
    // Workers have joined. Do not walk untried/unavailable backups on shutdown.
    std::vector<TrackerEntry*> registered;
    for (auto& tier : tracker_tiers_) for (auto& entry : tier) if (entry.started) registered.push_back(&entry);
    std::string current;
    { std::lock_guard<std::mutex> lock(peers_mutex_); current = tracker_status_.url; }
    std::stable_partition(registered.begin(), registered.end(), [&](const auto* entry) { return entry->url == current; });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (auto* entry : registered) {
        if (std::chrono::steady_clock::now() >= deadline) break;
        try {
            TrackerClient client(entry->url, torrent_.infoHash(), peer_id_);
            const auto notify = [&](const std::string& event) {
                const auto started = beginTrackerQuery(entry->status_index);
                TrackerResponse result;
                try {
                    result = client.announce(total_uploaded_, total_downloaded_, bytesRemaining(), entry->announced_port,
                        event, 2, [deadline] { return std::chrono::steady_clock::now() >= deadline; });
                } catch (const std::exception& error) { result.failure_reason = error.what(); }
                recordTrackerResult(entry->status_index, event, result, started, std::chrono::steady_clock::now() >= deadline);
            };
            // A fast finish can stop the worker before it sends completion.
            // Flush only notifications never attempted, within the same budget.
            if (seeding_mode_ && !complete_at_start_ && !entry->completion_attempted) {
                entry->completion_attempted = true;
                notify("completed");
            }
            if (std::chrono::steady_clock::now() < deadline) notify("stopped");
        } catch (const std::exception&) {} // Best effort; preserve useful diagnostics.
        entry->started = false;
    }
    std::lock_guard<std::mutex> lock(peers_mutex_);
    tracker_retry_requested_ = false;
    tracker_status_.retry_pending = false;
    tracker_status_.next_announce = tracker_status_.retry_after = {};
    if (tracker_status_.state == TrackerState::Announcing) {
        tracker_status_.state = TrackerState::Waiting;
        tracker_status_.error.clear();
    }
}

void DownloadManager::updateTracker() {
    if (tracker_tiers_.empty() || !running_) {
        std::lock_guard<std::mutex> lock(tracker_mutex_);
        next_tracker_announce_ = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        return;
    }
    const auto response = announceTracker("");
    if (!response.isSuccess()) {
        if (running_) std::cerr << "Tracker error: " << response.failure_reason << '\n';
        return;
    }
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (const auto& peer : response.peers) {
            const auto same = [&](const Peer& known) { return known.ip == peer.ip && known.port == peer.port; };
            if (std::any_of(active_peers_.begin(), active_peers_.end(), [&](const auto& active) {
                return !active->finished && same(active->peer_data);
            }) || std::any_of(connecting_peers_.begin(), connecting_peers_.end(), [&](const PeerStatus& known) {
                return known.ip == peer.ip && known.port == peer.port;
            })) continue;
            const auto duplicate = std::find_if(available_peers_.begin(), available_peers_.end(), [&](const Peer& known) {
                return same(known);
            });
            if (duplicate == available_peers_.end()) available_peers_.push_back(peer);
        }
    }
    if (running_) connectToPeers();
}

} // namespace torrent
