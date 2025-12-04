#include "tracker_client.h"
#include "bencode.h"
#include "utils.h"
#include <sstream>
#include <stdexcept>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#endif

namespace torrent {

TrackerClient::TrackerClient(const std::string& announce_url,
                             const std::vector<uint8_t>& info_hash,
                             const std::string& peer_id)
    : announce_url_(announce_url)
    , info_hash_(info_hash)
    , peer_id_(peer_id) {
}

std::string TrackerClient::buildAnnounceUrl(int64_t uploaded,
                                            int64_t downloaded,
                                            int64_t left,
                                            uint16_t port,
                                            const std::string& event) {
    std::ostringstream url;
    url << announce_url_;

    // Add separator
    url << (announce_url_.find('?') != std::string::npos ? "&" : "?");

    // Required parameters
    url << "info_hash=" << utils::urlEncode(info_hash_);
    url << "&peer_id=" << utils::urlEncode(peer_id_);
    url << "&port=" << port;
    url << "&uploaded=" << uploaded;
    url << "&downloaded=" << downloaded;
    url << "&left=" << left;
    url << "&compact=1";  // Request compact peer list

    if (!event.empty()) {
        url << "&event=" << event;
    }

    return url.str();
}

TrackerResponse TrackerClient::announce(int64_t uploaded,
                                       int64_t downloaded,
                                       int64_t left,
                                       uint16_t port,
                                       const std::string& event) {
    std::string url = buildAnnounceUrl(uploaded, downloaded, left, port, event);

    // TODO: Implement actual HTTP request
    // For now, return a dummy response
    // In a full implementation, you would use libcurl or implement HTTP client

    TrackerResponse response;
    response.interval = 1800;  // 30 minutes
    response.complete = 0;
    response.incomplete = 0;
    response.failure_reason = "HTTP client not yet implemented";

    return response;
}

TrackerResponse TrackerClient::parseResponse(const std::string& response) {
    TrackerResponse result;

    try {
        BencodeValue parsed = BencodeParser::parse(response);
        if (!parsed.isDictionary()) {
            result.failure_reason = "Invalid tracker response";
            return result;
        }

        const auto& dict = parsed.getDictionary();

        // Check for failure
        auto it = dict.find("failure reason");
        if (it != dict.end() && it->second.isString()) {
            result.failure_reason = it->second.getString();
            return result;
        }

        // Interval
        it = dict.find("interval");
        if (it != dict.end() && it->second.isInteger()) {
            result.interval = static_cast<int32_t>(it->second.getInteger());
        }

        // Complete (seeders)
        it = dict.find("complete");
        if (it != dict.end() && it->second.isInteger()) {
            result.complete = static_cast<int32_t>(it->second.getInteger());
        }

        // Incomplete (leechers)
        it = dict.find("incomplete");
        if (it != dict.end() && it->second.isInteger()) {
            result.incomplete = static_cast<int32_t>(it->second.getInteger());
        }

        // Peers
        it = dict.find("peers");
        if (it != dict.end()) {
            if (it->second.isString()) {
                // Compact format: 6 bytes per peer (4 for IP, 2 for port)
                const std::string& peers_data = it->second.getString();
                for (size_t i = 0; i + 6 <= peers_data.size(); i += 6) {
                    uint8_t ip1 = static_cast<uint8_t>(peers_data[i]);
                    uint8_t ip2 = static_cast<uint8_t>(peers_data[i + 1]);
                    uint8_t ip3 = static_cast<uint8_t>(peers_data[i + 2]);
                    uint8_t ip4 = static_cast<uint8_t>(peers_data[i + 3]);

                    std::ostringstream ip_oss;
                    ip_oss << static_cast<int>(ip1) << "."
                          << static_cast<int>(ip2) << "."
                          << static_cast<int>(ip3) << "."
                          << static_cast<int>(ip4);

                    uint16_t port = (static_cast<uint8_t>(peers_data[i + 4]) << 8) |
                                   static_cast<uint8_t>(peers_data[i + 5]);

                    result.peers.emplace_back(ip_oss.str(), port);
                }
            } else if (it->second.isList()) {
                // Dictionary format
                for (const auto& peer_value : it->second.getList()) {
                    if (!peer_value.isDictionary()) {
                        continue;
                    }

                    const auto& peer_dict = peer_value.getDictionary();

                    auto ip_it = peer_dict.find("ip");
                    auto port_it = peer_dict.find("port");

                    if (ip_it != peer_dict.end() && ip_it->second.isString() &&
                        port_it != peer_dict.end() && port_it->second.isInteger()) {

                        std::string ip = ip_it->second.getString();
                        uint16_t port = static_cast<uint16_t>(port_it->second.getInteger());

                        result.peers.emplace_back(ip, port);
                    }
                }
            }
        }

    } catch (const std::exception& e) {
        result.failure_reason = std::string("Failed to parse tracker response: ") + e.what();
    }

    return result;
}

} // namespace torrent
