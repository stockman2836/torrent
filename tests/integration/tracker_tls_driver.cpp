#include "torrent/network/tracker/tracker_client.h"
#include <iostream>

int main(int argc, char* argv[]) {
    if (argc != 3) return 2;
    torrent::TrackerClient tracker(argv[1], std::vector<uint8_t>(20, 7), "-TLSTST-123456789012");
    const auto response = tracker.announce(0, 0, 1, 24567, "started");
    const bool expect_success = std::string(argv[2]) == "success";
    if (response.isSuccess() != expect_success) {
        std::cerr << "Unexpected tracker result: " << response.failure_reason << '\n'; return 1;
    }
    if (!expect_success) std::cout << response.failure_reason << '\n';
    return 0;
}
