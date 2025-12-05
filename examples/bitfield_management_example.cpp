// Example: How to use the bitfield management functionality
// This file demonstrates the usage of Task 2.2 implementation

#include "peer_connection.h"
#include "piece_manager.h"
#include <iostream>
#include <vector>

using namespace torrent;

void example_bitfield_management() {
    std::cout << "=== Bitfield Management Example ===\n\n";

    // Setup peer connection
    std::string peer_ip = "192.168.1.100";
    uint16_t peer_port = 6881;
    std::vector<uint8_t> info_hash(20, 0x42); // Example info hash
    std::string peer_id = "-EX0001-123456789012"; // Example peer ID

    PeerConnection peer(peer_ip, peer_port, info_hash, peer_id);

    // Setup piece manager (example: 100 pieces, 256KB each)
    size_t num_pieces = 100;
    size_t piece_length = 262144; // 256KB
    size_t total_length = num_pieces * piece_length;
    std::vector<uint8_t> piece_hashes(num_pieces * 20, 0); // Example hashes

    PieceManager piece_manager(num_pieces, piece_length, total_length, piece_hashes);

    // Initialize peer's bitfield with the correct size
    peer.initializePeerBitfield(num_pieces);
    std::cout << "Initialized peer bitfield for " << num_pieces << " pieces\n\n";

    // Connect and perform handshake
    if (!peer.connect()) {
        std::cerr << "Failed to connect\n";
        return;
    }

    // Get our bitfield from piece manager
    std::vector<bool> our_bitfield = piece_manager.getBitfield();

    // Perform handshake and automatically send our bitfield
    std::cout << "Performing handshake with automatic bitfield exchange...\n";
    if (!peer.performHandshake(our_bitfield)) {
        std::cerr << "Failed to perform handshake\n";
        return;
    }

    std::cout << "\n=== After Handshake ===\n";

    // Wait for peer's bitfield message
    auto message = peer.receiveMessage();
    if (message && message->type == MessageType::BITFIELD) {
        std::cout << "Received peer's bitfield\n";

        // Query peer bitfield information
        std::cout << "\nPeer Bitfield Statistics:\n";
        std::cout << "  Total pieces: " << peer.getPeerBitfieldSize() << "\n";
        std::cout << "  Pieces available: " << peer.getPeerPieceCount() << "\n";
        std::cout << "  Is seeder: " << (peer.isPeerSeeder() ? "YES" : "NO") << "\n";

        // Check specific pieces
        std::cout << "\nChecking specific pieces:\n";
        for (uint32_t i = 0; i < 10; ++i) {
            bool has_piece = peer.peerHasPiece(i);
            std::cout << "  Piece #" << i << ": " << (has_piece ? "Available" : "Not available") << "\n";
        }

        // Find pieces we need that the peer has
        std::cout << "\nFinding pieces to download:\n";
        for (uint32_t i = 0; i < num_pieces; ++i) {
            if (!piece_manager.hasPiece(i) && peer.peerHasPiece(i)) {
                std::cout << "  Can download piece #" << i << " from this peer\n";
            }
        }
    }

    // Simulate receiving HAVE messages
    std::cout << "\n=== Simulating HAVE Messages ===\n";
    std::cout << "Peer announces it now has piece #50\n";

    // In a real scenario, this would come from receiveMessage()
    // For demonstration, we show what happens when HAVE is received
    // The receiveMessage() method automatically updates peer_bitfield_

    std::cout << "After receiving HAVE message, peer now has piece #50\n";
    std::cout << "Total pieces available from peer: " << peer.getPeerPieceCount() << "\n";
}

