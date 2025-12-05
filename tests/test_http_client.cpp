#include "tracker_client.h"
#include "utils.h"
#include <iostream>
#include <iomanip>

int main() {
    std::cout << "=== Testing HTTP Tracker Client ===\n\n";

    // Create a dummy info_hash and peer_id for testing
    std::vector<uint8_t> info_hash(20, 0);
    for (size_t i = 0; i < 20; ++i) {
        info_hash[i] = static_cast<uint8_t>(i);
    }

    std::string peer_id = torrent::utils::generatePeerId();

    // Test with a known public tracker (example)
    // For real testing, you would need a valid torrent file and its announce URL
    std::string announce_url = "http://tracker.opentrackr.org:1337/announce";

    std::cout << "Testing tracker: " << announce_url << "\n";
    std::cout << "Peer ID: " << peer_id << "\n";
    std::cout << "Info hash: " << torrent::utils::toHex(info_hash) << "\n\n";

    try {
        torrent::TrackerClient client(announce_url, info_hash, peer_id);

        // Test announce request
        std::cout << "Sending announce request...\n";
        torrent::TrackerResponse response = client.announce(
            0,      // uploaded
            0,      // downloaded
            1000000,// left (1MB)
            6881,   // port
            "started" // event
        );

        std::cout << "\n=== Tracker Response ===\n";

        if (!response.isSuccess()) {
            std::cout << "Error: " << response.failure_reason << "\n";
            return 1;
        }

        std::cout << "Success!\n";
        std::cout << "Interval: " << response.interval << " seconds\n";
        std::cout << "Complete (seeders): " << response.complete << "\n";
        std::cout << "Incomplete (leechers): " << response.incomplete << "\n";
        std::cout << "Peers received: " << response.peers.size() << "\n\n";

        if (!response.peers.empty()) {
            std::cout << "First 10 peers:\n";
            for (size_t i = 0; i < std::min(response.peers.size(), size_t(10)); ++i) {
                const auto& peer = response.peers[i];
                std::cout << "  " << peer.ip << ":" << peer.port << "\n";
            }
        }

        std::cout << "\n=== Test Passed! ===\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
        return 1;
    }
}
