#include "peer_connection.h"
#include "tracker_client.h"
#include "torrent_file.h"
#include "utils.h"
#include <iostream>
#include <iomanip>
#include <fstream>

void printDivider() {
    std::cout << "\n" << std::string(70, '=') << "\n\n";
}

void printBytes(const std::vector<uint8_t>& data, const std::string& label) {
    std::cout << label << ": ";
    for (size_t i = 0; i < std::min(data.size(), size_t(20)); ++i) {
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<int>(data[i]);
        if (i < data.size() - 1) std::cout << " ";
    }
    std::cout << std::dec << "\n";
}

int main(int argc, char* argv[]) {
    std::cout << "=== BitTorrent Handshake Testing Tool ===\n";
    printDivider();

    if (argc < 2) {
        std::cout << "This tool tests the BitTorrent handshake implementation.\n\n";
        std::cout << "Usage:\n";
        std::cout << "  Mode 1 (with .torrent file):\n";
        std::cout << "    " << argv[0] << " <torrent_file>\n\n";
        std::cout << "  Mode 2 (manual peer):\n";
        std::cout << "    " << argv[0] << " <peer_ip> <peer_port> <info_hash_hex>\n\n";
        std::cout << "Examples:\n";
        std::cout << "  " << argv[0] << " ubuntu.torrent\n";
        std::cout << "  " << argv[0] << " 192.168.1.100 6881 ABCDEF0123456789ABCDEF0123456789ABCDEF01\n";
        return 1;
    }

    std::string peer_ip;
    uint16_t peer_port = 0;
    std::vector<uint8_t> info_hash;
    std::string peer_id = torrent::utils::generatePeerId();

    // Mode detection
    bool use_torrent_file = (argc == 2);

    if (use_torrent_file) {
        // Mode 1: Load torrent file, get tracker info, use first peer
        std::string torrent_file = argv[1];

        std::cout << "Mode: Using .torrent file\n";
        std::cout << "Torrent file: " << torrent_file << "\n\n";

        try {
            // Load torrent file
            std::cout << "Step 1: Loading torrent file...\n";
            torrent::TorrentFile torrent(torrent_file);

            info_hash = torrent.getInfoHash();
            std::string announce_url = torrent.getAnnounceUrl();

            std::cout << "  Announce URL: " << announce_url << "\n";
            printBytes(info_hash, "  Info Hash");
            std::cout << "\n";

            // Get peers from tracker
            std::cout << "Step 2: Contacting tracker for peer list...\n";
            torrent::TrackerClient tracker(announce_url, info_hash, peer_id);

            auto response = tracker.announce(
                0,          // uploaded
                0,          // downloaded
                torrent.getTotalSize(), // left
                6881,       // port
                "started"   // event
            );

            if (!response.isSuccess()) {
                std::cerr << "ERROR: Tracker request failed: " << response.failure_reason << "\n";
                return 1;
            }

            std::cout << "  Tracker response received:\n";
            std::cout << "    Interval: " << response.interval << " seconds\n";
            std::cout << "    Seeders: " << response.complete << "\n";
            std::cout << "    Leechers: " << response.incomplete << "\n";
            std::cout << "    Peers: " << response.peers.size() << "\n\n";

            if (response.peers.empty()) {
                std::cerr << "ERROR: No peers available from tracker\n";
                return 1;
            }

            // Use first peer
            peer_ip = response.peers[0].ip;
            peer_port = response.peers[0].port;

            std::cout << "  Selected peer: " << peer_ip << ":" << peer_port << "\n";

        } catch (const std::exception& e) {
            std::cerr << "ERROR: " << e.what() << "\n";
            return 1;
        }

    } else if (argc == 4) {
        // Mode 2: Manual peer specification
        peer_ip = argv[1];
        peer_port = static_cast<uint16_t>(std::stoi(argv[2]));
        std::string info_hash_hex = argv[3];

        std::cout << "Mode: Manual peer specification\n";
        std::cout << "Peer IP: " << peer_ip << "\n";
        std::cout << "Peer Port: " << peer_port << "\n";
        std::cout << "Info Hash (hex): " << info_hash_hex << "\n\n";

        // Convert hex info_hash to bytes
        if (info_hash_hex.length() != 40) {
            std::cerr << "ERROR: Info hash must be 40 hex characters (20 bytes)\n";
            return 1;
        }

        info_hash = torrent::utils::fromHex(info_hash_hex);
        if (info_hash.size() != 20) {
            std::cerr << "ERROR: Invalid info hash\n";
            return 1;
        }

    } else {
        std::cerr << "ERROR: Invalid arguments\n";
        return 1;
    }

    printDivider();

    // Test configuration
    std::cout << "Test Configuration:\n";
    std::cout << "  Target Peer: " << peer_ip << ":" << peer_port << "\n";
    std::cout << "  Our Peer ID: " << peer_id << "\n";
    printBytes(info_hash, "  Info Hash");
    printDivider();

    try {
        // Create peer connection
        torrent::PeerConnection peer(peer_ip, peer_port, info_hash, peer_id);

        // Test 1: Connect
        std::cout << "TEST 1: TCP Connection\n";
        std::cout << "Attempting to connect to " << peer_ip << ":" << peer_port << "...\n";

        if (!peer.connect()) {
            std::cerr << "\n[FAIL] Failed to establish TCP connection\n";
            return 1;
        }

        std::cout << "[PASS] TCP connection established\n";
        printDivider();

        // Test 2: Perform handshake
        std::cout << "TEST 2: BitTorrent Handshake\n";

        if (!peer.performHandshake()) {
            std::cerr << "\n[FAIL] Handshake failed\n";
            return 1;
        }

        std::cout << "[PASS] Handshake successful\n";
        printDivider();

        // Test 3: Verify handshake data
        std::cout << "TEST 3: Verify Handshake Data\n";

        const std::string& remote_peer_id = peer.remotePeerId();
        std::cout << "  Remote Peer ID length: " << remote_peer_id.size() << " bytes\n";

        if (remote_peer_id.size() != 20) {
            std::cerr << "[FAIL] Invalid peer ID size\n";
            return 1;
        }

        std::cout << "  Remote Peer ID (printable): ";
        for (char c : remote_peer_id) {
            if (c >= 32 && c <= 126) {
                std::cout << c;
            } else {
                std::cout << '.';
            }
        }
        std::cout << "\n";

        std::cout << "  Remote Peer ID (hex): ";
        for (size_t i = 0; i < remote_peer_id.size(); ++i) {
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<int>(static_cast<uint8_t>(remote_peer_id[i]));
            if (i < remote_peer_id.size() - 1) std::cout << " ";
        }
        std::cout << std::dec << "\n";

        std::cout << "[PASS] Handshake data verified\n";
        printDivider();

        // Test 4: Check connection state
        std::cout << "TEST 4: Connection State\n";
        std::cout << "  Connected: " << (peer.isConnected() ? "YES" : "NO") << "\n";
        std::cout << "  Am choking: " << (peer.amChoking() ? "YES" : "NO") << "\n";
        std::cout << "  Am interested: " << (peer.amInterested() ? "YES" : "NO") << "\n";
        std::cout << "  Peer choking: " << (peer.peerChoking() ? "YES" : "NO") << "\n";
        std::cout << "  Peer interested: " << (peer.peerInterested() ? "YES" : "NO") << "\n";

        if (!peer.isConnected()) {
            std::cerr << "[FAIL] Connection lost\n";
            return 1;
        }

        std::cout << "[PASS] Connection state valid\n";
        printDivider();

        // Test 5: Try duplicate handshake (should succeed without re-doing)
        std::cout << "TEST 5: Duplicate Handshake Prevention\n";
        std::cout << "Attempting handshake again (should be skipped)...\n";

        if (!peer.performHandshake()) {
            std::cerr << "[FAIL] Second handshake failed\n";
            return 1;
        }

        std::cout << "[PASS] Duplicate handshake handled correctly\n";
        printDivider();

        // Test 6: Clean disconnect
        std::cout << "TEST 6: Clean Disconnect\n";
        peer.disconnect();
        std::cout << "  Connected after disconnect: " << (peer.isConnected() ? "YES" : "NO") << "\n";

        if (peer.isConnected()) {
            std::cerr << "[FAIL] Still connected after disconnect\n";
            return 1;
        }

        std::cout << "[PASS] Clean disconnect successful\n";
        printDivider();

        // Summary
        std::cout << "╔════════════════════════════════════════════════════════════╗\n";
        std::cout << "║                   ALL TESTS PASSED!                        ║\n";
        std::cout << "╚════════════════════════════════════════════════════════════╝\n";
        std::cout << "\nHandshake implementation is working correctly!\n";

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "\n[EXCEPTION] " << e.what() << "\n";
        return 1;
    }
}
