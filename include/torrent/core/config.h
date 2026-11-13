#pragma once

#include <string>
#include <cstdint>
#include <optional>
#include <vector>
#include <cstddef>
#include <map>
#include <utility>
#include "torrent/core/file_priority.h"
#include "torrent/core/port_mapping_options.h"

namespace torrent {

struct DhtBootstrapNode {
    std::string host;
    uint16_t port;
    DhtBootstrapNode(std::string host, uint16_t port) : host(std::move(host)), port(port) {}
};

struct Config {
    // Network settings
    uint16_t listen_port = 6881;
    int max_peers = 50;
    int max_connections = 100;
    bool enable_port_mapping = false;
    PortMappingOptions port_mapping_options; // Runtime overrides; never loaded from JSON.

    // Speed limits (bytes per second, 0 = unlimited)
    int64_t max_download_speed = 0;  // 0 = unlimited
    int64_t max_upload_speed = 0;    // 0 = unlimited
    // Desktop-wide payload limits; individual torrent caps above still apply.
    int64_t global_max_download_speed = 0;
    int64_t global_max_upload_speed = 0;

    // Download settings
    std::string download_dir = "./downloads";
    bool sequential_download = false;
    int piece_timeout_seconds = 30;
    // Runtime CLI/API selection, not persisted in the configuration file.
    // Zero-based file indices; empty means all files.
    std::vector<size_t> file_indices;
    std::map<size_t, FilePriority> file_priorities; // Runtime overrides, also zero-based.

    // Upload/Seeding settings
    bool seed_after_download = false;
    double seed_ratio_limit = 2.0;  // Stop seeding after ratio (0 = unlimited)
    int seed_time_limit_hours = 0;  // Stop seeding after hours (0 = unlimited)

    // Tracker settings
    int tracker_announce_interval = 1800;  // seconds
    int tracker_timeout_seconds = 30;
    int tracker_max_retries = 3;

    // Logging settings
    std::string log_level = "info";
    std::string log_file = "torrent_client.log";
    int64_t log_max_size = 5 * 1024 * 1024;  // 5 MB
    int log_max_files = 3;

    // Resume capability
    bool enable_resume = true;
    std::string resume_dir = "./resume";

    // DHT settings
    bool enable_dht = true;
    uint16_t dht_port = 6881;
    std::vector<DhtBootstrapNode> dht_bootstrap_nodes; // Empty uses the built-in public routers.

    // PEX settings (BEP 11)
    bool enable_pex = true;   // Now implemented!

    // Encryption settings (MSE/PE - BEP 8)
    bool enable_encryption = true;       // Enable MSE/PE support
    std::string encryption_mode = "prefer_encrypted";  // prefer_plaintext, prefer_encrypted, require_encrypted
    bool allow_legacy_peers = true;      // Allow connections without encryption

    // IPv6 settings (BEP 7)
    bool enable_ipv6 = true;             // Enable IPv6 support
    std::string ip_version = "dual";     // ipv4, ipv6, dual (prefer ipv4), dual_v6 (prefer ipv6)

    // LSD settings (BEP 14)
    bool enable_lsd = true;              // Enable Local Service Discovery

    // Web Seeding settings (BEP 19)
    bool enable_webseeds = true;         // Enable HTTP/FTP web seeding

    // uTP settings (BEP 29)
    bool enable_utp = true;              // Enable uTP (Micro Transport Protocol)
    bool prefer_utp = false;             // Prefer uTP over TCP when both available

    // Load configuration from JSON file
    // Returns true if file loaded successfully, false otherwise
    bool loadFromFile(const std::string& filepath);

    // Save current configuration to JSON file
    bool saveToFile(const std::string& filepath) const;

    // Load default configuration
    static Config loadDefault();

    // Merge with CLI arguments (CLI overrides config file)
    void mergeWithCLI(const std::optional<std::string>& download_dir_cli,
                      const std::optional<int64_t>& max_download_speed_cli,
                      const std::optional<int64_t>& max_upload_speed_cli,
                      const std::optional<uint16_t>& listen_port_cli,
                      const std::optional<std::string>& log_level_cli);

    // Validate configuration values
    bool validate() const;

    // Print configuration summary
    void print() const;
};

} // namespace torrent
