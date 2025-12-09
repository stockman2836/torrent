#pragma once

#include "tracker_client.h"
#include <string>
#include <vector>
#include <cstdint>
#include <random>

namespace torrent {

// BEP 15: UDP Tracker Protocol
// http://www.bittorrent.org/beps/bep_0015.html

// Protocol constants
constexpr int64_t UDP_PROTOCOL_ID = 0x41727101980LL;
constexpr int32_t UDP_ACTION_CONNECT = 0;
constexpr int32_t UDP_ACTION_ANNOUNCE = 1;
constexpr int32_t UDP_ACTION_SCRAPE = 2;
constexpr int32_t UDP_ACTION_ERROR = 3;

// Event types for announce
enum class UDPEvent : int32_t {
    NONE = 0,
    COMPLETED = 1,
    STARTED = 2,
    STOPPED = 3
};

// Connect request (16 bytes)
struct UDPConnectRequest {
    int64_t protocol_id;
    int32_t action;
    int32_t transaction_id;

    UDPConnectRequest(int32_t tid)
        : protocol_id(UDP_PROTOCOL_ID)
        , action(UDP_ACTION_CONNECT)
        , transaction_id(tid) {}

    std::vector<uint8_t> serialize() const;
};

// Connect response (16 bytes)
struct UDPConnectResponse {
    int32_t action;
    int32_t transaction_id;
    int64_t connection_id;

    static UDPConnectResponse deserialize(const std::vector<uint8_t>& data);
};

// Announce request (98 bytes)
struct UDPAnnounceRequest {
    int64_t connection_id;
    int32_t action;
    int32_t transaction_id;
    std::vector<uint8_t> info_hash;  // 20 bytes
    std::vector<uint8_t> peer_id;    // 20 bytes
    int64_t downloaded;
    int64_t left;
    int64_t uploaded;
    int32_t event;
    uint32_t ip;        // 0 = default
    uint32_t key;       // random
    int32_t num_want;   // -1 = default
    uint16_t port;

    UDPAnnounceRequest(int64_t conn_id,
                       int32_t tid,
                       const std::vector<uint8_t>& hash,
                       const std::string& peer_id_str,
                       int64_t dl,
                       int64_t lft,
                       int64_t ul,
                       UDPEvent evt,
                       uint16_t prt);

    std::vector<uint8_t> serialize() const;
};

// Announce response (20+ bytes)
struct UDPAnnounceResponse {
    int32_t action;
    int32_t transaction_id;
    int32_t interval;
    int32_t leechers;
    int32_t seeders;
    std::vector<PeerInfo> peers;

    static UDPAnnounceResponse deserialize(const std::vector<uint8_t>& data);
};

// Error response
struct UDPErrorResponse {
    int32_t action;
    int32_t transaction_id;
    std::string message;

    static UDPErrorResponse deserialize(const std::vector<uint8_t>& data);
};

class UDPTrackerClient {
public:
    UDPTrackerClient(const std::string& tracker_url,
                     const std::vector<uint8_t>& info_hash,
                     const std::string& peer_id);

    ~UDPTrackerClient();

    // Perform announce to UDP tracker
    TrackerResponse announce(int64_t uploaded,
                            int64_t downloaded,
                            int64_t left,
                            uint16_t port,
                            const std::string& event);

private:
    // Connect to tracker (get connection_id)
    bool connect();

    // Send UDP request and wait for response
    bool sendRequest(const std::vector<uint8_t>& request,
                    std::vector<uint8_t>& response,
                    int timeout_seconds = 15);

    // Parse tracker URL to get host and port
    bool parseURL(const std::string& url, std::string& host, uint16_t& port);

    // Generate random transaction ID
    int32_t generateTransactionId();

    std::string tracker_url_;
    std::vector<uint8_t> info_hash_;
    std::string peer_id_;

    int socket_fd_;
    int64_t connection_id_;
    bool connected_;

    std::random_device rd_;
    std::mt19937 rng_;
};

} // namespace torrent
