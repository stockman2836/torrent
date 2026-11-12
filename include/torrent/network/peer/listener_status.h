#pragma once

#include <cstdint>
#include <string>

namespace torrent {

enum class ListenerState { NotStarted, Listening, Stopped, Error };

// Local socket evidence only; it does not prove reachability through a router.
struct ListenerStatus {
    ListenerState state = ListenerState::NotStarted;
    uint16_t port = 0;
    bool ipv6 = false; // The listener accepts both IPv4 and IPv6 when true.
    std::string error;
};

bool isLoopbackAddress(const std::string& address);

} // namespace torrent
