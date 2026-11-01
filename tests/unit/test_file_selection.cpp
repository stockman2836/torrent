#include "torrent/metainfo/file_selection.h"
#include <gtest/gtest.h>
#include <algorithm>

using namespace torrent;

namespace {
TorrentFile metadata(const std::vector<int64_t>& lengths) {
    int64_t total = 0;
    BencodeValue::List files;
    for (size_t i = 0; i < lengths.size(); ++i) {
        files.emplace_back(BencodeValue::Dictionary{{"length", lengths[i]},
            {"path", BencodeValue::List{std::string("file") + std::to_string(i)}}});
        total += lengths[i];
    }
    const auto bytes = BencodeParser::encode(BencodeValue::Dictionary{
        {"info", BencodeValue::Dictionary{{"name", std::string("payload")}, {"files", files},
            {"piece length", int64_t(16384)}, {"pieces", std::string((total + 16383) / 16384 * 20, 'x')}}}});
    return TorrentFile::fromData({bytes.begin(), bytes.end()});
}
}

TEST(FileSelection, IncludesBothBoundaryPiecesAndOnlySelectedFiles) {
    const auto torrent = metadata({10007, 40009, 65536, 777, 0});
    const FileSelection selection(torrent, {1});
    EXPECT_EQ(selection.files(), (std::vector<bool>{false, true, false, false, false}));
    EXPECT_EQ(selection.pieces(), (std::vector<bool>{true, true, true, true, false, false, false, false}));
    EXPECT_EQ(selection.selectedBytes(), 40009);
}

TEST(FileSelection, CombinesDisjointSelectionsWithoutDoubleCounting) {
    const auto torrent = metadata({10007, 40009, 65536, 777, 0});
    const FileSelection selection(torrent, {1, 3, 1});
    EXPECT_EQ(selection.pieces(), (std::vector<bool>{true, true, true, true, false, false, false, true}));
    EXPECT_EQ(selection.selectedBytes(), 40009 + 777);
}

TEST(FileSelection, EmptyIndexListSelectsAllFiles) {
    const auto torrent = metadata({10007, 40009, 65536, 777, 0});
    const FileSelection selection(torrent);
    EXPECT_TRUE(std::all_of(selection.files().begin(), selection.files().end(), [](bool value) { return value; }));
    EXPECT_TRUE(std::all_of(selection.pieces().begin(), selection.pieces().end(), [](bool value) { return value; }));
}

TEST(FileSelection, ZeroLengthFilesRequireNoPieces) {
    const auto torrent = metadata({10007, 40009, 65536, 777, 0});
    const FileSelection selection(torrent, {4});
    EXPECT_EQ(selection.selectedBytes(), 0);
    EXPECT_EQ(std::count(selection.pieces().begin(), selection.pieces().end(), true), 0);
}

TEST(FileSelection, AlignedFilesDoNotIncludeTheNextPiece) {
    const auto torrent = metadata({16384, 0, 16384});
    EXPECT_EQ(FileSelection(torrent, {0}).pieces(), (std::vector<bool>{true, false}));
    EXPECT_EQ(FileSelection(torrent, {1}).pieces(), (std::vector<bool>{false, false}));
    EXPECT_EQ(FileSelection(torrent, {2}).pieces(), (std::vector<bool>{false, true}));
}

TEST(FileSelection, RejectsOutOfRangeIndices) {
    const auto torrent = metadata({100});
    EXPECT_THROW(FileSelection(torrent, {1}), std::invalid_argument);
}

TEST(FileSelection, RejectsMissingPieceHashesInsteadOfCompletingAnEmptyMask) {
    const auto encoded = BencodeParser::encode(BencodeValue::Dictionary{
        {"info", BencodeValue::Dictionary{{"name", std::string("file")}, {"length", int64_t(1)},
            {"piece length", int64_t(16384)}, {"pieces", std::string{}}}}});
    const auto torrent = TorrentFile::fromData({encoded.begin(), encoded.end()});
    EXPECT_THROW((void)FileSelection(torrent), std::invalid_argument);
}
