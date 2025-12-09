#pragma once

#include <string>
#include <cstdint>
#include <optional>

namespace torrent {

struct Config {
    // Network settings
    uint16_t listen_port = 6881;
    int max_peers = 50;
    int max_connections = 100;

    // Speed limits (bytes per second, 0 = unlimited)
    int64_t max_download_speed = 0;  // 0 = unlimited
    int64_t max_upload_speed = 0;    // 0 = unlimited

    // Download settings
    std::string download_dir = "./downloads";
    bool sequential_download = false;
    int piece_timeout_seconds = 30;

    // Upload/Seeding settings
    bool seed_after_download = true;
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

    // DHT settings (future use)
    bool enable_dht = false;
    uint16_t dht_port = 6881;

    // PEX settings (future use)
    bool enable_pex = false;

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
