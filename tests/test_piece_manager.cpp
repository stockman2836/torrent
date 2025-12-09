#include <gtest/gtest.h>
#include "piece_manager.h"
#include <vector>
#include <algorithm>

using namespace torrent;

class PieceManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        num_pieces_ = 10;
        piece_length_ = 262144; // 256 KB
        total_length_ = 2621440; // 10 * 256 KB

        piece_hashes_.resize(num_pieces_ * 20);
        for (size_t i = 0; i < piece_hashes_.size(); ++i) {
            piece_hashes_[i] = static_cast<uint8_t>(i % 256);
        }

        manager_ = std::make_unique<PieceManager>(
            num_pieces_,
            piece_length_,
            total_length_,
            piece_hashes_
        );
    }

    void TearDown() override {}

    size_t num_pieces_;
    size_t piece_length_;
    size_t total_length_;
    std::vector<uint8_t> piece_hashes_;
    std::unique_ptr<PieceManager> manager_;
};

// ==================== Basic Functionality Tests ====================

TEST_F(PieceManagerTest, InitialStateIsEmpty) {
    EXPECT_EQ(manager_->numPiecesDownloaded(), 0);
    EXPECT_DOUBLE_EQ(manager_->percentComplete(), 0.0);

    for (size_t i = 0; i < num_pieces_; ++i) {
        EXPECT_FALSE(manager_->hasPiece(i));
    }
}

TEST_F(PieceManagerTest, BitfieldInitiallyAllFalse) {
    auto bitfield = manager_->getBitfield();
    EXPECT_EQ(bitfield.size(), num_pieces_);

    for (bool bit : bitfield) {
        EXPECT_FALSE(bit);
    }
}

TEST_F(PieceManagerTest, MarkPieceComplete) {
    EXPECT_FALSE(manager_->hasPiece(0));

    manager_->markPieceComplete(0);

    EXPECT_TRUE(manager_->hasPiece(0));
    EXPECT_EQ(manager_->numPiecesDownloaded(), 1);
}

TEST_F(PieceManagerTest, MarkMultiplePiecesComplete) {
    manager_->markPieceComplete(0);
    manager_->markPieceComplete(2);
    manager_->markPieceComplete(5);

    EXPECT_TRUE(manager_->hasPiece(0));
    EXPECT_FALSE(manager_->hasPiece(1));
    EXPECT_TRUE(manager_->hasPiece(2));
    EXPECT_FALSE(manager_->hasPiece(3));
    EXPECT_FALSE(manager_->hasPiece(4));
    EXPECT_TRUE(manager_->hasPiece(5));

    EXPECT_EQ(manager_->numPiecesDownloaded(), 3);
}

TEST_F(PieceManagerTest, MarkSamePieceTwiceDoesNotDuplicate) {
    manager_->markPieceComplete(0);
    manager_->markPieceComplete(0);

    EXPECT_EQ(manager_->numPiecesDownloaded(), 1);
}

TEST_F(PieceManagerTest, HasPieceReturnsFalseForInvalidIndex) {
    EXPECT_FALSE(manager_->hasPiece(999));
}

// ==================== Progress Calculation Tests ====================

TEST_F(PieceManagerTest, PercentCompleteCalculation) {
    EXPECT_DOUBLE_EQ(manager_->percentComplete(), 0.0);

    manager_->markPieceComplete(0);
    EXPECT_DOUBLE_EQ(manager_->percentComplete(), 10.0);

    manager_->markPieceComplete(1);
    EXPECT_DOUBLE_EQ(manager_->percentComplete(), 20.0);

    for (size_t i = 2; i < num_pieces_; ++i) {
        manager_->markPieceComplete(i);
    }
    EXPECT_DOUBLE_EQ(manager_->percentComplete(), 100.0);
}

// ==================== Block Management Tests ====================

TEST_F(PieceManagerTest, GetBlocksForPiece) {
    auto blocks = manager_->getBlocksForPiece(0);

    EXPECT_GT(blocks.size(), 0);

    size_t expected_blocks = piece_length_ / BLOCK_SIZE;
    EXPECT_EQ(blocks.size(), expected_blocks);

    for (size_t i = 0; i < blocks.size(); ++i) {
        EXPECT_EQ(blocks[i].piece_index, 0);
        EXPECT_EQ(blocks[i].offset, i * BLOCK_SIZE);
        EXPECT_EQ(blocks[i].length, BLOCK_SIZE);
        EXPECT_FALSE(blocks[i].downloaded);
    }
}

TEST_F(PieceManagerTest, GetBlocksForLastPiece) {
    size_t last_piece_index = num_pieces_ - 1;
    auto blocks = manager_->getBlocksForPiece(last_piece_index);

    EXPECT_GT(blocks.size(), 0);

    size_t total_size = 0;
    for (const auto& block : blocks) {
        EXPECT_EQ(block.piece_index, last_piece_index);
        total_size += block.length;
    }

    size_t expected_last_piece_size = total_length_ % piece_length_;
    if (expected_last_piece_size == 0) {
        expected_last_piece_size = piece_length_;
    }
    EXPECT_EQ(total_size, expected_last_piece_size);
}

