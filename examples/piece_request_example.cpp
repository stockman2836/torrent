// Example: Piece Request Logic
// This demonstrates Task 2.3 implementation

#include "peer_connection.h"
#include "piece_manager.h"
#include <iostream>

using namespace torrent;

void example_piece_request_workflow() {
    std::cout << "=== Piece Request Workflow Example ===\n\n";

    // Setup
    std::string peer_ip = "192.168.1.100";
    uint16_t peer_port = 6881;
    std::vector<uint8_t> info_hash(20, 0x42);
    std::string peer_id = "-EX0001-123456789012";

    size_t num_pieces = 10;
    size_t piece_length = 262144; // 256KB
    size_t total_length = num_pieces * piece_length;
    std::vector<uint8_t> piece_hashes(num_pieces * 20, 0);

    PeerConnection peer(peer_ip, peer_port, info_hash, peer_id);
    PieceManager piece_manager(num_pieces, piece_length, total_length, piece_hashes);

    // Connect
    if (!peer.connect()) {
        std::cerr << "Failed to connect\n";
        return;
    }

    // Initialize bitfield and handshake
    peer.initializePeerBitfield(num_pieces);
    std::vector<bool> our_bitfield = piece_manager.getBitfield();

    if (!peer.performHandshake(our_bitfield)) {
        std::cerr << "Failed to perform handshake\n";
        return;
    }

    std::cout << "\n=== Step 1: Wait for peer's bitfield ===\n";
    auto message = peer.receiveMessage();
    if (!message || message->type != MessageType::BITFIELD) {
        std::cerr << "Expected BITFIELD message\n";
        return;
    }

    std::cout << "Peer has " << peer.getPeerPieceCount() << " pieces\n";

    std::cout << "\n=== Step 2: Check if we're interested ===\n";
    bool has_needed_pieces = false;
    for (uint32_t i = 0; i < num_pieces; ++i) {
        if (!piece_manager.hasPiece(i) && peer.peerHasPiece(i)) {
            std::cout << "Peer has piece #" << i << " that we need\n";
            has_needed_pieces = true;
        }
    }

    if (!has_needed_pieces) {
        std::cout << "Peer has no pieces we need\n";
        return;
    }

    std::cout << "\n=== Step 3: Send INTERESTED ===\n";
    peer.sendInterested();
    std::cout << "Sent INTERESTED message\n";

    std::cout << "\n=== Step 4: Wait for UNCHOKE ===\n";
    while (true) {
        message = peer.receiveMessage();
        if (!message) break;

        if (message->type == MessageType::UNCHOKE) {
            std::cout << "Received UNCHOKE - we can now request pieces!\n";
            break;
        }
        std::cout << "Received message type: " << static_cast<int>(message->type) << "\n";
    }

    if (!peer.canDownload()) {
        std::cout << "Still cannot download (choked or not interested)\n";
        return;
    }

    std::cout << "\n=== Step 5: Select piece to download ===\n";
    int32_t next_piece = piece_manager.getNextPiece(peer.peerBitfield());
    if (next_piece < 0) {
        std::cout << "No pieces to download\n";
        return;
    }

    std::cout << "Selected piece #" << next_piece << " for download\n";

    std::cout << "\n=== Step 6: Split piece into blocks ===\n";
    std::vector<Block> blocks = piece_manager.getBlocksForPiece(next_piece);
    std::cout << "Piece split into " << blocks.size() << " blocks (16KB each)\n";

    for (size_t i = 0; i < blocks.size(); ++i) {
        std::cout << "  Block " << i << ": offset=" << blocks[i].offset
                  << " length=" << blocks[i].length << "\n";
    }

    std::cout << "\n=== Step 7: Request all blocks ===\n";
    if (peer.requestPiece(next_piece, blocks)) {
        std::cout << "Successfully requested piece #" << next_piece << "\n";
        std::cout << "Pending requests: " << peer.numPendingRequests() << "\n";
    }

    std::cout << "\n=== Step 8: Receive PIECE messages ===\n";
    int blocks_received = 0;

    while (blocks_received < blocks.size()) {
        message = peer.receiveMessage();
        if (!message) break;

        if (message->type == MessageType::PIECE) {
            PieceMessage piece_msg;
            if (peer.parsePiece(*message, piece_msg)) {
                std::cout << "Received block: piece=" << piece_msg.piece_index
                          << " offset=" << piece_msg.offset
                          << " size=" << piece_msg.data.size() << " bytes\n";

                // Add to piece manager
                piece_manager.addBlock(piece_msg.piece_index, piece_msg.offset, piece_msg.data);

                blocks_received++;
                std::cout << "Progress: " << blocks_received << "/" << blocks.size()
                          << " blocks (" << (blocks_received * 100 / blocks.size()) << "%)\n";
            }
        }

        std::cout << "Pending requests: " << peer.numPendingRequests() << "\n";
    }

    std::cout << "\n=== Download Complete! ===\n";
}

