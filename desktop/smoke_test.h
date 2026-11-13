#pragma once

#include <cstdint>
#include <string>

namespace torrent::desktop {

// State for the explicit --ui-test modes. Disabled for an ordinary launch.
struct SmokeTest {
    bool enabled = false, priorities = false, global_limits = false, network = false, mapping = false, trackers = false;
    uint16_t nat_port = 0, ssdp_port = 0;
    std::wstring source, second, folder, config;
    bool started = false, priorities_applied = false, global_limits_applied = false;
    bool network_checked = false, mapping_checked = false;
    uint64_t deadline = 0, started_at = 0;
};

SmokeTest parseSmokeTest(int argc, wchar_t* argv[]);

} // namespace torrent::desktop
