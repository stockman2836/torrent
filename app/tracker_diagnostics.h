#pragma once

#include "torrent/session/network_status.h"

namespace torrent::app {
struct DownloadSnapshot;

bool canUpdateTrackers(const DownloadSnapshot& snapshot);
std::string trackerStateText(TrackerState state);
std::string trackerEventText(const TrackerEvent& event);
int64_t trackerWaitSeconds(std::chrono::steady_clock::time_point deadline);

} // namespace torrent::app
