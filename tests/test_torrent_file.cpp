#include <gtest/gtest.h>
#include "torrent_file.h"
#include "bencode.h"
#include <vector>
#include <map>

using namespace torrent;

class TorrentFileTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}

    std::vector<uint8_t> createMockSingleFileTorrent() {
        BencodeValue::Dictionary root;
        root["announce"] = BencodeValue(std::string("http://tracker.example.com:8080/announce"));

        BencodeValue::Dictionary info;
        info["name"] = BencodeValue(std::string("test_file.txt"));
        info["piece length"] = BencodeValue(int64_t(262144)); // 256 KB

        std::string pieces_data(40, 'x');
        info["pieces"] = BencodeValue(pieces_data);
        info["length"] = BencodeValue(int64_t(1048576)); // 1 MB

        root["info"] = BencodeValue(info);

        std::string encoded = BencodeParser::encode(BencodeValue(root));
        return std::vector<uint8_t>(encoded.begin(), encoded.end());
    }

    std::vector<uint8_t> createMockMultiFileTorrent() {
        BencodeValue::Dictionary root;
        root["announce"] = BencodeValue(std::string("http://tracker.example.com:8080/announce"));
        root["comment"] = BencodeValue(std::string("Test torrent"));
        root["created by"] = BencodeValue(std::string("Unit Test"));
        root["creation date"] = BencodeValue(int64_t(1234567890));

        BencodeValue::Dictionary info;
        info["name"] = BencodeValue(std::string("test_folder"));
        info["piece length"] = BencodeValue(int64_t(524288)); // 512 KB

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
        file2["length"] = BencodeValue(int64_t(750000));
        BencodeValue::List path2;
        path2.push_back(BencodeValue(std::string("subfolder")));
        path2.push_back(BencodeValue(std::string("file2.dat")));
        file2["path"] = BencodeValue(path2);
        files.push_back(BencodeValue(file2));

        info["files"] = BencodeValue(files);

        root["info"] = BencodeValue(info);

        std::string encoded = BencodeParser::encode(BencodeValue(root));
        return std::vector<uint8_t>(encoded.begin(), encoded.end());
    }

    std::vector<uint8_t> createTorrentWithAnnounceList() {
        BencodeValue::Dictionary root;
        root["announce"] = BencodeValue(std::string("http://tracker1.example.com/announce"));

        BencodeValue::List announce_list;
        BencodeValue::List tier1;
        tier1.push_back(BencodeValue(std::string("http://tracker1.example.com/announce")));
        tier1.push_back(BencodeValue(std::string("http://tracker2.example.com/announce")));
        announce_list.push_back(BencodeValue(tier1));

        BencodeValue::List tier2;
        tier2.push_back(BencodeValue(std::string("http://tracker3.example.com/announce")));
        announce_list.push_back(BencodeValue(tier2));

        root["announce-list"] = BencodeValue(announce_list);

        BencodeValue::Dictionary info;
        info["name"] = BencodeValue(std::string("test.txt"));
        info["piece length"] = BencodeValue(int64_t(262144));
        info["pieces"] = BencodeValue(std::string(20, 'z'));
        info["length"] = BencodeValue(int64_t(100000));

        root["info"] = BencodeValue(info);

        std::string encoded = BencodeParser::encode(BencodeValue(root));
        return std::vector<uint8_t>(encoded.begin(), encoded.end());
    }
};

// ==================== Single File Torrent Tests ====================

TEST_F(TorrentFileTest, ParseSingleFileTorrent) {
    auto data = createMockSingleFileTorrent();

    TorrentFile torrent = TorrentFile::fromData(data);

    EXPECT_EQ(torrent.announce(), "http://tracker.example.com:8080/announce");
    EXPECT_EQ(torrent.name(), "test_file.txt");
    EXPECT_EQ(torrent.pieceLength(), 262144);
    EXPECT_EQ(torrent.totalLength(), 1048576);
    EXPECT_EQ(torrent.numPieces(), 2);
    EXPECT_TRUE(torrent.isSingleFile());
    EXPECT_EQ(torrent.files().size(), 1);
    EXPECT_EQ(torrent.files()[0].path, "test_file.txt");
    EXPECT_EQ(torrent.files()[0].length, 1048576);
}

TEST_F(TorrentFileTest, InfoHashIsCalculated) {
    auto data = createMockSingleFileTorrent();
    TorrentFile torrent = TorrentFile::fromData(data);

    const auto& hash = torrent.infoHash();
    EXPECT_EQ(hash.size(), 20);

    auto torrent2 = TorrentFile::fromData(data);
    EXPECT_EQ(torrent.infoHash(), torrent2.infoHash());
}

