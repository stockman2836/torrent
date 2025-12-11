#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "utils.h"

namespace torrent {

struct Peer {
    std::string ip;
    uint16_t port;
    bool is_ipv6;

    Peer(const std::string& ip_, uint16_t port_)
        : ip(ip_), port(port_),
          is_ipv6(utils::detectIPVersion(ip_) == utils::IPVersion::IPv6) {}

    // Static factory methods
    static Peer fromCompactIPv4(const uint8_t* data);
    static Peer fromCompactIPv6(const uint8_t* data);

    // Convert to compact format
    std::vector<uint8_t> toCompact() const;
};

// Alias for consistency with UDP tracker
using PeerInfo = Peer;

class TrackerResponse {
public:
    int32_t interval;
    int32_t complete;    // seeders
    int32_t incomplete;  // leechers
    std::vector<Peer> peers;
    std::string failure_reason;

    bool isSuccess() const { return failure_reason.empty(); }
};

class TrackerClient {
public:
    TrackerClient(const std::string& announce_url,
                  const std::vector<uint8_t>& info_hash,
                  const std::string& peer_id);

    // Send announce request to tracker (automatically selects HTTP or UDP)
    TrackerResponse announce(int64_t uploaded,
                            int64_t downloaded,
                            int64_t left,
                            uint16_t port,
                            const std::string& event = "");

    // Check if tracker is UDP
    bool isUDP() const;

private:
    // HTTP tracker methods
    std::string buildAnnounceUrl(int64_t uploaded,
                                 int64_t downloaded,
                                 int64_t left,
                                 uint16_t port,
                                 const std::string& event);

    TrackerResponse parseResponse(const std::string& response);

    TrackerResponse announceHTTP(int64_t uploaded,
                                int64_t downloaded,
                                int64_t left,
                                uint16_t port,
                                const std::string& event);

    TrackerResponse announceUDP(int64_t uploaded,
                               int64_t downloaded,
                               int64_t left,
                               uint16_t port,
                               const std::string& event);

    std::string announce_url_;
    std::vector<uint8_t> info_hash_;
    std::string peer_id_;
    bool is_udp_;
};

} // namespace torrent
