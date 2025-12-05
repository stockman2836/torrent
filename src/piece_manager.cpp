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
    std::lock_guard<std::mutex> lock(mutex_);

    if (piece_index >= num_pieces_) {
        std::cerr << "Invalid piece index: " << piece_index << "\n";
        return false;
    }

    // Check if already have this piece
    if (bitfield_[piece_index]) {
        std::cout << "Piece " << piece_index << " already completed, ignoring block\n";
        return true;
    }

    // Get or create PieceInProgress
    auto it = pieces_in_progress_.find(piece_index);
    if (it == pieces_in_progress_.end()) {
        // Create new piece in progress
        size_t piece_size = piece_length_;
        if (piece_index == num_pieces_ - 1) {
            size_t last_piece_size = total_length_ % piece_length_;
            if (last_piece_size != 0) {
                piece_size = last_piece_size;
            }
        }

        size_t num_blocks = (piece_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
        auto piece = std::make_unique<PieceInProgress>(piece_index, piece_size, num_blocks);
        it = pieces_in_progress_.emplace(piece_index, std::move(piece)).first;

        std::cout << "Started assembling piece " << piece_index
                  << " (size=" << piece_size << " bytes, "
                  << num_blocks << " blocks)\n";
    }

    PieceInProgress* piece = it->second.get();

    // Calculate block index
    size_t block_index = offset / BLOCK_SIZE;
    if (block_index >= piece->total_blocks) {
        std::cerr << "Invalid block index " << block_index
                  << " for piece " << piece_index << "\n";
        return false;
    }

    // Check if already have this block
    if (piece->blocks_received[block_index]) {
        std::cout << "Block already received: piece=" << piece_index
                  << " offset=" << offset << "\n";
        return true;
    }

    // Copy block data into piece buffer
    if (offset + data.size() > piece->piece_size) {
        std::cerr << "Block data exceeds piece size\n";
        return false;
    }

    std::copy(data.begin(), data.end(), piece->data.begin() + offset);
    piece->blocks_received[block_index] = true;
    piece->blocks_downloaded++;

    std::cout << "Block received: piece=" << piece_index
              << " offset=" << offset
              << " size=" << data.size()
              << " (" << piece->blocks_downloaded << "/"
              << piece->total_blocks << " blocks, "
              << static_cast<int>(piece->progress() * 100) << "%)\n";

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

bool PieceManager::isPieceInProgress(uint32_t piece_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pieces_in_progress_.find(piece_index) != pieces_in_progress_.end();
}

PieceInProgress* PieceManager::getPieceInProgress(uint32_t piece_index) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pieces_in_progress_.find(piece_index);
    return (it != pieces_in_progress_.end()) ? it->second.get() : nullptr;
}

bool PieceManager::completePiece(uint32_t piece_index, FileManager* file_manager) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if piece is in progress
    auto it = pieces_in_progress_.find(piece_index);
    if (it == pieces_in_progress_.end()) {
        std::cerr << "Piece " << piece_index << " is not in progress\n";
        return false;
    }

    PieceInProgress* piece = it->second.get();

    // Check if all blocks received
    if (!piece->isComplete()) {
        std::cerr << "Piece " << piece_index << " is not complete yet ("
                  << piece->blocks_downloaded << "/" << piece->total_blocks << " blocks)\n";
        return false;
    }

    std::cout << "Piece " << piece_index << " assembly complete, verifying hash...\n";

    // Verify hash
    if (!verifyPiece(piece_index, piece->data)) {
        std::cerr << "ERROR: Piece " << piece_index << " hash verification FAILED!\n";
        std::cerr << "  Discarding piece and will re-request\n";

        // Remove from in-progress
        pieces_in_progress_.erase(it);
        return false;
    }

    std::cout << "Piece " << piece_index << " hash verification SUCCESS\n";

    // Write to disk
    if (file_manager && !file_manager->writePiece(piece_index, piece->data)) {
        std::cerr << "ERROR: Failed to write piece " << piece_index << " to disk\n";
        pieces_in_progress_.erase(it);
        return false;
    }

    std::cout << "Piece " << piece_index << " written to disk successfully\n";

    // Mark as complete
    if (!bitfield_[piece_index]) {
        bitfield_[piece_index] = true;
        pieces_downloaded_++;
    }

    // Remove from in-progress
    pieces_in_progress_.erase(it);

    std::cout << "✓ Piece " << piece_index << " COMPLETED! Progress: "
              << percentComplete() << "%\n";

    return true;
}

