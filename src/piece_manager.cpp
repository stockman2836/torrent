#include "piece_manager.h"
#include "utils.h"
#include <algorithm>

namespace torrent {

PieceManager::PieceManager(size_t num_pieces,
                          size_t piece_length,
                          size_t total_length,
                          const std::vector<uint8_t>& piece_hashes)
    : num_pieces_(num_pieces)
    , piece_length_(piece_length)
    , total_length_(total_length)
    , piece_hashes_(piece_hashes)
    , bitfield_(num_pieces, false)
    , pieces_downloaded_(0) {
}

bool PieceManager::hasPiece(uint32_t piece_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (piece_index >= num_pieces_) {
        return false;
    }
    return bitfield_[piece_index];
}

void PieceManager::markPieceComplete(uint32_t piece_index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (piece_index < num_pieces_ && !bitfield_[piece_index]) {
        bitfield_[piece_index] = true;
        pieces_downloaded_++;
    }
}

int32_t PieceManager::getNextPiece(const std::vector<bool>& peer_has_pieces) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Simple strategy: sequential download
    // TODO: Implement rarest-first or random-first strategies
    for (size_t i = 0; i < num_pieces_; ++i) {
        if (!bitfield_[i] && i < peer_has_pieces.size() && peer_has_pieces[i]) {
            return static_cast<int32_t>(i);
        }
    }

    return -1;  // No piece to download
}

std::vector<Block> PieceManager::getBlocksForPiece(uint32_t piece_index) {
    std::vector<Block> blocks;

    if (piece_index >= num_pieces_) {
        return blocks;
    }

    // Calculate piece size (last piece might be smaller)
    size_t piece_size = piece_length_;
    if (piece_index == num_pieces_ - 1) {
        size_t last_piece_size = total_length_ % piece_length_;
        if (last_piece_size != 0) {
            piece_size = last_piece_size;
        }
    }

    // Split piece into blocks
    size_t offset = 0;
    while (offset < piece_size) {
        size_t block_size = std::min(BLOCK_SIZE, piece_size - offset);
        blocks.emplace_back(piece_index, offset, block_size);
        offset += block_size;
    }

    return blocks;
}

bool PieceManager::addBlock(uint32_t piece_index, uint32_t offset, const std::vector<uint8_t>& data) {
    // TODO: Store block data and check if piece is complete
    // For now, just a stub
    return true;
}

bool PieceManager::verifyPiece(uint32_t piece_index, const std::vector<uint8_t>& data) const {
    if (piece_index >= num_pieces_) {
        return false;
    }

    // Get expected hash from piece_hashes_
    size_t hash_offset = piece_index * 20;  // SHA1 is 20 bytes
    if (hash_offset + 20 > piece_hashes_.size()) {
        return false;
    }

    std::vector<uint8_t> expected_hash(
        piece_hashes_.begin() + hash_offset,
        piece_hashes_.begin() + hash_offset + 20
    );

    // Calculate actual hash
    std::vector<uint8_t> actual_hash = utils::sha1(data);

    // Compare
    return expected_hash == actual_hash;
}

double PieceManager::percentComplete() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (num_pieces_ == 0) {
        return 0.0;
    }
    return (static_cast<double>(pieces_downloaded_) / num_pieces_) * 100.0;
}

std::vector<bool> PieceManager::getBitfield() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bitfield_;
}

} // namespace torrent
