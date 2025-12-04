#include "peer_connection.h"
#include "utils.h"
#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>

void printBytes(const std::vector<uint8_t>& data, const std::string& label) {
    std::cout << label << ": ";
    for (size_t i = 0; i < std::min(data.size(), size_t(32)); ++i) {
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<int>(data[i]) << " ";
    }
    if (data.size() > 32) {
        std::cout << "... (" << std::dec << data.size() << " bytes total)";
    }
    std::cout << std::dec << "\n";
}

int main(int argc, char* argv[]) {
    std::cout << "=== Testing TCP Socket Implementation ===\n\n";

    // For testing, you need a peer IP and port
    // These would typically come from a tracker response

    if (argc < 3) {
        std::cout << "Usage: " << argv[0] << " <peer_ip> <peer_port>\n";
        std::cout << "\nNote: To get peer IPs:\n";
        std::cout << "  1. Use test_http_client to get peers from tracker\n";
        std::cout << "  2. Or manually specify a known peer\n";
        std::cout << "\nExample:\n";
        std::cout << "  " << argv[0] << " 192.168.1.100 6881\n";
        return 1;
    }

    std::string peer_ip = argv[1];
    uint16_t peer_port = static_cast<uint16_t>(std::stoi(argv[2]));

    // Create dummy info_hash and peer_id for testing
    std::vector<uint8_t> info_hash(20, 0);
    for (size_t i = 0; i < 20; ++i) {
        info_hash[i] = static_cast<uint8_t>(i * 12);
    }

    std::string peer_id = torrent::utils::generatePeerId();

    std::cout << "Test Configuration:\n";
    std::cout << "  Peer IP: " << peer_ip << "\n";
    std::cout << "  Peer Port: " << peer_port << "\n";
    std::cout << "  Our Peer ID: " << peer_id << "\n";
    printBytes(info_hash, "  Info Hash");
    std::cout << "\n";

    try {
        // Create peer connection
        torrent::PeerConnection peer(peer_ip, peer_port, info_hash, peer_id);

        // Test 1: Connect
        std::cout << "Test 1: Connecting to peer...\n";
        if (!peer.connect()) {
            std::cerr << "Failed to connect to peer\n";
            return 1;
        }
        std::cout << "  [OK] Connected successfully\n\n";

        // Test 2: Perform handshake
        std::cout << "Test 2: Performing BitTorrent handshake...\n";
        if (!peer.performHandshake()) {
            std::cerr << "Handshake failed\n";
            return 1;
        }
        std::cout << "  [OK] Handshake successful\n\n";

        // Test 3: Send interested message
        std::cout << "Test 3: Sending INTERESTED message...\n";
        if (!peer.sendInterested()) {
            std::cerr << "Failed to send INTERESTED\n";
            return 1;
        }
        std::cout << "  [OK] INTERESTED sent\n\n";

        // Test 4: Receive messages (with timeout)
        std::cout << "Test 4: Receiving messages from peer...\n";
        for (int i = 0; i < 5; ++i) {
            auto message = peer.receiveMessage();
            if (message) {
                std::cout << "  [OK] Received message type: "
                          << static_cast<int>(message->type)
                          << " with payload size: " << message->payload.size() << "\n";

                // Print message type name
                switch (message->type) {
                    case torrent::MessageType::CHOKE:
                        std::cout << "       (CHOKE)\n";
                        break;
                    case torrent::MessageType::UNCHOKE:
                        std::cout << "       (UNCHOKE)\n";
                        break;
                    case torrent::MessageType::INTERESTED:
                        std::cout << "       (INTERESTED)\n";
                        break;
                    case torrent::MessageType::NOT_INTERESTED:
                        std::cout << "       (NOT_INTERESTED)\n";
                        break;
                    case torrent::MessageType::HAVE:
                        std::cout << "       (HAVE)\n";
                        break;
                    case torrent::MessageType::BITFIELD:
                        std::cout << "       (BITFIELD)\n";
                        break;
                    default:
                        break;
                }
            } else {
                std::cout << "  No more messages or error\n";
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::cout << "\n";

        // Test 5: Check connection state
        std::cout << "Test 5: Connection state:\n";
        std::cout << "  Connected: " << (peer.isConnected() ? "YES" : "NO") << "\n";
        std::cout << "  Am choking: " << (peer.amChoking() ? "YES" : "NO") << "\n";
        std::cout << "  Am interested: " << (peer.amInterested() ? "YES" : "NO") << "\n";
        std::cout << "  Peer choking: " << (peer.peerChoking() ? "YES" : "NO") << "\n";
        std::cout << "  Peer interested: " << (peer.peerInterested() ? "YES" : "NO") << "\n";
        std::cout << "\n";

        // Test 6: Disconnect
        std::cout << "Test 6: Disconnecting...\n";
        peer.disconnect();
        std::cout << "  [OK] Disconnected\n";
        std::cout << "  Connected: " << (peer.isConnected() ? "YES" : "NO") << "\n\n";

        std::cout << "=== All Tests Passed! ===\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
        return 1;
    }
}
