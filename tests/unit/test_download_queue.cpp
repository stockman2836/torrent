#include "download_queue.h"
#include "torrent/metainfo/bencode.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>

using namespace torrent;
using namespace torrent::app;
namespace fs = std::filesystem;

class DownloadQueueTest : public ::testing::Test {
protected:
    fs::path directory;
    Config config;
    void SetUp() override {
        directory = fs::temp_directory_path() / ("torrent-queue-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(directory);
        config.download_dir = (directory / fs::u8path(u8"загрузки")).u8string();
        config.enable_dht = config.enable_lsd = config.enable_utp = config.enable_webseeds = false;
    }
    void TearDown() override { fs::remove_all(directory); }
    std::string torrent(const std::string& name) {
        BencodeValue::Dictionary info;
        info["name"] = BencodeValue(name);
        info["length"] = BencodeValue(int64_t(0));
        info["piece length"] = BencodeValue(int64_t(16384));
        info["pieces"] = BencodeValue(std::string());
        BencodeValue::Dictionary root;
        root["info"] = BencodeValue(info);
        auto path = directory / fs::u8path(name + ".torrent");
        std::ofstream out(path, std::ios::binary);
        out << BencodeParser::encode(BencodeValue(root)); out.close();
        return path.u8string();
    }
    bool until(DownloadQueue& queue, const std::function<bool()>& condition) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            queue.tick();
            if (condition()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }
    bool ready(DownloadQueue& queue) {
        return until(queue, [&] {
            for (const auto& entry : queue.snapshots()) if (entry.busy) return false;
            return true;
        });
    }
};

TEST_F(DownloadQueueTest, EntriesHaveIndependentSettingsAndDuplicateSourcesAreSelected) {
    DownloadQueue queue;
    auto path = torrent("first");
    const auto first = queue.add(path, config);
    const auto second = queue.add(torrent("second"), config);
    EXPECT_EQ(queue.add(path, config), first);
    ASSERT_TRUE(ready(queue));
    ASSERT_TRUE(queue.configure(first, config.download_dir, {{0, FilePriority::Low}}));
    auto entries = queue.snapshots();
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_NE(first, second);
    EXPECT_EQ(entries[0].config.file_priorities.at(0), FilePriority::Low);
    EXPECT_TRUE(entries[1].config.file_priorities.empty());
    EXPECT_NE(entries[0].config.listen_port, entries[1].config.listen_port);
    EXPECT_EQ(entries[1].config.dht_port, entries[1].config.listen_port + 1);
}

TEST_F(DownloadQueueTest, NetworkDefaultsApplyOnNextStartAndToNewEntries) {
    DownloadQueue queue;
    const auto first = queue.add(torrent("first"), config);
    queue.add(torrent("second"), config);
    ASSERT_TRUE(ready(queue));
    const auto original = queue.snapshots();
    const NetworkSettings settings{26001, 12, false, false};
    ASSERT_TRUE(queue.setNetworkSettings(settings));
    EXPECT_EQ(queue.snapshots()[0].config.listen_port, original[0].config.listen_port);
    EXPECT_EQ(queue.snapshots()[0].config.max_peers, original[0].config.max_peers);
    ASSERT_TRUE(queue.enqueue(first));
    ASSERT_TRUE(until(queue, [&] { return queue.snapshots()[0].download.state == DownloadState::Complete; }));
    EXPECT_EQ(queue.snapshots()[0].config.listen_port, 26001);
    EXPECT_EQ(queue.snapshots()[0].config.dht_port, 26002);
    EXPECT_EQ(queue.snapshots()[0].config.max_peers, 12);
    EXPECT_FALSE(queue.snapshots()[0].config.enable_pex);
    EXPECT_EQ(queue.snapshots()[1].config.listen_port, original[1].config.listen_port);
    queue.add(torrent("third"), config);
    ASSERT_TRUE(ready(queue));
    EXPECT_EQ(queue.snapshots()[2].config.listen_port, 26003);
    EXPECT_EQ(queue.snapshots()[2].config.max_peers, 12);
}

TEST_F(DownloadQueueTest, InvalidNetworkSettingsLeaveTheQueueUnchanged) {
    DownloadQueue queue;
    queue.add(torrent("first"), config);
    queue.add(torrent("second"), config);
    ASSERT_TRUE(queue.setNetworkSettings({25001, 8, false, false}));
    EXPECT_THROW(queue.setNetworkSettings({65534, 8, false, false}), std::invalid_argument);
    EXPECT_THROW(queue.setNetworkSettings({25001, 0, false, false}), std::invalid_argument);
    queue.add(torrent("third"), config);
    EXPECT_EQ(queue.snapshots()[2].config.listen_port, 25001);
    EXPECT_EQ(queue.snapshots()[2].config.max_peers, 8);
    queue.close();
    EXPECT_FALSE(queue.setNetworkSettings({24001, 8, false, false}));
}

TEST_F(DownloadQueueTest, RestoreRejectsAnInsufficientPortRangeBeforeLoadingEntries) {
    DownloadQueue original;
    original.add(torrent("first"), config);
    original.add(torrent("second"), config);
    const auto path = (directory / "list.json").u8string();
    original.save(path);
    DownloadQueue restored;
    ASSERT_TRUE(restored.setNetworkSettings({65534, 8, false, false}));
    EXPECT_THROW(restored.restore(path, config), std::invalid_argument);
    EXPECT_TRUE(restored.snapshots().empty());
}

TEST_F(DownloadQueueTest, CancellingAWaitingEntryDoesNotStartIt) {
    DownloadQueue queue;
    const auto first = queue.add(torrent("first"), config);
    const auto second = queue.add(torrent("second"), config);
    ASSERT_TRUE(ready(queue));
    EXPECT_TRUE(queue.enqueue(first));
    EXPECT_FALSE(queue.enqueue(first));
    EXPECT_TRUE(queue.enqueue(second));
    EXPECT_FALSE(queue.configure(second, config.download_dir, {}));
    queue.stop(second);
    ASSERT_TRUE(until(queue, [&] { return queue.snapshots()[0].download.state == DownloadState::Complete; }));
    EXPECT_EQ(queue.snapshots()[1].download.state, DownloadState::Ready);
    EXPECT_FALSE(fs::exists(fs::u8path(config.download_dir) / "second"));
}

TEST_F(DownloadQueueTest, RemovePreservesFilesAndStableIds) {
    DownloadQueue queue;
    const auto first = queue.add(torrent("first"), config);
    const auto second = queue.add(torrent("second"), config);
    ASSERT_TRUE(ready(queue));
    ASSERT_TRUE(queue.enqueue(first));
    ASSERT_TRUE(until(queue, [&] { return queue.snapshots()[0].download.state == DownloadState::Complete; }));
    queue.remove(first);
    ASSERT_TRUE(until(queue, [&] { return queue.snapshots().size() == 1; }));
    EXPECT_EQ(queue.snapshots()[0].id, second);
    EXPECT_TRUE(fs::exists(fs::u8path(config.download_dir) / "first"));
    queue.close();
    ASSERT_TRUE(until(queue, [&] { return queue.closed(); }));
    EXPECT_FALSE(queue.enqueue(second));
}

TEST_F(DownloadQueueTest, SavedListRestoresChoicesAndLimitWithoutStartingDownloads) {
    const auto path = (directory / fs::u8path(u8"список.json")).u8string();
    DownloadQueue queue;
    const auto first = queue.add(torrent(u8"первый"), config);
    queue.add(torrent("second"), config);
    ASSERT_TRUE(ready(queue));
    ASSERT_TRUE(queue.configure(first, config.download_dir, {{0, FilePriority::High}}));
    queue.setLimit(3);
    ASSERT_TRUE(queue.setSpeedLimits(first, 32768, 16384));
    ASSERT_TRUE(queue.setSeedingOptions(first, {true, 1.5, 3}));
    queue.save(path);
    queue.save(path); // Replaces an existing file, including on Windows.
    DownloadQueue restored;
    restored.restore(path, config);
    ASSERT_TRUE(ready(restored));
    ASSERT_EQ(restored.snapshots().size(), 2u);
    EXPECT_EQ(restored.limit(), 3u);
    EXPECT_EQ(restored.snapshots()[0].config.max_download_speed, 32768);
    EXPECT_EQ(restored.snapshots()[0].config.max_upload_speed, 16384);
    EXPECT_EQ(restored.snapshots()[1].config.max_download_speed, 0);
    EXPECT_TRUE(restored.snapshots()[0].config.seed_after_download);
    EXPECT_DOUBLE_EQ(restored.snapshots()[0].config.seed_ratio_limit, 1.5);
    EXPECT_EQ(restored.snapshots()[0].config.seed_time_limit_hours, 3);
    EXPECT_FALSE(restored.snapshots()[1].config.seed_after_download);
    EXPECT_EQ(restored.snapshots()[0].config.file_priorities.at(0), FilePriority::High);
    EXPECT_EQ(restored.snapshots()[0].config.download_dir, config.download_dir);
    EXPECT_FALSE(restored.snapshots()[0].queued);
    EXPECT_FALSE(fs::exists(fs::u8path(config.download_dir)));
}

TEST_F(DownloadQueueTest, InvalidSavedListDoesNotRestoreOnlyPartOfTheList) {
    DownloadQueue queue;
    queue.add(torrent("first"), config);
    queue.add(torrent("second"), config);
    const auto path = (directory / "list.json").u8string();
    queue.save(path);
    nlohmann::json document;
    { std::ifstream input(fs::u8path(path)); input >> document; }
    document["downloads"][1]["priorities"] = {{{"index", 0}, {"priority", "broken"}}};
    { std::ofstream output(fs::u8path(path)); output << document; }
    DownloadQueue restored;
    EXPECT_THROW(restored.restore(path, config), std::invalid_argument);
    EXPECT_TRUE(restored.snapshots().empty());
    document["downloads"][1]["priorities"] = nlohmann::json::array();
    document["downloads"][1]["files"] = {-1};
    { std::ofstream output(fs::u8path(path)); output << document; }
    EXPECT_THROW(restored.restore(path, config), std::invalid_argument);
    EXPECT_TRUE(restored.snapshots().empty());
    EXPECT_THROW(restored.setLimit(0), std::invalid_argument);
    EXPECT_THROW(restored.setLimit(9), std::invalid_argument);
}

TEST_F(DownloadQueueTest, FailedMetadataDoesNotBlockAnotherTorrent) {
    DownloadQueue queue;
    const auto failed = queue.add((directory / "missing.torrent").u8string(), config);
    const auto valid = queue.add(torrent("valid"), config);
    queue.enqueue(failed); queue.enqueue(valid);
    ASSERT_TRUE(until(queue, [&] { return queue.snapshots()[1].download.state == DownloadState::Complete; }));
    EXPECT_EQ(queue.snapshots()[0].download.state, DownloadState::Failed);
    EXPECT_FALSE(queue.snapshots()[0].queued);
}

TEST_F(DownloadQueueTest, FailedTransferCanBeStartedAgainAfterCorrectingItsChoices) {
    DownloadQueue queue;
    const auto id = queue.add(torrent("retry"), config);
    ASSERT_TRUE(ready(queue));
    ASSERT_TRUE(queue.configure(id, config.download_dir, {{99, FilePriority::High}}));
    ASSERT_TRUE(queue.enqueue(id));
    ASSERT_TRUE(until(queue, [&] {
        const auto entry = queue.snapshots()[0];
        return !entry.busy && entry.download.state == DownloadState::Failed;
    }));
    ASSERT_TRUE(queue.configure(id, config.download_dir, {{0, FilePriority::Normal}}));
    ASSERT_TRUE(queue.enqueue(id));
    ASSERT_TRUE(until(queue, [&] { return queue.snapshots()[0].download.state == DownloadState::Complete; }));
}

TEST_F(DownloadQueueTest, LimitsCanChangeForWaitingJobsWithoutAffectingOtherEntries) {
    DownloadQueue queue;
    const auto first = queue.add(torrent("first"), config);
    queue.add(torrent("second"), config);
    ASSERT_TRUE(ready(queue));
    ASSERT_TRUE(queue.enqueue(first));
    ASSERT_TRUE(queue.setSpeedLimits(first, 2048, 4096));
    auto entries = queue.snapshots();
    EXPECT_EQ(entries[0].config.max_download_speed, 2048);
    EXPECT_EQ(entries[0].config.max_upload_speed, 4096);
    EXPECT_EQ(entries[1].config.max_download_speed, 0);
    EXPECT_THROW(queue.setSpeedLimits(first, -1, 0), std::invalid_argument);
    EXPECT_EQ(queue.snapshots()[0].config.max_download_speed, 2048);
    queue.remove(first);
    EXPECT_FALSE(queue.setSpeedLimits(first, 0, 0));
    queue.close();
    EXPECT_FALSE(queue.setSpeedLimits(entries[1].id, 0, 0));
}

TEST_F(DownloadQueueTest, LegacyListUsesDefaultLimitsAndInvalidRatesKeepTheListEmpty) {
    DownloadQueue queue;
    queue.add(torrent("legacy"), config);
    const auto path = (directory / "list.json").u8string();
    queue.save(path);
    nlohmann::json document;
    { std::ifstream input(fs::u8path(path)); input >> document; }
    document["downloads"][0].erase("max_download_speed");
    document["downloads"][0].erase("max_upload_speed");
    document["downloads"][0].erase("seed_after_download");
    document["downloads"][0].erase("seed_ratio_limit");
    document["downloads"][0].erase("seed_time_limit_hours");
    { std::ofstream output(fs::u8path(path)); output << document; }
    Config defaults = config; defaults.max_download_speed = 1234; defaults.max_upload_speed = 5678;
    DownloadQueue restored;
    restored.restore(path, defaults);
    EXPECT_EQ(restored.snapshots()[0].config.max_download_speed, 1234);
    EXPECT_EQ(restored.snapshots()[0].config.max_upload_speed, 5678);
    EXPECT_FALSE(restored.snapshots()[0].config.seed_after_download);
    for (const auto value : {nlohmann::json(-1), nlohmann::json(1.5), nlohmann::json("100")}) {
        document["downloads"][0]["max_download_speed"] = value;
        { std::ofstream output(fs::u8path(path)); output << document; }
        DownloadQueue invalid;
        EXPECT_THROW(invalid.restore(path, config), std::invalid_argument);
        EXPECT_TRUE(invalid.snapshots().empty());
    }
}

TEST_F(DownloadQueueTest, SeedingChoicesCanChangeWhileWaitingAndRejectInvalidValues) {
    DownloadQueue queue;
    const auto id = queue.add(torrent("waiting"), config);
    queue.enqueue(id);
    EXPECT_TRUE(queue.setSeedingOptions(id, {true, 0, 2}));
    EXPECT_TRUE(queue.snapshots()[0].config.seed_after_download);
    EXPECT_THROW(queue.setSeedingOptions(id, {true, -1, 0}), std::invalid_argument);
    EXPECT_EQ(queue.snapshots()[0].config.seed_time_limit_hours, 2);
    queue.close();
    EXPECT_FALSE(queue.setSeedingOptions(id, {false, 0, 0}));
}
