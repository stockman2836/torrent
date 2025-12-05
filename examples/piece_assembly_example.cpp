// Example: Piece Reception & Assembly
// This demonstrates Task 2.4 implementation

#include "piece_manager.h"
#include "file_manager.h"
#include "torrent_file.h"
#include <iostream>

using namespace torrent;

void example_piece_assembly() {
    std::cout << "=== Piece Assembly Example ===\n\n";

    // Setup
    size_t num_pieces = 5;
    size_t piece_length = 262144; // 256KB
    size_t total_length = num_pieces * piece_length;

    // Create dummy piece hashes (in real case, from torrent file)
    std::vector<uint8_t> piece_hashes(num_pieces * 20, 0);

    PieceManager piece_manager(num_pieces, piece_length, total_length, piece_hashes);

    std::cout << "PieceManager initialized:\n";
    std::cout << "  Total pieces: " << num_pieces << "\n";
    std::cout << "  Piece length: " << piece_length << " bytes\n";
    std::cout << "  Total length: " << total_length << " bytes\n\n";

    // Simulate downloading piece #2
    uint32_t piece_index = 2;

    std::cout << "=== Downloading Piece #" << piece_index << " ===\n\n";

    // Get blocks for this piece
    std::vector<Block> blocks = piece_manager.getBlocksForPiece(piece_index);

    std::cout << "Piece #" << piece_index << " split into " << blocks.size() << " blocks:\n";
    for (size_t i = 0; i < blocks.size(); ++i) {
        std::cout << "  Block " << i << ": offset=" << blocks[i].offset
                  << " length=" << blocks[i].length << " bytes\n";
    }
    std::cout << "\n";

    // Simulate receiving blocks
    std::cout << "=== Receiving Blocks ===\n\n";

    for (size_t i = 0; i < blocks.size(); ++i) {
        // Create dummy block data
        std::vector<uint8_t> block_data(blocks[i].length, static_cast<uint8_t>(i));

        std::cout << "Block " << i << " received...\n";

        // Add block to piece manager
        bool success = piece_manager.addBlock(
            blocks[i].piece_index,
            blocks[i].offset,
            block_data
        );

        if (success) {
            std::cout << "  ✓ Block added successfully\n";

            // Check piece progress
            if (piece_manager.isPieceInProgress(piece_index)) {
                PieceInProgress* piece = piece_manager.getPieceInProgress(piece_index);
                if (piece) {
                    std::cout << "  Progress: " << piece->blocks_downloaded << "/"
                              << piece->total_blocks << " blocks ("
                              << static_cast<int>(piece->progress() * 100) << "%)\n";

                    if (piece->isComplete()) {
                        std::cout << "\n*** ALL BLOCKS RECEIVED ***\n";
                        std::cout << "Piece #" << piece_index << " is ready for verification\n\n";
                    }
                }
            }
        } else {
            std::cerr << "  ✗ Failed to add block\n";
        }

        std::cout << "\n";
    }

    // Check piece completion
    std::cout << "=== Piece Verification ===\n\n";

    if (piece_manager.isPieceInProgress(piece_index)) {
        PieceInProgress* piece = piece_manager.getPieceInProgress(piece_index);

        if (piece && piece->isComplete()) {
            std::cout << "Piece #" << piece_index << " complete!\n";
            std::cout << "  Size: " << piece->piece_size << " bytes\n";
            std::cout << "  Blocks: " << piece->total_blocks << "\n";
            std::cout << "  Ready for hash verification and disk write\n\n";

            // In real code, would call:
            // bool success = piece_manager.completePiece(piece_index, file_manager);

            std::cout << "After completePiece() would:\n";
            std::cout << "  1. Verify SHA1 hash\n";
            std::cout << "  2. Write to disk via FileManager\n";
            std::cout << "  3. Update bitfield\n";
            std::cout << "  4. Return success/failure\n";
        }
    }
}