TEST_F(TorrentFileTest, PiecesAreParsedCorrectly) {
    auto data = createMockSingleFileTorrent();
    TorrentFile torrent = TorrentFile::fromData(data);

    const auto& pieces = torrent.pieces();
    EXPECT_EQ(pieces.size(), 40);
    EXPECT_EQ(pieces.size() % 20, 0);
}

// ==================== Multi File Torrent Tests ====================

TEST_F(TorrentFileTest, ParseMultiFileTorrent) {
    auto data = createMockMultiFileTorrent();
    TorrentFile torrent = TorrentFile::fromData(data);

    EXPECT_EQ(torrent.name(), "test_folder");
    EXPECT_EQ(torrent.pieceLength(), 524288);
    EXPECT_EQ(torrent.totalLength(), 1250000);
    EXPECT_EQ(torrent.numPieces(), 3);
    EXPECT_FALSE(torrent.isSingleFile());
    EXPECT_EQ(torrent.files().size(), 2);
}

TEST_F(TorrentFileTest, MultiFilePathsAreCorrect) {
    auto data = createMockMultiFileTorrent();
    TorrentFile torrent = TorrentFile::fromData(data);

    const auto& files = torrent.files();
    ASSERT_EQ(files.size(), 2);

    EXPECT_EQ(files[0].path, "test_folder/file1.txt");
    EXPECT_EQ(files[0].length, 500000);

    EXPECT_EQ(files[1].path, "test_folder/subfolder/file2.dat");
    EXPECT_EQ(files[1].length, 750000);
}

TEST_F(TorrentFileTest, OptionalFieldsAreParsed) {
    auto data = createMockMultiFileTorrent();
    TorrentFile torrent = TorrentFile::fromData(data);

    EXPECT_EQ(torrent.announce(), "http://tracker.example.com:8080/announce");
}

// ==================== Announce List Tests ====================

TEST_F(TorrentFileTest, AnnounceListIsParsed) {
    auto data = createTorrentWithAnnounceList();
    TorrentFile torrent = TorrentFile::fromData(data);

    const auto& announce_list = torrent.announceList();
    EXPECT_EQ(announce_list.size(), 3);
    EXPECT_EQ(announce_list[0], "http://tracker1.example.com/announce");
    EXPECT_EQ(announce_list[1], "http://tracker2.example.com/announce");
    EXPECT_EQ(announce_list[2], "http://tracker3.example.com/announce");
}

// ==================== Error Handling Tests ====================

TEST_F(TorrentFileTest, MissingInfoDictionaryThrows) {
    BencodeValue::Dictionary root;
    root["announce"] = BencodeValue(std::string("http://tracker.example.com/announce"));

    std::string encoded = BencodeParser::encode(BencodeValue(root));
    std::vector<uint8_t> data(encoded.begin(), encoded.end());

    EXPECT_THROW(TorrentFile::fromData(data), std::runtime_error);
}

TEST_F(TorrentFileTest, MissingNameThrows) {
    BencodeValue::Dictionary root;
    root["announce"] = BencodeValue(std::string("http://tracker.example.com/announce"));

    BencodeValue::Dictionary info;
    info["piece length"] = BencodeValue(int64_t(262144));
    info["pieces"] = BencodeValue(std::string(20, 'x'));
    info["length"] = BencodeValue(int64_t(100000));

    root["info"] = BencodeValue(info);

    std::string encoded = BencodeParser::encode(BencodeValue(root));
    std::vector<uint8_t> data(encoded.begin(), encoded.end());

    EXPECT_THROW(TorrentFile::fromData(data), std::runtime_error);
}

TEST_F(TorrentFileTest, MissingPieceLengthThrows) {
    BencodeValue::Dictionary root;
    root["announce"] = BencodeValue(std::string("http://tracker.example.com/announce"));

    BencodeValue::Dictionary info;
    info["name"] = BencodeValue(std::string("test.txt"));
    info["pieces"] = BencodeValue(std::string(20, 'x'));
    info["length"] = BencodeValue(int64_t(100000));

    root["info"] = BencodeValue(info);

    std::string encoded = BencodeParser::encode(BencodeValue(root));
    std::vector<uint8_t> data(encoded.begin(), encoded.end());

    EXPECT_THROW(TorrentFile::fromData(data), std::runtime_error);
}

