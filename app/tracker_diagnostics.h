#pragma once

#include "torrent/session/network_status.h"

namespace torrent::app {
struct DownloadSnapshot;

bool canUpdateTrackers(const DownloadSnapshot& snapshot);
std::string trackerStateText(TrackerState state);
std::string trackerEventText(const TrackerEvent& event);
std::string trackerTime(int64_t unix_milliseconds);
int64_t trackerWaitSeconds(std::chrono::steady_clock::time_point deadline);

} // namespace torrent::app