size_t PieceManager::numPiecesInProgress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pieces_in_progress_.size();
}

// Piece selection strategies

std::vector<int> PieceManager::calculatePieceRarity(
    const std::vector<std::vector<bool>>& all_peer_bitfields) const {

    std::vector<int> rarity(num_pieces_, 0);

    // Count how many peers have each piece
    for (const auto& bitfield : all_peer_bitfields) {
        for (size_t i = 0; i < std::min(bitfield.size(), num_pieces_); ++i) {
            if (bitfield[i]) {
                rarity[i]++;
            }
        }
    }

    return rarity;
}

int32_t PieceManager::getNextPieceRarestFirst(
    const std::vector<std::vector<bool>>& all_peer_bitfields,
    const std::vector<bool>& peer_has_pieces,
    const std::set<uint32_t>& in_download) {

    std::lock_guard<std::mutex> lock(mutex_);

    // Calculate rarity
    std::vector<int> rarity = calculatePieceRarity(all_peer_bitfields);

    // Find rarest piece that this peer has and we need
    int32_t best_piece = -1;
    int min_rarity = INT_MAX;

    for (size_t i = 0; i < num_pieces_; ++i) {
        if (!bitfield_[i] &&  // We don't have it
            pieces_in_progress_.find(i) == pieces_in_progress_.end() &&  // Not in assembly
            in_download.find(i) == in_download.end() &&  // Not being downloaded
            i < peer_has_pieces.size() && peer_has_pieces[i] &&  // Peer has it
            rarity[i] < min_rarity && rarity[i] > 0) {  // Rarer than current best

            min_rarity = rarity[i];
            best_piece = static_cast<int32_t>(i);
        }
    }

    if (best_piece >= 0) {
        std::cout << "Selected piece #" << best_piece << " (rarity: " << min_rarity << ")\n";
    }

    return best_piece;
}

int32_t PieceManager::getNextPieceRandomFirst(
    const std::vector<bool>& peer_has_pieces,
    const std::set<uint32_t>& in_download) {

    std::lock_guard<std::mutex> lock(mutex_);

    // Check if we should still do random-first
    if (pieces_downloaded_ >= random_first_pieces_) {
        return -1;  // Switch to rarest-first
    }

    // Collect available pieces
    std::vector<uint32_t> available;
    for (size_t i = 0; i < num_pieces_; ++i) {
        if (!bitfield_[i] &&
            pieces_in_progress_.find(i) == pieces_in_progress_.end() &&
            in_download.find(i) == in_download.end() &&
            i < peer_has_pieces.size() && peer_has_pieces[i]) {
            available.push_back(i);
        }
    }

    if (available.empty()) {
        return -1;
    }

    // Select random piece
    size_t random_idx = rand() % available.size();
    int32_t selected = available[random_idx];

    std::cout << "Selected piece #" << selected << " (random-first mode)\n";
    return selected;
}

int32_t PieceManager::getNextPieceSequential(
    const std::vector<bool>& peer_has_pieces,
    const std::set<uint32_t>& in_download) {

    std::lock_guard<std::mutex> lock(mutex_);

    // Select first piece that we need and peer has
    for (size_t i = 0; i < num_pieces_; ++i) {
        if (!bitfield_[i] &&
            pieces_in_progress_.find(i) == pieces_in_progress_.end() &&
            in_download.find(i) == in_download.end() &&
            i < peer_has_pieces.size() && peer_has_pieces[i]) {
            return static_cast<int32_t>(i);
        }
    }

    return -1;
}

} // namespace torrent
