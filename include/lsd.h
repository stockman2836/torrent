#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>

namespace torrent {

// BEP 14: Local Service Discovery (LSD)
// Discovers peers on local network via multicast

// LSD multicast address and port (IPv4)
constexpr const char* LSD_MULTICAST_ADDR = "239.192.152.143";
constexpr uint16_t LSD_MULTICAST_PORT = 6771;

// LSD multicast address and port (IPv6)
constexpr const char* LSD_MULTICAST_ADDR_V6 = "ff15::efc0:988f";
constexpr uint16_t LSD_MULTICAST_PORT_V6 = 6771;

// LSD cookie for message identification
constexpr const char* LSD_COOKIE = "BT-SEARCH * HTTP/1.1";

// Peer discovered via LSD
struct LSDPeer {
    std::string ip;
    uint16_t port;
    std::vector<uint8_t> info_hash;

    LSDPeer(const std::string& ip_, uint16_t port_, const std::vector<uint8_t>& info_hash_)
        : ip(ip_), port(port_), info_hash(info_hash_) {}
};

// Callback for when peers are discovered
using LSDPeerCallback = std::function<void(const std::vector<LSDPeer>&)>;

class LSD {
public:
    LSD(uint16_t listen_port = 6881, bool enable_ipv6 = false);
    ~LSD();

    // Start LSD service
    void start();

    // Stop LSD service
    void stop();

    // Check if LSD is running
    bool isRunning() const { return running_; }

    // Announce a torrent to the local network
    void announce(const std::vector<uint8_t>& info_hash);

    // Stop announcing a torrent
    void stopAnnounce(const std::vector<uint8_t>& info_hash);

    // Set callback for discovered peers
    void setPeerCallback(LSDPeerCallback callback);

    // Get statistics
    size_t getAnnouncedTorrentsCount() const;
    size_t getDiscoveredPeersCount() const;

private:
    // Announcement thread
    void announceLoop();

    // Listening thread
    void listenLoop();

    // Send LSD announcement for a specific info_hash
    void sendAnnouncement(const std::vector<uint8_t>& info_hash);

    // Parse incoming LSD message
    void parseMessage(const std::string& message, const std::string& sender_ip, uint16_t sender_port);

    // Build LSD announcement message
    std::string buildAnnounceMessage(const std::vector<uint8_t>& info_hash) const;

    // Socket operations
    bool createMulticastSocket();
    void closeSocket();
    bool joinMulticastGroup();
    bool leaveMulticastGroup();

    uint16_t listen_port_;
    bool enable_ipv6_;
    int socket_fd_;

    std::atomic<bool> running_;

    // Torrents being announced
    std::vector<std::vector<uint8_t>> announced_torrents_;
    std::mutex torrents_mutex_;

    // Discovered peers
    std::vector<LSDPeer> discovered_peers_;
    std::mutex peers_mutex_;

    // Callback for peer discovery
    LSDPeerCallback peer_callback_;
    std::mutex callback_mutex_;

    // Worker threads
    std::thread announce_thread_;
    std::thread listen_thread_;

    // Announce interval (seconds)
    static constexpr int ANNOUNCE_INTERVAL = 5 * 60;  // 5 minutes
};

} // namespace torrent
