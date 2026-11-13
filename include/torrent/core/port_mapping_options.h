#pragma once

#include <cstdint>
#include <string>

namespace torrent {

// Runtime embedding/test overrides; the desktop uses default IPv4 gateway discovery.
// Only enable_port_mapping is persisted in Config.
struct PortMappingOptions {
    std::string gateway;
    std::string discovery_address = "239.255.255.250";
    uint16_t discovery_port = 1900;
    uint16_t natpmp_port = 5351;
    uint32_t lease_seconds = 3600;
    int request_timeout_ms = 1500;
    int discovery_timeout_ms = 1500;
    int retry_delay_ms = 30000;
};

} // namespace torrent
