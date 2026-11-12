#pragma once

#include "torrent/network/peer/peer_connection.h"
#include "torrent/network/peer/listener_status.h"

namespace torrent {

// Owns the listening socket. The session runs accept() on its listener worker.
class PeerListener {
public:
    explicit PeerListener(uint16_t port, bool ipv6 = true);
    ~PeerListener();
    PeerListener(const PeerListener&) = delete;
    PeerListener& operator=(const PeerListener&) = delete;
    uint16_t port() const { return port_; }
    ListenerStatus status() const;
    std::unique_ptr<PeerConnection> accept(const std::vector<uint8_t>& info_hash,
                                         const std::string& peer_id, int timeout_ms = 100);
private:
    std::intptr_t socket_ = -1;
    uint16_t port_ = 0;
    bool ipv6_ = false;
};

} // namespace torrent
