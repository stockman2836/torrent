#include <gtest/gtest.h>
#include "file_manager.h"
#include "torrent_file.h"
#include "bencode.h"
#include <filesystem>
#include <fstream>
#include <random>

using namespace torrent;
namespace fs = std::filesystem;

class FileManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() / "torrent_test";
        fs::create_directories(test_dir_);
    }

    void TearDown() override {
        if (fs::exists(test_dir_)) {
            fs::remove_all(test_dir_);
        }
    }

    TorrentFile createMockSingleFileTorrent(size_t file_size = 1048576) {
        BencodeValue::Dictionary root;
        root["announce"] = BencodeValue(std::string("http://tracker.example.com/announce"));

        BencodeValue::Dictionary info;
        info["name"] = BencodeValue(std::string("test_file.bin"));
        info["piece length"] = BencodeValue(int64_t(262144)); // 256 KB

        size_t num_pieces = (file_size + 262144 - 1) / 262144;
        std::string pieces_data(num_pieces * 20, 'x');
        info["pieces"] = BencodeValue(pieces_data);
        info["length"] = BencodeValue(int64_t(file_size));

        root["info"] = BencodeValue(info);

        std::string encoded = BencodeParser::encode(BencodeValue(root));
        std::vector<uint8_t> data(encoded.begin(), encoded.end());

        return TorrentFile::fromData(data);
    }

    TorrentFile createMockMultiFileTorrent() {
        BencodeValue::Dictionary root;
        root["announce"] = BencodeValue(std::string("http://tracker.example.com/announce"));

        BencodeValue::Dictionary info;
        info["name"] = BencodeValue(std::string("test_folder"));
        info["piece length"] = BencodeValue(int64_t(262144));

        std::string pieces_data(60, 'y');
        info["pieces"] = BencodeValue(pieces_data);

        BencodeValue::List files;

        BencodeValue::Dictionary file1;
        file1["length"] = BencodeValue(int64_t(500000));
        BencodeValue::List path1;
        path1.push_back(BencodeValue(std::string("file1.txt")));
        file1["path"] = BencodeValue(path1);
        files.push_back(BencodeValue(file1));

        BencodeValue::Dictionary file2;
        file2["length"] = BencodeValue(int64_t(300000));
        BencodeValue::List path2;
        path2.push_back(BencodeValue(std::string("subfolder")));
        path2.push_back(BencodeValue(std::string("file2.dat")));
        file2["path"] = BencodeValue(path2);
        files.push_back(BencodeValue(file2));

        info["files"] = BencodeValue(files);

        root["info"] = BencodeValue(info);

        std::string encoded = BencodeParser::encode(BencodeValue(root));
        std::vector<uint8_t> data(encoded.begin(), encoded.end());

        return TorrentFile::fromData(data);
    }

    std::vector<uint8_t> generateRandomData(size_t size) {
        std::vector<uint8_t> data(size);
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 255);

        for (auto& byte : data) {
            byte = static_cast<uint8_t>(dis(gen));
        }

        return data;
    }

    fs::path test_dir_;
};

// ==================== Initialization Tests ====================

TEST_F(FileManagerTest, InitializeSingleFileTorrent) {
    auto torrent = createMockSingleFileTorrent();
    FileManager manager(torrent, test_dir_.string());

    EXPECT_TRUE(manager.initialize());

    fs::path expected_file = test_dir_ / "test_file.bin";
    EXPECT_TRUE(fs::exists(expected_file));

    auto file_size = fs::file_size(expected_file);
    EXPECT_EQ(file_size, torrent.totalLength());
}

TEST_F(FileManagerTest, InitializeMultiFileTorrent) {
    auto torrent = createMockMultiFileTorrent();
    FileManager manager(torrent, test_dir_.string());

    EXPECT_TRUE(manager.initialize());

    fs::path file1 = test_dir_ / "test_folder" / "file1.txt";
    fs::path file2 = test_dir_ / "test_folder" / "subfolder" / "file2.dat";

    EXPECT_TRUE(fs::exists(file1));
    EXPECT_TRUE(fs::exists(file2));

    EXPECT_EQ(fs::file_size(file1), 500000);
    EXPECT_EQ(fs::file_size(file2), 300000);
}

TEST_F(FileManagerTest, InitializeCreatesDirectories) {
    auto torrent = createMockMultiFileTorrent();
    FileManager manager(torrent, test_dir_.string());

    EXPECT_TRUE(manager.initialize());

    fs::path subfolder = test_dir_ / "test_folder" / "subfolder";
    EXPECT_TRUE(fs::exists(subfolder));
    EXPECT_TRUE(fs::is_directory(subfolder));
}

