#include "download_task.h"
#include "torrent/metainfo/bencode.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <chrono>

using namespace torrent;
using namespace torrent::app;
namespace fs = std::filesystem;

class DownloadTaskTest : public ::testing::Test {
protected:
    fs::path directory;
    Config config;
    void SetUp() override {
        directory = fs::temp_directory_path() / ("torrent-ui-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())) / fs::u8path(u8"папка");
        fs::create_directories(directory);
        config.download_dir = (directory / fs::u8path(u8"загрузки")).u8string();
        config.enable_dht = config.enable_lsd = config.enable_utp = config.enable_webseeds = false;
    }
    void TearDown() override { fs::remove_all(directory.parent_path()); }
    std::string torrent() {
        BencodeValue::Dictionary info;
        info["name"] = BencodeValue(std::string(u8"пустой.txt"));
        info["length"] = BencodeValue(int64_t(0));
        info["piece length"] = BencodeValue(int64_t(16384));
        info["pieces"] = BencodeValue(std::string());
        BencodeValue::Dictionary root;
        root["info"] = BencodeValue(info);
        auto encoded = BencodeParser::encode(BencodeValue(root));
        auto path = directory / fs::u8path(u8"пример.torrent");
        std::ofstream out(path, std::ios::binary); out << encoded; out.close();
        return path.u8string();
    }
};

TEST_F(DownloadTaskTest, LoadCompleteAndResumeWithUnicodePaths) {
    DownloadTask task;
    ASSERT_TRUE(task.load(torrent(), config)); task.wait();
    auto snapshot = task.snapshot();
    ASSERT_EQ(snapshot.state, DownloadState::Ready);
    ASSERT_TRUE(snapshot.files);
    ASSERT_EQ(snapshot.files->size(), 1u);
    EXPECT_EQ(snapshot.name, u8"пустой.txt");
    ASSERT_TRUE(task.start(config)); task.wait();
    EXPECT_EQ(task.snapshot().state, DownloadState::Complete);
    EXPECT_DOUBLE_EQ(task.snapshot().progress, 100);
    EXPECT_TRUE(fs::exists(fs::u8path(config.download_dir) / fs::u8path(u8"пустой.txt")));
    ASSERT_TRUE(task.start(config)); task.wait();
    EXPECT_EQ(task.snapshot().state, DownloadState::Complete);
    ASSERT_TRUE(task.unload()); task.wait();
    EXPECT_EQ(task.snapshot().state, DownloadState::Idle);
    EXPECT_FALSE(task.snapshot().files);
    EXPECT_FALSE(task.start(config));
}

TEST_F(DownloadTaskTest, ErrorCanBeRecoveredByLoadingAnotherInput) {
    DownloadTask task;
    ASSERT_TRUE(task.load((directory / "missing.torrent").u8string(), config)); task.wait();
    EXPECT_EQ(task.snapshot().state, DownloadState::Failed);
    EXPECT_FALSE(task.snapshot().error.empty());
    ASSERT_TRUE(task.load(torrent(), config)); task.wait();
    EXPECT_EQ(task.snapshot().state, DownloadState::Ready);
    EXPECT_TRUE(task.snapshot().error.empty());
}

TEST_F(DownloadTaskTest, InvalidSelectionFailsWithoutLosingMetadata) {
    DownloadTask task;
    ASSERT_TRUE(task.load(torrent(), config)); task.wait();
    config.file_indices = {99};
    ASSERT_TRUE(task.start(config)); task.wait();
    EXPECT_EQ(task.snapshot().state, DownloadState::Failed);
    EXPECT_TRUE(task.snapshot().files);
    config.file_indices.clear();
    ASSERT_TRUE(task.start(config)); task.wait();
    EXPECT_EQ(task.snapshot().state, DownloadState::Complete);
}

TEST_F(DownloadTaskTest, ConfigurationRoundTripsUnicodePath) {
    const auto path = (directory / fs::u8path(u8"настройки.json")).u8string();
    ASSERT_TRUE(config.saveToFile(path));
    Config loaded;
    ASSERT_TRUE(loaded.loadFromFile(path));
    EXPECT_EQ(loaded.download_dir, config.download_dir);
}