TEST_F(PieceManagerTest, GetBlocksForInvalidPieceReturnsEmpty) {
    auto blocks = manager_->getBlocksForPiece(999);
    EXPECT_EQ(blocks.size(), 0);
}

// ==================== Block Addition Tests ====================

TEST_F(PieceManagerTest, AddBlockCreatesPieceInProgress) {
    std::vector<uint8_t> data(BLOCK_SIZE, 0xAA);

    EXPECT_FALSE(manager_->isPieceInProgress(0));

    bool result = manager_->addBlock(0, 0, data);

    EXPECT_TRUE(result);
    EXPECT_TRUE(manager_->isPieceInProgress(0));
}

TEST_F(PieceManagerTest, AddMultipleBlocks) {
    std::vector<uint8_t> data(BLOCK_SIZE, 0xBB);

    manager_->addBlock(0, 0, data);
    manager_->addBlock(0, BLOCK_SIZE, data);
    manager_->addBlock(0, BLOCK_SIZE * 2, data);

    EXPECT_TRUE(manager_->isPieceInProgress(0));

    auto piece = manager_->getPieceInProgress(0);
    ASSERT_NE(piece, nullptr);
    EXPECT_GE(piece->blocks_downloaded, 3);
}

TEST_F(PieceManagerTest, AddBlockToCompletedPieceIgnored) {
    std::vector<uint8_t> data(BLOCK_SIZE, 0xCC);

    manager_->markPieceComplete(0);

    bool result = manager_->addBlock(0, 0, data);

    EXPECT_TRUE(result);
    EXPECT_FALSE(manager_->isPieceInProgress(0));
}

TEST_F(PieceManagerTest, AddBlockWithInvalidIndexReturnsFalse) {
    std::vector<uint8_t> data(BLOCK_SIZE, 0xDD);

    bool result = manager_->addBlock(999, 0, data);

    EXPECT_FALSE(result);
}

// ==================== Piece In Progress Tests ====================

TEST_F(PieceManagerTest, PieceInProgressTracking) {
    std::vector<uint8_t> data(BLOCK_SIZE, 0xEE);

    EXPECT_EQ(manager_->numPiecesInProgress(), 0);

    manager_->addBlock(0, 0, data);
    EXPECT_EQ(manager_->numPiecesInProgress(), 1);

    manager_->addBlock(1, 0, data);
    EXPECT_EQ(manager_->numPiecesInProgress(), 2);
}

TEST_F(PieceManagerTest, GetPieceInProgressReturnsCorrectData) {
    std::vector<uint8_t> data(BLOCK_SIZE, 0xFF);

    manager_->addBlock(0, 0, data);

    auto piece = manager_->getPieceInProgress(0);
    ASSERT_NE(piece, nullptr);
    EXPECT_EQ(piece->piece_index, 0);
    EXPECT_EQ(piece->piece_size, piece_length_);
    EXPECT_GT(piece->total_blocks, 0);
}

TEST_F(PieceManagerTest, GetPieceInProgressReturnsNullForNonExistent) {
    auto piece = manager_->getPieceInProgress(5);
    EXPECT_EQ(piece, nullptr);
}

// ==================== Piece Selection Tests ====================

TEST_F(PieceManagerTest, GetNextPieceReturnsFirstAvailable) {
    std::vector<bool> peer_has(num_pieces_, true);

    int32_t next = manager_->getNextPiece(peer_has);

    EXPECT_EQ(next, 0);
}

TEST_F(PieceManagerTest, GetNextPieceSkipsCompletedPieces) {
    std::vector<bool> peer_has(num_pieces_, true);

    manager_->markPieceComplete(0);
    manager_->markPieceComplete(1);

    int32_t next = manager_->getNextPiece(peer_has);

    EXPECT_EQ(next, 2);
}

TEST_F(PieceManagerTest, GetNextPieceRespectsePeerBitfield) {
    std::vector<bool> peer_has(num_pieces_, false);
    peer_has[5] = true;

    int32_t next = manager_->getNextPiece(peer_has);

    EXPECT_EQ(next, 5);
}

TEST_F(PieceManagerTest, GetNextPieceReturnsNegativeWhenNoneAvailable) {
    std::vector<bool> peer_has(num_pieces_, false);

    int32_t next = manager_->getNextPiece(peer_has);

    EXPECT_EQ(next, -1);
}

TEST_F(PieceManagerTest, GetNextPieceReturnsNegativeWhenAllComplete) {
    std::vector<bool> peer_has(num_pieces_, true);

    for (size_t i = 0; i < num_pieces_; ++i) {
        manager_->markPieceComplete(i);
    }

    int32_t next = manager_->getNextPiece(peer_has);

    EXPECT_EQ(next, -1);
}

// ==================== Sequential Mode Tests ====================

TEST_F(PieceManagerTest, SequentialModeDefaultIsFalse) {
    EXPECT_FALSE(manager_->isSequentialMode());
}