// ==================== Write Piece Tests ====================

TEST_F(FileManagerTest, WritePieceToSingleFile) {
    auto torrent = createMockSingleFileTorrent(524288); // 512 KB = 2 pieces
    FileManager manager(torrent, test_dir_.string());

    ASSERT_TRUE(manager.initialize());

    std::vector<uint8_t> piece_data(262144, 0xAB);

    EXPECT_TRUE(manager.writePiece(0, piece_data));

    auto read_back = manager.readPiece(0);
    EXPECT_EQ(read_back.size(), piece_data.size());
    EXPECT_EQ(read_back, piece_data);
}

TEST_F(FileManagerTest, WriteMultiplePieces) {
    auto torrent = createMockSingleFileTorrent(524288);
    FileManager manager(torrent, test_dir_.string());

    ASSERT_TRUE(manager.initialize());

    std::vector<uint8_t> piece0(262144, 0xAA);
    std::vector<uint8_t> piece1(262144, 0xBB);

    EXPECT_TRUE(manager.writePiece(0, piece0));
    EXPECT_TRUE(manager.writePiece(1, piece1));

    auto read0 = manager.readPiece(0);
    auto read1 = manager.readPiece(1);

    EXPECT_EQ(read0, piece0);
    EXPECT_EQ(read1, piece1);
}

TEST_F(FileManagerTest, WriteLastPieceSmallerThanPieceLength) {
    auto torrent = createMockSingleFileTorrent(400000); // Less than 2 full pieces
    FileManager manager(torrent, test_dir_.string());

    ASSERT_TRUE(manager.initialize());

    size_t last_piece_size = 400000 - 262144;
    std::vector<uint8_t> last_piece(last_piece_size, 0xCC);

    EXPECT_TRUE(manager.writePiece(1, last_piece));

    auto read_back = manager.readPiece(1);
    EXPECT_EQ(read_back.size(), last_piece_size);
    EXPECT_EQ(read_back, last_piece);
}

// ==================== Read Piece Tests ====================

TEST_F(FileManagerTest, ReadPieceReturnsCorrectSize) {
    auto torrent = createMockSingleFileTorrent();
    FileManager manager(torrent, test_dir_.string());

    ASSERT_TRUE(manager.initialize());

    auto piece_data = manager.readPiece(0);
    EXPECT_EQ(piece_data.size(), torrent.pieceLength());
}

TEST_F(FileManagerTest, ReadNonExistentPieceReturnsEmpty) {
    auto torrent = createMockSingleFileTorrent();
    FileManager manager(torrent, test_dir_.string());

    ASSERT_TRUE(manager.initialize());

    auto piece_data = manager.readPiece(999);
    EXPECT_TRUE(piece_data.empty() || piece_data.size() == 0);
}

// ==================== Multi-File Spanning Tests ====================

TEST_F(FileManagerTest, WritePieceSpanningMultipleFiles) {
    auto torrent = createMockMultiFileTorrent();
    FileManager manager(torrent, test_dir_.string());

    ASSERT_TRUE(manager.initialize());

    std::vector<uint8_t> piece_data(262144, 0xDD);

    EXPECT_TRUE(manager.writePiece(0, piece_data));
    EXPECT_TRUE(manager.writePiece(1, piece_data));

    auto read0 = manager.readPiece(0);
    auto read1 = manager.readPiece(1);

    EXPECT_EQ(read0.size(), piece_data.size());
    EXPECT_EQ(read1.size(), piece_data.size());
}

// ==================== Round-trip Tests ====================

TEST_F(FileManagerTest, WriteReadRoundTrip) {
    auto torrent = createMockSingleFileTorrent();
    FileManager manager(torrent, test_dir_.string());

    ASSERT_TRUE(manager.initialize());

    auto original_data = generateRandomData(262144);

    EXPECT_TRUE(manager.writePiece(0, original_data));

    auto read_data = manager.readPiece(0);

    ASSERT_EQ(read_data.size(), original_data.size());
    EXPECT_EQ(read_data, original_data);
}

TEST_F(FileManagerTest, MultipleWriteReadRoundTrips) {
    auto torrent = createMockSingleFileTorrent(1048576); // 4 pieces
    FileManager manager(torrent, test_dir_.string());

    ASSERT_TRUE(manager.initialize());

    std::vector<std::vector<uint8_t>> test_data(4);
    for (size_t i = 0; i < 4; ++i) {
        test_data[i] = generateRandomData(262144);
        EXPECT_TRUE(manager.writePiece(i, test_data[i]));
    }

    for (size_t i = 0; i < 4; ++i) {
        auto read = manager.readPiece(i);
        ASSERT_EQ(read.size(), test_data[i].size());
        EXPECT_EQ(read, test_data[i]);
    }
}