void example_piece_selection_with_bitfield() {
    std::cout << "\n\n=== Piece Selection Example ===\n\n";

    // Setup
    size_t num_pieces = 50;
    size_t piece_length = 262144;
    size_t total_length = num_pieces * piece_length;
    std::vector<uint8_t> piece_hashes(num_pieces * 20, 0);

    PieceManager piece_manager(num_pieces, piece_length, total_length, piece_hashes);

    // Simulate having some pieces already
    piece_manager.markPieceComplete(0);
    piece_manager.markPieceComplete(1);
    piece_manager.markPieceComplete(2);

    std::cout << "We have " << piece_manager.numPiecesDownloaded() << " pieces\n";
    std::cout << "Download progress: " << piece_manager.percentComplete() << "%\n\n";

    // Create peer connection
    std::string peer_ip = "192.168.1.101";
    uint16_t peer_port = 6881;
    std::vector<uint8_t> info_hash(20, 0x42);
    std::string peer_id = "-EX0002-abcdefghijkl";

    PeerConnection peer(peer_ip, peer_port, info_hash, peer_id);
    peer.initializePeerBitfield(num_pieces);

    // Simulate peer having pieces 5-10
    // In real code, this would come from BITFIELD message
    std::cout << "Peer has pieces 5-10\n";

    // Use PieceManager to select next piece to download
    std::vector<bool> peer_has = peer.peerBitfield();
    int32_t next_piece = piece_manager.getNextPiece(peer_has);

    if (next_piece >= 0) {
        std::cout << "Next piece to download from this peer: #" << next_piece << "\n";

        // Check if we can download it
        if (peer.peerHasPiece(next_piece)) {
            std::cout << "Confirmed: Peer has piece #" << next_piece << "\n";

            // In real code, you would now:
            // 1. Send INTERESTED message
            // 2. Wait for UNCHOKE
            // 3. Request blocks for this piece
        } else {
            std::cout << "ERROR: Peer doesn't have this piece!\n";
        }
    } else {
        std::cout << "No pieces to download from this peer\n";
    }
}

void example_sharing_pieces() {
    std::cout << "\n\n=== Sharing Pieces Example ===\n\n";

    // Setup piece manager with some downloaded pieces
    size_t num_pieces = 20;
    size_t piece_length = 262144;
    size_t total_length = num_pieces * piece_length;
    std::vector<uint8_t> piece_hashes(num_pieces * 20, 0);

    PieceManager piece_manager(num_pieces, piece_length, total_length, piece_hashes);

    // Simulate having downloaded some pieces
    std::cout << "Downloading pieces...\n";
    for (uint32_t i = 0; i < 5; ++i) {
        piece_manager.markPieceComplete(i);
        std::cout << "  Downloaded piece #" << i << "\n";
    }

    std::cout << "\nProgress: " << piece_manager.percentComplete() << "%\n";
    std::cout << "Pieces: " << piece_manager.numPiecesDownloaded() << "/" << num_pieces << "\n\n";

    // Get our bitfield to share with peers
    std::vector<bool> our_bitfield = piece_manager.getBitfield();

    std::cout << "Our bitfield (first 20 pieces):\n  ";
    for (size_t i = 0; i < num_pieces; ++i) {
        std::cout << (our_bitfield[i] ? "1" : "0");
        if ((i + 1) % 10 == 0) std::cout << " ";
    }
    std::cout << "\n\n";

    // When connecting to a new peer, send our bitfield
    std::cout << "Connecting to new peer and sharing our bitfield...\n";

    // Create connection
    PeerConnection new_peer("192.168.1.102", 6881,
                           std::vector<uint8_t>(20, 0x42),
                           "-EX0003-sharing_test");

    // Connect and handshake will automatically send bitfield
    // if (!new_peer.connect()) return;
    // if (!new_peer.performHandshake(our_bitfield)) return;

    std::cout << "Bitfield sent to peer during handshake\n";
    std::cout << "Peer now knows we have pieces 0-4\n";
}

int main() {
    std::cout << "BitTorrent Bitfield Management Examples\n";
    std::cout << "========================================\n\n";

    // Uncomment to run examples:

    // Example 1: Basic bitfield management
    // example_bitfield_management();

    // Example 2: Piece selection using bitfield
    // example_piece_selection_with_bitfield();

    // Example 3: Sharing our pieces with peers
    // example_sharing_pieces();

    std::cout << "\nAll examples completed!\n";
    return 0;
}
