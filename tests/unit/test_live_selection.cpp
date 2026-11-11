#include "torrent/storage/file_manager.h"
#include "torrent/storage/piece_manager.h"
#include "torrent/metainfo/file_selection.h"
#include "torrent/core/utils.h"
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace torrent;
namespace fs = std::filesystem;

class LiveSelectionTest : public ::testing::Test {
protected:
    fs::path directory;
    std::vector<uint8_t> data;
    TorrentFile torrent;
    void SetUp() override {
        directory = fs::temp_directory_path() / ("torrent-live-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(directory);
        data.resize(32768);
        for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i % 251);
        BencodeValue::Dictionary info;
        info["name"] = BencodeValue(std::string("payload"));
        info["piece length"] = BencodeValue(int64_t{16384});
        std::string hashes;
        for (size_t offset = 0; offset < data.size(); offset += 16384) {
            const auto digest = utils::sha1(std::vector<uint8_t>(data.begin() + offset, data.begin() + offset + 16384));
            hashes.append(digest.begin(), digest.end());
        }
        info["pieces"] = BencodeValue(hashes);
        BencodeValue::List files;
        const int64_t lengths[] = {8000, 24768, 0};
        for (int i = 0; i < 3; ++i) {
            BencodeValue::Dictionary file;
            file["length"] = BencodeValue(lengths[i]);
            file["path"] = BencodeValue(BencodeValue::List{BencodeValue("file" + std::to_string(i))});
            files.emplace_back(file);
        }
        info["files"] = BencodeValue(files);
        BencodeValue::Dictionary root; root["info"] = BencodeValue(info);
        const auto encoded = BencodeParser::encode(BencodeValue(root));
        torrent = TorrentFile::fromData({encoded.begin(), encoded.end()});
    }
    void TearDown() override { fs::remove_all(directory); }
    fs::path path(size_t index) const { return directory / fs::u8path(torrent.files()[index].path); }
    std::vector<uint8_t> read(size_t index) const {
        std::ifstream input(path(index), std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }
    std::vector<uint8_t> piece(size_t index) const { return {data.begin() + index * 16384, data.begin() + (index + 1) * 16384}; }
};

TEST_F(LiveSelectionTest, EnablingAFileRestoresVerifiedCachedBytesWithoutAnotherDownload) {
    FileManager manager(torrent, directory.u8string(), std::vector<size_t>{0});
    ASSERT_TRUE(manager.initialize()); ASSERT_TRUE(manager.writePiece(0, piece(0)));
    ASSERT_FALSE(fs::exists(path(1)));
    manager.setSelection(FileSelection(torrent), {true, false});
    const auto restored = read(1);
    ASSERT_EQ(restored.size(), 24768u);
    EXPECT_TRUE(std::equal(data.begin() + 8000, data.begin() + 16384, restored.begin()));
    EXPECT_EQ(manager.readPiece(0), piece(0));
    EXPECT_TRUE(fs::exists(path(2))); EXPECT_EQ(fs::file_size(path(2)), 0u);
    ASSERT_TRUE(manager.writePiece(1, piece(1)));
    EXPECT_EQ(read(1), (std::vector<uint8_t>{data.begin() + 8000, data.end()}));
}

TEST_F(LiveSelectionTest, DeselectingAFilePreservesItsDataAndCachesTheSharedBoundary) {
    FileManager manager(torrent, directory.u8string()); ASSERT_TRUE(manager.initialize());
    ASSERT_TRUE(manager.writePiece(0, piece(0))); ASSERT_TRUE(manager.writePiece(1, piece(1)));
    manager.setSelection(FileSelection(torrent, {1}), {true, true});
    EXPECT_EQ(read(0), (std::vector<uint8_t>{data.begin(), data.begin() + 8000}));
    ASSERT_TRUE(fs::remove(path(0)));
    EXPECT_EQ(manager.readPiece(0), piece(0));
    manager.setSelection(FileSelection(torrent), {true, true});
    EXPECT_EQ(read(0), (std::vector<uint8_t>{data.begin(), data.begin() + 8000}));
    EXPECT_EQ(manager.readPiece(0), piece(0));
}

TEST_F(LiveSelectionTest, SkippingEverythingDoesNotDeleteFilesOrCopyWholeFilesIntoTheCache) {
    FileManager manager(torrent, directory.u8string()); ASSERT_TRUE(manager.initialize());
    ASSERT_TRUE(manager.writePiece(0, piece(0))); ASSERT_TRUE(manager.writePiece(1, piece(1)));
    manager.setSelection(FileSelection(torrent, {}, {{0, FilePriority::Skip}, {1, FilePriority::Skip}, {2, FilePriority::Skip}}), {true, true});
    EXPECT_EQ(read(0), (std::vector<uint8_t>{data.begin(), data.begin() + 8000}));
    EXPECT_FALSE(fs::exists(directory / (".parts_" + utils::toHex(torrent.infoHash()))));
    manager.setSelection(FileSelection(torrent), {true, true});
    EXPECT_EQ(manager.readPiece(0), piece(0)); EXPECT_EQ(manager.readPiece(1), piece(1));
}

TEST_F(LiveSelectionTest, AnUnwritableNewPathKeepsThePreviousSelectionUsable) {
    FileManager manager(torrent, directory.u8string(), std::vector<size_t>{0});
    ASSERT_TRUE(manager.initialize()); ASSERT_TRUE(manager.writePiece(0, piece(0)));
    fs::create_directory(path(1));
    EXPECT_THROW(manager.setSelection(FileSelection(torrent), {true, false}), std::exception);
    EXPECT_EQ(manager.readPiece(0), piece(0));
    EXPECT_TRUE(manager.writePiece(0, piece(0)));
    EXPECT_TRUE(fs::is_directory(path(1)));
}

TEST_F(LiveSelectionTest, ChangedVerifiedBytesAreNotCopiedIntoANewFile) {
    FileManager manager(torrent, directory.u8string(), std::vector<size_t>{0});
    ASSERT_TRUE(manager.initialize()); ASSERT_TRUE(manager.writePiece(0, piece(0)));
    { std::fstream changed(path(0), std::ios::binary | std::ios::in | std::ios::out); changed.put('X'); }
    EXPECT_THROW(manager.setSelection(FileSelection(torrent), {true, false}), std::runtime_error);
    EXPECT_EQ(read(1), std::vector<uint8_t>(24768, 0));
    EXPECT_TRUE(manager.writePiece(0, piece(0)));
    EXPECT_EQ(manager.readPiece(0), piece(0));
}

TEST(PieceManagerLiveSelection, SkippingDiscardsOnlyUnverifiedAssemblyAndAllowsSelectingItAgain) {
    const std::vector<uint8_t> data(65536, 7);
    std::vector<uint8_t> hashes;
    const auto digest = utils::sha1(std::vector<uint8_t>(data.begin(), data.begin() + 32768));
    hashes.insert(hashes.end(), digest.begin(), digest.end()); hashes.insert(hashes.end(), digest.begin(), digest.end());
    PieceManager pieces(2, 32768, 65536, hashes);
    pieces.markPieceComplete(0);
    ASSERT_TRUE(pieces.addBlock(1, 0, std::vector<uint8_t>(16384, 7)));
    pieces.setPiecePriorities({FilePriority::Normal, FilePriority::Skip});
    EXPECT_TRUE(pieces.hasPiece(0)); EXPECT_FALSE(pieces.isPieceInProgress(1));
    EXPECT_DOUBLE_EQ(pieces.percentComplete(), 100);
    pieces.setPiecePriorities({FilePriority::Low, FilePriority::High});
    EXPECT_EQ(pieces.getNextPieceSequential({true, true}, {}), 1);
    EXPECT_EQ(pieces.getMissingBlocks(1).size(), 2u);
    EXPECT_DOUBLE_EQ(pieces.percentComplete(), 50);
}
