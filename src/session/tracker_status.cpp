#include "torrent/session/download_manager.h"
#include <algorithm>

namespace torrent {
namespace {
int64_t timestamp() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
}

std::chrono::steady_clock::time_point DownloadManager::beginTrackerQuery(size_t index) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    auto& detail = tracker_details_.at(index);
    detail.state = TrackerState::Announcing;
    ++detail.requests;
    return std::chrono::steady_clock::now();
}

void DownloadManager::recordTrackerResult(size_t index, const std::string& event, const TrackerResponse& response,
                                        std::chrono::steady_clock::time_point started, bool cancelled) {
    const auto now = std::chrono::steady_clock::now();
    TrackerEvent result;
    result.time_ms = timestamp();
    result.event = event.empty() ? "update" : event;
    result.result = cancelled ? TrackerEventResult::Cancelled :
        response.isSuccess() ? TrackerEventResult::Success : TrackerEventResult::Failure;
    result.peers_returned = response.isSuccess() && !cancelled ? response.peers.size() : 0;
    size_t length = std::min(response.failure_reason.size(), size_t(1024));
    // Do not split a UTF-8 code point when bounding a valid tracker message.
    if (length < response.failure_reason.size())
        while (length && (static_cast<unsigned char>(response.failure_reason[length]) & 0xc0) == 0x80) --length;
    result.error = cancelled ? "" : response.failure_reason.substr(0, length);
    result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count();
    std::lock_guard<std::mutex> lock(peers_mutex_);
    auto& detail = tracker_details_.at(index);
    if (cancelled) detail.state = TrackerState::Waiting;
    else if (response.isSuccess()) {
        detail.state = TrackerState::Working;
        detail.peers_returned = result.peers_returned;
        detail.consecutive_failures = 0;
        detail.last_response_ms = result.time_ms;
        detail.error.clear();
        if (response.min_interval > 0) detail.retry_after = now + std::chrono::seconds(response.min_interval);
    } else {
        detail.state = TrackerState::Error;
        detail.error = result.error;
        ++detail.consecutive_failures;
    }
    if (detail.history.size() == TRACKER_HISTORY_LIMIT) detail.history.erase(detail.history.begin());
    detail.history.push_back(std::move(result));
}

void DownloadManager::scheduleTrackerUpdate(const TrackerResponse& response) {
    const auto now = std::chrono::steady_clock::now();
    next_tracker_announce_ = now + std::chrono::seconds(response.isSuccess() ?
        std::max({response.interval, response.min_interval, 1}) : 30);
    std::lock_guard<std::mutex> lock(peers_mutex_);
    tracker_status_.next_announce = next_tracker_announce_;
    tracker_status_.retry_after = now + std::chrono::seconds(
        response.isSuccess() && response.min_interval > 0 ? response.min_interval : 30);
    // A manual round can revisit earlier tiers; respect every known minimum.
    for (const auto& detail : tracker_details_)
        tracker_status_.retry_after = std::max(tracker_status_.retry_after, detail.retry_after);
    tracker_status_.retry_pending = false;
}

bool DownloadManager::requestTrackerUpdate() {
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        if (!running_ || !tracker_status_.total_trackers || tracker_status_.retry_pending ||
            (tracker_status_.state != TrackerState::Working && tracker_status_.state != TrackerState::Error) ||
            std::chrono::steady_clock::now() < tracker_status_.retry_after) return false;
        tracker_status_.retry_pending = true;
        tracker_retry_requested_ = true;
    }
    // Pair notification with the wait predicate's lock, including at its boundary.
    std::lock_guard<std::mutex> lock(stop_mutex_);
    stop_cv_.notify_all();
    return true;
}

} // namespace torrent
