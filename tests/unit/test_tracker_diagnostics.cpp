#include "tracker_diagnostics.h"
#include "download_task.h"
#include "network_diagnostics.h"
#include <gtest/gtest.h>

using namespace torrent;
using namespace torrent::app;

TEST(TrackerDiagnostics, UpdatesRequireAnActiveSessionWithACompletedRoundAndNoPendingRequest) {
    DownloadSnapshot snapshot;
    snapshot.state = DownloadState::Downloading;
    snapshot.network.tracker.total_trackers = 2;
    snapshot.network.tracker.state = TrackerState::Working;
    EXPECT_TRUE(canUpdateTrackers(snapshot));
    snapshot.network.tracker.retry_pending = true;
    EXPECT_FALSE(canUpdateTrackers(snapshot));
    snapshot.network.tracker.retry_pending = false;
    snapshot.network.tracker.retry_after = std::chrono::steady_clock::now() + std::chrono::minutes(1);
    EXPECT_FALSE(canUpdateTrackers(snapshot));
    snapshot.network.tracker.retry_after = {};
    for (const auto state : {TrackerState::None, TrackerState::Waiting, TrackerState::Announcing}) {
        snapshot.network.tracker.state = state; EXPECT_FALSE(canUpdateTrackers(snapshot));
    }
    snapshot.network.tracker.state = TrackerState::Error;
    EXPECT_TRUE(canUpdateTrackers(snapshot));
    for (const auto state : {DownloadState::Ready, DownloadState::Loading, DownloadState::Stopping, DownloadState::Stopped,
                             DownloadState::Complete, DownloadState::Failed}) {
        snapshot.state = state; EXPECT_FALSE(canUpdateTrackers(snapshot));
    }
    snapshot.state = DownloadState::Seeding; EXPECT_TRUE(canUpdateTrackers(snapshot));
    snapshot.network.tracker.total_trackers = 0; EXPECT_FALSE(canUpdateTrackers(snapshot));
}

TEST(TrackerDiagnostics, DisplaysMissingTimesAndRoundsUpRemainingSeconds) {
    EXPECT_EQ(trackerWaitSeconds({}), 0);
    EXPECT_EQ(trackerWaitSeconds(std::chrono::steady_clock::now() + std::chrono::milliseconds(500)), 1);
    EXPECT_EQ(trackerTime(0), u8"—");
    EXPECT_NE(trackerTime(1700000000000), u8"—");
    TrackerEvent event; event.event = "update"; event.result = TrackerEventResult::Cancelled;
    EXPECT_NE(trackerEventText(event).find(u8"отменено"), std::string::npos);
}

TEST(TrackerDiagnostics, HistoryReportRedactsEveryTrackerUrl) {
    DownloadSnapshot snapshot;
    TrackerDetails detail;
    detail.url = "https://user:secret@tracker.invalid/private-token/announce?passkey=query-token";
    detail.tier = 2; detail.requests = 1; detail.state = TrackerState::Error;
    TrackerEvent event; event.time_ms = 1700000000000; event.event = "started";
    event.result = TrackerEventResult::Failure; event.error = "fixture error";
    detail.history.push_back(event); snapshot.network.trackers.push_back(detail);
    const auto report = networkReport(snapshot);
    EXPECT_NE(report.find("https://tracker.invalid"), std::string::npos);
    EXPECT_NE(report.find("fixture error"), std::string::npos);
    for (const auto* secret : {"user", "secret", "private-token", "query-token"}) EXPECT_EQ(report.find(secret), std::string::npos);
}