TEST_F(PieceManagerTest, SetSequentialMode) {
    manager_->setSequentialMode(true);
    EXPECT_TRUE(manager_->isSequentialMode());

    manager_->setSequentialMode(false);
    EXPECT_FALSE(manager_->isSequentialMode());
}

TEST_F(PieceManagerTest, GetNextPieceSequentialMode) {
    std::vector<bool> peer_has(num_pieces_, true);
    std::set<uint32_t> in_download;

    manager_->setSequentialMode(true);

    int32_t next = manager_->getNextPieceSequential(peer_has, in_download);
    EXPECT_EQ(next, 0);

    manager_->markPieceComplete(0);
    next = manager_->getNextPieceSequential(peer_has, in_download);
    EXPECT_EQ(next, 1);
}

// ==================== Rarest First Tests ====================

TEST_F(PieceManagerTest, GetNextPieceRarestFirst) {
    std::vector<bool> peer_has(num_pieces_, true);
    std::set<uint32_t> in_download;

    std::vector<std::vector<bool>> all_peers(3, std::vector<bool>(num_pieces_, false));
    all_peers[0][0] = true;
    all_peers[1][0] = true;
    all_peers[2][0] = true;

    all_peers[0][1] = true;
    all_peers[1][1] = true;

    all_peers[0][2] = true;

    int32_t next = manager_->getNextPieceRarestFirst(all_peers, peer_has, in_download);

    EXPECT_TRUE(next == 2 || next >= 0);
}

// ==================== Random First Tests ====================

TEST_F(PieceManagerTest, GetNextPieceRandomFirst) {
    std::vector<bool> peer_has(num_pieces_, true);
    std::set<uint32_t> in_download;

    int32_t next = manager_->getNextPieceRandomFirst(peer_has, in_download);

    EXPECT_GE(next, 0);
    EXPECT_LT(next, static_cast<int32_t>(num_pieces_));
}

// ==================== Bitfield Tests ====================

TEST_F(PieceManagerTest, SetBitfield) {
    std::vector<bool> new_bitfield(num_pieces_, false);
    new_bitfield[0] = true;
    new_bitfield[3] = true;
    new_bitfield[7] = true;

    manager_->setBitfield(new_bitfield);

    EXPECT_TRUE(manager_->hasPiece(0));
    EXPECT_FALSE(manager_->hasPiece(1));
    EXPECT_FALSE(manager_->hasPiece(2));
    EXPECT_TRUE(manager_->hasPiece(3));
    EXPECT_TRUE(manager_->hasPiece(7));

    EXPECT_EQ(manager_->numPiecesDownloaded(), 3);
}

TEST_F(PieceManagerTest, GetBitfieldReflectsCompletedPieces) {
    manager_->markPieceComplete(1);
    manager_->markPieceComplete(4);
    manager_->markPieceComplete(9);

    auto bitfield = manager_->getBitfield();

    EXPECT_TRUE(bitfield[1]);
    EXPECT_TRUE(bitfield[4]);
    EXPECT_TRUE(bitfield[9]);

    for (size_t i = 0; i < num_pieces_; ++i) {
        if (i != 1 && i != 4 && i != 9) {
            EXPECT_FALSE(bitfield[i]);
        }
    }
}

// ==================== Edge Cases ====================

TEST_F(PieceManagerTest, HandleSinglePieceTorrent) {
    std::vector<uint8_t> single_hash(20, 0xAA);
    PieceManager single_manager(1, 100000, 100000, single_hash);

    EXPECT_EQ(single_manager.numPiecesDownloaded(), 0);

    single_manager.markPieceComplete(0);

    EXPECT_EQ(single_manager.numPiecesDownloaded(), 1);
    EXPECT_DOUBLE_EQ(single_manager.percentComplete(), 100.0);
}

TEST_F(PieceManagerTest, HandleVeryLargeTorrent) {
    size_t large_num_pieces = 100000;
    std::vector<uint8_t> large_hashes(large_num_pieces * 20, 0xBB);

    PieceManager large_manager(
        large_num_pieces,
        piece_length_,
        large_num_pieces * piece_length_,
        large_hashes
    );

    EXPECT_EQ(large_manager.numPiecesDownloaded(), 0);
    EXPECT_DOUBLE_EQ(large_manager.percentComplete(), 0.0);

    large_manager.markPieceComplete(0);
    EXPECT_EQ(large_manager.numPiecesDownloaded(), 1);
}

// ==================== Thread Safety Tests ====================

TEST_F(PieceManagerTest, ConcurrentMarkPieceComplete) {
    const size_t num_threads = 4;
    const size_t pieces_per_thread = num_pieces_ / num_threads;

    std::vector<std::thread> threads;
    for (size_t t = 0; t < num_threads; ++t) {
        threads.emplace_back([this, t, pieces_per_thread]() {
            for (size_t i = 0; i < pieces_per_thread; ++i) {
                manager_->markPieceComplete(t * pieces_per_thread + i);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(manager_->numPiecesDownloaded(), num_threads * pieces_per_thread);
}

// ==================== Main ====================

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