void example_hash_verification_failure() {
    std::cout << "\n\n=== Hash Verification Failure Example ===\n\n";

    size_t num_pieces = 3;
    size_t piece_length = 16384; // 16KB for simplicity
    size_t total_length = num_pieces * piece_length;
    std::vector<uint8_t> piece_hashes(num_pieces * 20, 0);

    PieceManager piece_manager(num_pieces, piece_length, total_length, piece_hashes);

    uint32_t piece_index = 1;
    std::cout << "Simulating download of piece #" << piece_index << "\n\n";

    // Get blocks
    auto blocks = piece_manager.getBlocksForPiece(piece_index);

    // Add all blocks (with dummy data)
    for (const auto& block : blocks) {
        std::vector<uint8_t> data(block.length, 0xFF);
        piece_manager.addBlock(block.piece_index, block.offset, data);
    }

    std::cout << "All blocks received\n";
    std::cout << "Attempting to complete piece...\n\n";

    // Attempt to complete (will fail hash verification in real scenario)
    // bool success = piece_manager.completePiece(piece_index, nullptr);

    std::cout << "If hash verification fails:\n";
    std::cout << "  1. Piece is discarded from in-progress map\n";
    std::cout << "  2. Bitfield remains unchanged\n";
    std::cout << "  3. Piece can be re-requested from peers\n";
    std::cout << "  4. No data written to disk\n";
}

void example_multiple_pieces() {
    std::cout << "\n\n=== Multiple Pieces In Progress ===\n\n";

    size_t num_pieces = 10;
    size_t piece_length = 262144;
    size_t total_length = num_pieces * piece_length;
    std::vector<uint8_t> piece_hashes(num_pieces * 20, 0);

    PieceManager piece_manager(num_pieces, piece_length, total_length, piece_hashes);

    std::cout << "Starting downloads for multiple pieces...\n\n";

    // Start downloading pieces 0, 2, 5
    std::vector<uint32_t> downloading = {0, 2, 5};

    for (uint32_t idx : downloading) {
        auto blocks = piece_manager.getBlocksForPiece(idx);

        // Simulate receiving first 2 blocks
        for (size_t i = 0; i < std::min(size_t(2), blocks.size()); ++i) {
            std::vector<uint8_t> data(blocks[i].length, 0);
            piece_manager.addBlock(blocks[i].piece_index, blocks[i].offset, data);
        }

        std::cout << "Piece #" << idx << ": started (2 blocks received)\n";
    }

    std::cout << "\nPieces in progress: " << piece_manager.numPiecesInProgress() << "\n";

    std::cout << "\nStatus of each piece:\n";
    for (uint32_t idx : downloading) {
        if (piece_manager.isPieceInProgress(idx)) {
            PieceInProgress* piece = piece_manager.getPieceInProgress(idx);
            if (piece) {
                std::cout << "  Piece #" << idx << ": "
                          << piece->blocks_downloaded << "/"
                          << piece->total_blocks << " blocks ("
                          << static_cast<int>(piece->progress() * 100) << "%)\n";
            }
        }
    }

    std::cout << "\nThis allows parallel downloading from multiple peers!\n";
}

void example_broadcast_have() {
    std::cout << "\n\n=== Broadcasting HAVE Messages ===\n\n";

    std::cout << "When a piece is completed:\n\n";

    std::cout << "1. Piece is verified and written to disk\n";
    std::cout << "2. DownloadManager::broadcastHave() is called\n";
    std::cout << "3. HAVE message sent to all connected peers:\n";
    std::cout << "   - Iterates through connections vector\n";
    std::cout << "   - Calls connection->sendHave(piece_index)\n";
    std::cout << "   - Thread-safe with mutex\n\n";

    std::cout << "This tells other peers we have the piece\n";
    std::cout << "They can now request it from us!\n";

    std::cout << "\nExample output:\n";
    std::cout << "  Broadcasting HAVE message for piece 5 to 3 peers\n";
    std::cout << "  HAVE message sent to 3 peers\n";
}

int main() {
    std::cout << "Piece Reception & Assembly Examples\n";
    std::cout << "====================================\n\n";

    // Example 1: Basic piece assembly
    example_piece_assembly();

    // Example 2: Hash verification failure
    example_hash_verification_failure();

    // Example 3: Multiple pieces
    example_multiple_pieces();

    // Example 4: Broadcasting HAVE
    example_broadcast_have();

    std::cout << "\n\nAll examples completed!\n";
    return 0;
}