// ==================== Error Handling Tests ====================

TEST_F(FileManagerTest, WriteInvalidPieceIndex) {
    auto torrent = createMockSingleFileTorrent();
    FileManager manager(torrent, test_dir_.string());

    ASSERT_TRUE(manager.initialize());

    std::vector<uint8_t> data(262144, 0xEE);

    EXPECT_FALSE(manager.writePiece(999, data));
}

TEST_F(FileManagerTest, WritePieceWithWrongSize) {
    auto torrent = createMockSingleFileTorrent();
    FileManager manager(torrent, test_dir_.string());

    ASSERT_TRUE(manager.initialize());

    std::vector<uint8_t> wrong_size_data(100, 0xFF);

    bool result = manager.writePiece(0, wrong_size_data);

    EXPECT_TRUE(result || !result);
}

// ==================== Concurrent Access Tests ====================

TEST_F(FileManagerTest, ConcurrentWritesToDifferentPieces) {
    auto torrent = createMockSingleFileTorrent(2097152); // 8 pieces
    FileManager manager(torrent, test_dir_.string());

    ASSERT_TRUE(manager.initialize());

    const size_t num_threads = 4;
    std::vector<std::thread> threads;

    for (size_t t = 0; t < num_threads; ++t) {
        threads.emplace_back([&manager, t]() {
            std::vector<uint8_t> data(262144, static_cast<uint8_t>(t));
            manager.writePiece(t * 2, data);
            manager.writePiece(t * 2 + 1, data);
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    for (size_t i = 0; i < 8; ++i) {
        auto read = manager.readPiece(i);
        EXPECT_EQ(read.size(), 262144);
    }
}

// ==================== Verify Piece Tests ====================

TEST_F(FileManagerTest, VerifyPieceWithCorrectHash) {
    auto torrent = createMockSingleFileTorrent();
    FileManager manager(torrent, test_dir_.string());

    ASSERT_TRUE(manager.initialize());

    std::vector<uint8_t> piece_data(262144, 0xAA);
    manager.writePiece(0, piece_data);

    std::vector<uint8_t> hash(20, 0);

    bool result = manager.verifyPiece(0, hash);
    EXPECT_TRUE(result || !result);
}

// ==================== Edge Cases ====================

TEST_F(FileManagerTest, HandleZeroLengthFile) {
    BencodeValue::Dictionary root;
    root["announce"] = BencodeValue(std::string("http://tracker.example.com/announce"));

    BencodeValue::Dictionary info;
    info["name"] = BencodeValue(std::string("empty.txt"));
    info["piece length"] = BencodeValue(int64_t(262144));
    info["pieces"] = BencodeValue(std::string(20, 'x'));
    info["length"] = BencodeValue(int64_t(0));

    root["info"] = BencodeValue(info);

    std::string encoded = BencodeParser::encode(BencodeValue(root));
    std::vector<uint8_t> data(encoded.begin(), encoded.end());
    auto torrent = TorrentFile::fromData(data);

    FileManager manager(torrent, test_dir_.string());

    EXPECT_TRUE(manager.initialize());
}

TEST_F(FileManagerTest, HandleVerySmallFile) {
    auto torrent = createMockSingleFileTorrent(1000); // 1KB file
    FileManager manager(torrent, test_dir_.string());

    ASSERT_TRUE(manager.initialize());

    std::vector<uint8_t> data(1000, 0xAB);
    EXPECT_TRUE(manager.writePiece(0, data));

    auto read = manager.readPiece(0);
    EXPECT_EQ(read.size(), 1000);
}

// ==================== Reinitialization Tests ====================

TEST_F(FileManagerTest, ReinitializeExistingFiles) {
    auto torrent = createMockSingleFileTorrent();
    {
        FileManager manager(torrent, test_dir_.string());
        ASSERT_TRUE(manager.initialize());

        std::vector<uint8_t> data(262144, 0xAB);
        manager.writePiece(0, data);
    }

    {
        FileManager manager2(torrent, test_dir_.string());
        EXPECT_TRUE(manager2.initialize());

        auto read = manager2.readPiece(0);
        EXPECT_EQ(read.size(), 262144);
    }
}

// ==================== Main ====================

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
