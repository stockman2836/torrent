#include "tracker_client.h"
#include "bencode.h"
#include "utils.h"
#include "logger.h"
#include <sstream>
#include <stdexcept>
#include <cstring>
#include <curl/curl.h>

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

// Callback function for libcurl to write response data
static size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    std::string* response = static_cast<std::string*>(userp);
    response->append(static_cast<char*>(contents), total_size);
    return total_size;
}

// Helper function to perform HTTP GET request using libcurl
static std::string httpGet(const std::string& url, std::string& error_msg) {
    LOG_DEBUG("HTTP GET request to: {}", url);

    CURL* curl = curl_easy_init();
    if (!curl) {
        error_msg = "Failed to initialize libcurl";
        LOG_ERROR("Failed to initialize libcurl");
        return "";
    }

    std::string response_data;
    CURLcode res;

    // Set URL
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

    // Follow redirects (up to 5)
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);

    // Set timeout (30 seconds)
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    // Set user agent
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "BitTorrent Client/1.0");

    // Set write callback
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);

    // Disable SSL verification for now (can be enabled later)
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    // Perform the request
    res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        error_msg = std::string("HTTP request failed: ") + curl_easy_strerror(res);
        LOG_ERROR("HTTP request failed: {}", curl_easy_strerror(res));
        curl_easy_cleanup(curl);
        return "";
    }

    // Check HTTP response code
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (http_code != 200) {
        std::ostringstream oss;
        oss << "HTTP error code: " << http_code;
        error_msg = oss.str();
        LOG_ERROR("HTTP error code: {}", http_code);
        curl_easy_cleanup(curl);
        return "";
    }

    LOG_DEBUG("HTTP request successful, received {} bytes", response_data.size());
    curl_easy_cleanup(curl);
    return response_data;
}

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
    LOG_INFO("Announcing to tracker: uploaded={}, downloaded={}, left={}, port={}, event={}",
             uploaded, downloaded, left, port, event);

    std::string url = buildAnnounceUrl(uploaded, downloaded, left, port, event);

    // Perform HTTP GET request
    std::string error_msg;
    std::string response_body = httpGet(url, error_msg);

    // Handle HTTP errors
    if (response_body.empty()) {
        LOG_ERROR("Tracker announce failed: {}", error_msg);
        TrackerResponse error_response;
        error_response.interval = 1800;
        error_response.complete = 0;
        error_response.incomplete = 0;
        error_response.failure_reason = error_msg;
        return error_response;
    }

    // Parse the tracker response
    return parseResponse(response_body);
}

TrackerResponse TrackerClient::parseResponse(const std::string& response) {
    TrackerResponse result;

    LOG_DEBUG("Parsing tracker response");

    try {
        BencodeValue parsed = BencodeParser::parse(response);
        if (!parsed.isDictionary()) {
            result.failure_reason = "Invalid tracker response";
            LOG_ERROR("Invalid tracker response: not a dictionary");
            return result;
        }

        const auto& dict = parsed.getDictionary();

        // Check for failure
        auto it = dict.find("failure reason");
        if (it != dict.end() && it->second.isString()) {
            result.failure_reason = it->second.getString();
            LOG_WARN("Tracker returned failure: {}", result.failure_reason);
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
        LOG_ERROR("Exception parsing tracker response: {}", e.what());
    }

    if (result.failure_reason.empty()) {
        LOG_INFO("Tracker response parsed: interval={}, complete={}, incomplete={}, peers={}",
                 result.interval, result.complete, result.incomplete, result.peers.size());
    }

    return result;
}

} // namespace torrent
