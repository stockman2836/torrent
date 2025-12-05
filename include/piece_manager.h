#pragma once

#include <vector>
#include <cstdint>
#include <mutex>
#include <bitset>
#include <map>
#include <memory>

namespace torrent {

// Forward declarations
class FileManager;

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

// In-progress piece assembly
struct PieceInProgress {
    uint32_t piece_index;
    size_t piece_size;
    std::vector<uint8_t> data;  // Full piece data buffer
    std::vector<bool> blocks_received;  // Track which blocks received
    size_t blocks_downloaded;
    size_t total_blocks;

    PieceInProgress(uint32_t index, size_t size, size_t num_blocks)
        : piece_index(index), piece_size(size), data(size, 0),
          blocks_received(num_blocks, false), blocks_downloaded(0),
          total_blocks(num_blocks) {}

    bool isComplete() const { return blocks_downloaded == total_blocks; }
    float progress() const { return total_blocks > 0 ?
        static_cast<float>(blocks_downloaded) / total_blocks : 0.0f; }
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

    // Piece assembly
    bool isPieceInProgress(uint32_t piece_index) const;
    PieceInProgress* getPieceInProgress(uint32_t piece_index);
    bool completePiece(uint32_t piece_index, FileManager* file_manager);

    // Verification
    bool verifyPiece(uint32_t piece_index, const std::vector<uint8_t>& data) const;

    // Progress
    size_t numPiecesDownloaded() const { return pieces_downloaded_; }
    double percentComplete() const;
    std::vector<bool> getBitfield() const { return bitfield_; }
    size_t numPiecesInProgress() const;

private:
    size_t num_pieces_;
    size_t piece_length_;
    size_t total_length_;
    std::vector<uint8_t> piece_hashes_;  // SHA1 hashes

    std::vector<bool> bitfield_;  // Which pieces we have
    size_t pieces_downloaded_;

    // Pieces in progress (being assembled)
    std::map<uint32_t, std::unique_ptr<PieceInProgress>> pieces_in_progress_;

    mutable std::mutex mutex_;
};

} // namespace torrent
