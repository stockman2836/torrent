#include "piece_manager.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <set>

using namespace torrent;

void printBitfield(const std::vector<bool>& bitfield, const std::string& label) {
    std::cout << label << ": ";
    for (bool bit : bitfield) {
        std::cout << (bit ? "1" : "0");
    }
    std::cout << "\n";
}

void demonstratePieceSelection() {
    std::cout << "=== Piece Selection Strategy Example ===\n\n";

    // Create a piece manager for a 10-piece torrent
    const size_t num_pieces = 10;
    const size_t piece_length = 262144;  // 256KB
    const size_t total_length = num_pieces * piece_length;

    // Create dummy piece hashes (20 bytes per piece)
    std::vector<uint8_t> piece_hashes(num_pieces * 20, 0);

    PieceManager pm(num_pieces, piece_length, total_length, piece_hashes);

    // Simulate that we already have pieces 0, 1, 2
    pm.markPieceComplete(0);
    pm.markPieceComplete(1);
    pm.markPieceComplete(2);

    std::cout << "Our bitfield (we have pieces 0, 1, 2):\n";
    printBitfield(pm.getBitfield(), "We have");
    std::cout << "Progress: " << pm.percentComplete() << "%\n\n";

    // Simulate three peers with different pieces
    std::cout << "=== Peer Bitfields ===\n";

    // Peer 1: has pieces 3, 4, 5, 6 (common pieces)
    std::vector<bool> peer1_bitfield(num_pieces, false);
    peer1_bitfield[3] = peer1_bitfield[4] = peer1_bitfield[5] = peer1_bitfield[6] = true;
    printBitfield(peer1_bitfield, "Peer 1  ");

    // Peer 2: has pieces 3, 4, 5, 7 (also has common pieces 3,4,5)
    std::vector<bool> peer2_bitfield(num_pieces, false);
    peer2_bitfield[3] = peer2_bitfield[4] = peer2_bitfield[5] = peer2_bitfield[7] = true;
    printBitfield(peer2_bitfield, "Peer 2  ");

    // Peer 3: has pieces 8, 9 (rare pieces!)
    std::vector<bool> peer3_bitfield(num_pieces, false);
    peer3_bitfield[8] = peer3_bitfield[9] = true;
    printBitfield(peer3_bitfield, "Peer 3  ");

    std::cout << "\nRarity Analysis:\n";
    std::cout << "- Pieces 3,4,5: common (2 peers have them)\n";
    std::cout << "- Pieces 6,7: uncommon (1 peer each)\n";
    std::cout << "- Pieces 8,9: rare (1 peer has them)\n\n";

    // Collect all peer bitfields
    std::vector<std::vector<bool>> all_peer_bitfields = {
        peer1_bitfield, peer2_bitfield, peer3_bitfield
    };

    std::set<uint32_t> in_download;  // No pieces currently downloading

    // === Demonstrate Random-First Strategy ===
    std::cout << "=== 1. Random-First Strategy (first 4 pieces) ===\n";
    std::cout << "Used for: Initial pieces to quickly get something to share\n";
    std::cout << "This improves swarm health by giving us pieces to upload\n\n";

    for (int i = 0; i < 2; ++i) {
        int32_t piece = pm.getNextPieceRandomFirst(peer1_bitfield, in_download);
        if (piece >= 0) {
            std::cout << "Selected (random): piece #" << piece << "\n";
        }
    }

    std::cout << "\n";

    // === Demonstrate Rarest-First Strategy ===
    std::cout << "=== 2. Rarest-First Strategy ===\n";
    std::cout << "Used for: Main download phase (after random-first)\n";
    std::cout << "Prioritizes rare pieces to improve swarm availability\n\n";

    // From Peer 3 (has rare pieces 8, 9)
    std::cout << "Selecting from Peer 3 (has rare pieces 8, 9):\n";
    int32_t rarest = pm.getNextPieceRarestFirst(all_peer_bitfields, peer3_bitfield, in_download);
    if (rarest >= 0) {
        std::cout << "-> Should select piece 8 or 9 (rarest)\n";
        in_download.insert(rarest);  // Mark as downloading
    }

    std::cout << "\n";

    // From Peer 1 (has common pieces)
    std::cout << "Selecting from Peer 1 (has common pieces 3,4,5,6):\n";
    rarest = pm.getNextPieceRarestFirst(all_peer_bitfields, peer1_bitfield, in_download);
    if (rarest >= 0) {
        std::cout << "-> Should select piece 6 (less common than 3,4,5)\n";
        in_download.insert(rarest);  // Mark as downloading
    }

    std::cout << "\n";

    // === Demonstrate Sequential Strategy ===
    std::cout << "=== 3. Sequential Strategy ===\n";
    std::cout << "Used for: Streaming/preview (e.g., video files)\n";
    std::cout << "Downloads pieces in order from first to last\n\n";

    pm.setSequentialMode(true);
    in_download.clear();  // Reset

    for (int i = 0; i < 3; ++i) {
        int32_t piece = pm.getNextPieceSequential(peer1_bitfield, in_download);
        if (piece >= 0) {
            std::cout << "Selected (sequential): piece #" << piece << "\n";
            in_download.insert(piece);
        }
    }

    std::cout << "\n";

    // === Summary ===
    std::cout << "=== Strategy Comparison ===\n\n";

    std::cout << "Random-First:\n";
    std::cout << "  + Quick start, get pieces to share fast\n";
    std::cout << "  + Helps swarm health early on\n";
    std::cout << "  - Not optimal for overall download time\n";
    std::cout << "  Usage: First 4 pieces only\n\n";

    std::cout << "Rarest-First:\n";
    std::cout << "  + Improves swarm availability\n";
    std::cout << "  + Prevents rare pieces from disappearing\n";
    std::cout << "  + Optimal for overall swarm performance\n";
    std::cout << "  Usage: Main download phase (after random-first)\n\n";

    std::cout << "Sequential:\n";
    std::cout << "  + Enables streaming/preview of files\n";
    std::cout << "  + Predictable download order\n";
    std::cout << "  - Poor for swarm health\n";
    std::cout << "  - Slower overall completion\n";
    std::cout << "  Usage: User preference for streaming\n\n";

    std::cout << "=== Integrated Download Flow ===\n\n";
    std::cout << "1. Start: Random-first (pieces 0-3)\n";
    std::cout << "   -> Get something to share quickly\n\n";
    std::cout << "2. Main Phase: Rarest-first (pieces 4 to N-5)\n";
    std::cout << "   -> Optimal piece distribution\n\n";
    std::cout << "3. Endgame: Request remaining pieces from all peers\n";
    std::cout << "   -> Fast completion (< 5 pieces left)\n\n";

    std::cout << "Configuration:\n";
    std::cout << "- setSequentialMode(true/false): Enable sequential download\n";
    std::cout << "- random_first_pieces_: Number of random pieces (default: 4)\n";
}

int main() {
    try {
        demonstratePieceSelection();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
