#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace torrent {

struct Peer {
    std::string ip;
    uint16_t port;

    Peer(const std::string& ip, uint16_t port) : ip(ip), port(port) {}
};

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

    // Send announce request to tracker
    TrackerResponse announce(int64_t uploaded,
                            int64_t downloaded,
                            int64_t left,
                            uint16_t port,
                            const std::string& event = "");

private:
    std::string buildAnnounceUrl(int64_t uploaded,
                                 int64_t downloaded,
                                 int64_t left,
                                 uint16_t port,
                                 const std::string& event);

    TrackerResponse parseResponse(const std::string& response);

    std::string announce_url_;
    std::vector<uint8_t> info_hash_;
    std::string peer_id_;
};

} // namespace torrent