void example_timeout_handling() {
    std::cout << "\n\n=== Request Timeout Example ===\n\n";

    std::string peer_ip = "192.168.1.101";
    uint16_t peer_port = 6881;
    std::vector<uint8_t> info_hash(20, 0x42);
    std::string peer_id = "-EX0002-timeout_test";

    PeerConnection peer(peer_ip, peer_port, info_hash, peer_id);

    // Simulate some pending requests
    std::cout << "Simulating pending requests...\n";
    // In real code, these would be actual requests

    std::cout << "Checking for timed out requests (30 second timeout)...\n";
    auto timed_out = peer.getTimedOutRequests(30);

    if (timed_out.empty()) {
        std::cout << "No timed out requests\n";
    } else {
        std::cout << "Found " << timed_out.size() << " timed out requests:\n";
        for (const auto& req : timed_out) {
            std::cout << "  Piece #" << req.piece_index
                      << " offset=" << req.offset
                      << " length=" << req.length << "\n";
        }

        std::cout << "These requests can be retried with another peer\n";
    }
}

void example_state_management() {
    std::cout << "\n\n=== State Management Example ===\n\n";

    std::string peer_ip = "192.168.1.102";
    uint16_t peer_port = 6881;
    std::vector<uint8_t> info_hash(20, 0x42);
    std::string peer_id = "-EX0003-state_test";

    PeerConnection peer(peer_ip, peer_port, info_hash, peer_id);

    std::cout << "Initial state:\n";
    std::cout << "  Can download: " << peer.canDownload() << "\n";
    std::cout << "  Ready for requests: " << peer.isReadyForRequests() << "\n";
    std::cout << "  Am interested: " << peer.amInterested() << "\n";
    std::cout << "  Peer choking: " << peer.peerChoking() << "\n";

    // After handshake and sending INTERESTED
    std::cout << "\nAfter sending INTERESTED:\n";
    // peer.sendInterested();
    std::cout << "  Am interested: true\n";
    std::cout << "  Still choked, cannot download yet\n";

    // After receiving UNCHOKE
    std::cout << "\nAfter receiving UNCHOKE:\n";
    std::cout << "  Can download: true\n";
    std::cout << "  Ready for requests: true\n";
    std::cout << "  Can start requesting pieces!\n";

    // If peer sends CHOKE
    std::cout << "\nIf peer sends CHOKE:\n";
    std::cout << "  Pending requests will be cleared automatically\n";
    std::cout << "  Need to wait for UNCHOKE again before requesting\n";
}

int main() {
    std::cout << "Piece Request Logic Examples\n";
    std::cout << "=============================\n\n";

    // Uncomment to run examples:

    // Example 1: Complete workflow
    // example_piece_request_workflow();

    // Example 2: Timeout handling
    // example_timeout_handling();

    // Example 3: State management
    // example_state_management();

    std::cout << "\nAll examples completed!\n";
    return 0;
}