TEST_F(TorrentFileTest, MissingPiecesThrows) {
    BencodeValue::Dictionary root;
    root["announce"] = BencodeValue(std::string("http://tracker.example.com/announce"));

    BencodeValue::Dictionary info;
    info["name"] = BencodeValue(std::string("test.txt"));
    info["piece length"] = BencodeValue(int64_t(262144));
    info["length"] = BencodeValue(int64_t(100000));

    root["info"] = BencodeValue(info);

    std::string encoded = BencodeParser::encode(BencodeValue(root));
    std::vector<uint8_t> data(encoded.begin(), encoded.end());

    EXPECT_THROW(TorrentFile::fromData(data), std::runtime_error);
}

TEST_F(TorrentFileTest, InvalidPiecesSizeThrows) {
    BencodeValue::Dictionary root;
    root["announce"] = BencodeValue(std::string("http://tracker.example.com/announce"));

    BencodeValue::Dictionary info;
    info["name"] = BencodeValue(std::string("test.txt"));
    info["piece length"] = BencodeValue(int64_t(262144));
    info["pieces"] = BencodeValue(std::string(25, 'x'));
    info["length"] = BencodeValue(int64_t(100000));

    root["info"] = BencodeValue(info);

    std::string encoded = BencodeParser::encode(BencodeValue(root));
    std::vector<uint8_t> data(encoded.begin(), encoded.end());

    EXPECT_THROW(TorrentFile::fromData(data), std::runtime_error);
}

TEST_F(TorrentFileTest, NotADictionaryThrows) {
    BencodeValue value(int64_t(42));
    std::string encoded = BencodeParser::encode(value);
    std::vector<uint8_t> data(encoded.begin(), encoded.end());

    EXPECT_THROW(TorrentFile::fromData(data), std::runtime_error);
}

TEST_F(TorrentFileTest, MissingLengthAndFilesThrows) {
    BencodeValue::Dictionary root;
    root["announce"] = BencodeValue(std::string("http://tracker.example.com/announce"));

    BencodeValue::Dictionary info;
    info["name"] = BencodeValue(std::string("test"));
    info["piece length"] = BencodeValue(int64_t(262144));
    info["pieces"] = BencodeValue(std::string(20, 'x'));

    root["info"] = BencodeValue(info);

    std::string encoded = BencodeParser::encode(BencodeValue(root));
    std::vector<uint8_t> data(encoded.begin(), encoded.end());

    EXPECT_THROW(TorrentFile::fromData(data), std::runtime_error);
}

// ==================== Edge Cases ====================

TEST_F(TorrentFileTest, EmptyFilesListHandled) {
    BencodeValue::Dictionary root;
    root["announce"] = BencodeValue(std::string("http://tracker.example.com/announce"));

    BencodeValue::Dictionary info;
    info["name"] = BencodeValue(std::string("test_folder"));
    info["piece length"] = BencodeValue(int64_t(262144));
    info["pieces"] = BencodeValue(std::string(20, 'x'));

    BencodeValue::List files;
    info["files"] = BencodeValue(files);

    root["info"] = BencodeValue(info);

    std::string encoded = BencodeParser::encode(BencodeValue(root));
    std::vector<uint8_t> data(encoded.begin(), encoded.end());

    TorrentFile torrent = TorrentFile::fromData(data);
    EXPECT_EQ(torrent.totalLength(), 0);
    EXPECT_EQ(torrent.files().size(), 0);
}

TEST_F(TorrentFileTest, VeryLargePieceLength) {
    BencodeValue::Dictionary root;
    root["announce"] = BencodeValue(std::string("http://tracker.example.com/announce"));

    BencodeValue::Dictionary info;
    info["name"] = BencodeValue(std::string("large_file.bin"));
    info["piece length"] = BencodeValue(int64_t(16777216)); // 16 MB
    info["pieces"] = BencodeValue(std::string(20, 'x'));
    info["length"] = BencodeValue(int64_t(1073741824)); // 1 GB

    root["info"] = BencodeValue(info);

    std::string encoded = BencodeParser::encode(BencodeValue(root));
    std::vector<uint8_t> data(encoded.begin(), encoded.end());

    TorrentFile torrent = TorrentFile::fromData(data);
    EXPECT_EQ(torrent.pieceLength(), 16777216);
    EXPECT_EQ(torrent.totalLength(), 1073741824);
}

// ==================== Main ====================

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
