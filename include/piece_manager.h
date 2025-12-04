#pragma once

#include <vector>
#include <cstdint>
#include <mutex>
#include <bitset>

namespace torrent {

// Block size (typically 16KB)
constexpr size_t BLOCK_SIZE = 16384;

struct Block {
    uint32_t piece_index;
    uint32_t offset;
    uint32_t length;
    std::vector<uint8_t> data;
    bool downloaded;

    Block(uint32_t pi, uint32_t off, uint32_t len)
        : piece_index(pi), offset(off), length(len), downloaded(false) {}
};

class PieceManager {
public:
    PieceManager(size_t num_pieces,
                 size_t piece_length,
                 size_t total_length,
                 const std::vector<uint8_t>& piece_hashes);

    // Piece management
    bool hasPiece(uint32_t piece_index) const;
    void markPieceComplete(uint32_t piece_index);

    // Get next piece to download
    int32_t getNextPiece(const std::vector<bool>& peer_has_pieces);

    // Block management
    std::vector<Block> getBlocksForPiece(uint32_t piece_index);
    bool addBlock(uint32_t piece_index, uint32_t offset, const std::vector<uint8_t>& data);

    // Verification
    bool verifyPiece(uint32_t piece_index, const std::vector<uint8_t>& data) const;

    // Progress
    size_t numPiecesDownloaded() const { return pieces_downloaded_; }
    double percentComplete() const;
    std::vector<bool> getBitfield() const { return bitfield_; }

private:
    size_t num_pieces_;
    size_t piece_length_;
    size_t total_length_;
    std::vector<uint8_t> piece_hashes_;  // SHA1 hashes

    std::vector<bool> bitfield_;  // Which pieces we have
    size_t pieces_downloaded_;

    mutable std::mutex mutex_;
};

} // namespace torrent
