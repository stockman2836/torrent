// Example: How to use the new message parsing functionality
// This file demonstrates the usage of Task 2.1 implementation

#include "peer_connection.h"
#include <iostream>

using namespace torrent;

void example_receiving_and_parsing_messages() {
    // Assume we have a connected PeerConnection
    std::string peer_ip = "192.168.1.100";
    uint16_t peer_port = 6881;
    std::vector<uint8_t> info_hash(20, 0); // Example info hash
    std::string peer_id = "-EX0001-123456789012"; // Example peer ID

    PeerConnection peer(peer_ip, peer_port, info_hash, peer_id);

    // Connect and perform handshake
    if (!peer.connect()) {
        std::cerr << "Failed to connect\n";
        return;
    }

    if (!peer.performHandshake()) {
        std::cerr << "Failed to perform handshake\n";
        return;
    }

    // Receive and parse messages
    while (true) {
        // Receive a message (this automatically parses and queues it)
        auto message = peer.receiveMessage();

        if (!message) {
            std::cerr << "Failed to receive message or connection closed\n";
            break;
        }

        // Process based on message type
        switch (message->type) {
            case MessageType::KEEP_ALIVE:
                std::cout << "Received keep-alive\n";
                break;

            case MessageType::CHOKE:
                std::cout << "Peer choked us\n";
                break;

            case MessageType::UNCHOKE:
                std::cout << "Peer unchoked us - we can request pieces now!\n";
                break;

            case MessageType::INTERESTED:
                std::cout << "Peer is interested in our pieces\n";
                break;

            case MessageType::BITFIELD: {
                BitfieldMessage bitfield;
                if (peer.parseBitfield(*message, bitfield)) {
                    std::cout << "Received bitfield with " << bitfield.bitfield.size() << " pieces\n";

                    // Count how many pieces the peer has
                    int piece_count = 0;
                    for (bool has_piece : bitfield.bitfield) {
                        if (has_piece) piece_count++;
                    }
                    std::cout << "Peer has " << piece_count << " pieces\n";
                }
                break;
            }

            case MessageType::HAVE: {
                HaveMessage have;
                if (peer.parseHave(*message, have)) {
                    std::cout << "Peer now has piece #" << have.piece_index << "\n";

                    // You can check if we need this piece and request it
                }
                break;
            }

            case MessageType::PIECE: {
                PieceMessage piece;
                if (peer.parsePiece(*message, piece)) {
                    std::cout << "Received piece data:\n";
                    std::cout << "  Piece index: " << piece.piece_index << "\n";
                    std::cout << "  Offset: " << piece.offset << "\n";
                    std::cout << "  Data size: " << piece.data.size() << " bytes\n";

                    // Here you would:
                    // 1. Store the piece data
                    // 2. Check if the full piece is complete
                    // 3. Verify the piece hash
                    // 4. Write to disk if valid
                    // 5. Send HAVE to other peers
                }
                break;
            }

            case MessageType::REQUEST: {
                RequestMessage request;
                if (peer.parseRequest(*message, request)) {
                    std::cout << "Peer requested:\n";
                    std::cout << "  Piece index: " << request.piece_index << "\n";
                    std::cout << "  Offset: " << request.offset << "\n";
                    std::cout << "  Length: " << request.length << " bytes\n";

                    // Here you would:
                    // 1. Check if we have this piece
                    // 2. Read the data from disk
                    // 3. Send PIECE message back to peer
                }
                break;
            }

            case MessageType::CANCEL: {
                CancelMessage cancel;
                if (peer.parseCancel(*message, cancel)) {
                    std::cout << "Peer cancelled request for piece #" << cancel.piece_index << "\n";

                    // Remove the request from pending uploads
                }
                break;
            }

            default:
                std::cout << "Received unknown message type\n";
                break;
        }

        // Check if there are more messages in the queue
        while (peer.hasMessages()) {
            auto queued_message = peer.popMessage();
            if (queued_message) {
                std::cout << "Processing queued message of type "
                          << static_cast<int>(queued_message->type) << "\n";
            }
        }
    }
}

// Example: Sending messages after receiving appropriate responses
void example_requesting_pieces() {
    // ... setup connection as above ...

    // After receiving UNCHOKE from peer, we can request pieces
    PeerConnection peer("192.168.1.100", 6881,
                       std::vector<uint8_t>(20, 0),
                       "-EX0001-123456789012");

    // 1. First, send INTERESTED to let peer know we want pieces
    peer.sendInterested();

    // 2. Wait for UNCHOKE message
    auto msg = peer.receiveMessage();
    if (msg && msg->type == MessageType::UNCHOKE) {
        std::cout << "Peer unchoked us, we can request now\n";

        // 3. Request a piece (16KB block at offset 0 of piece 0)
        uint32_t piece_index = 0;
        uint32_t offset = 0;
        uint32_t length = 16384; // 16KB standard block size

        peer.sendRequest(piece_index, offset, length);

        // 4. Wait for PIECE message
        auto piece_msg = peer.receiveMessage();
        if (piece_msg && piece_msg->type == MessageType::PIECE) {
            PieceMessage piece;
            if (peer.parsePiece(*piece_msg, piece)) {
                std::cout << "Successfully received piece data!\n";
                // Process the piece...
            }
        }
    }
}

int main() {
    std::cout << "Message Parsing Examples\n";
    std::cout << "========================\n\n";

    std::cout << "Example 1: Receiving and parsing messages\n";
    // Uncomment to run:
    // example_receiving_and_parsing_messages();

    std::cout << "\nExample 2: Requesting pieces\n";
    // Uncomment to run:
    // example_requesting_pieces();

    return 0;
}
