#include "torrent/core/config.h"
#include "torrent/core/logger.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <cmath>
#include <iostream>
#include <limits>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using json = nlohmann::json;

namespace torrent {

bool Config::loadFromFile(const std::string& filepath) {
    try {
        std::ifstream file(std::filesystem::u8path(filepath));
        if (!file.is_open()) {
            LOG_WARN("Config file not found: {}. Using defaults.", filepath);
            return false;
        }

        json j;
        file >> j;

        LOG_INFO("Loading configuration from: {}", filepath);

        // Network settings
        if (j.contains("listen_port")) listen_port = j["listen_port"].get<uint16_t>();
        if (j.contains("max_peers")) max_peers = j["max_peers"].get<int>();
        if (j.contains("max_connections")) max_connections = j["max_connections"].get<int>();
        if (j.contains("enable_port_mapping")) enable_port_mapping = j["enable_port_mapping"].get<bool>();

        // Speed limits
        if (j.contains("max_download_speed")) {
            max_download_speed = j["max_download_speed"].get<int64_t>();
        }
        if (j.contains("max_upload_speed")) {
            max_upload_speed = j["max_upload_speed"].get<int64_t>();
        }
        const auto globalSpeed = [&j](const char* key, int64_t fallback) {
            if (!j.contains(key)) return fallback;
            const auto& value = j.at(key);
            if (!value.is_number_integer() || (value.is_number_unsigned() &&
                value.get<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())))
                throw std::invalid_argument("Invalid global speed limit");
            const auto speed = value.get<int64_t>();
            if (speed < 0) throw std::invalid_argument("Invalid global speed limit");
            return speed;
        };
        global_max_download_speed = globalSpeed("global_max_download_speed", global_max_download_speed);
        global_max_upload_speed = globalSpeed("global_max_upload_speed", global_max_upload_speed);

        // Download settings
        if (j.contains("download_dir")) download_dir = j["download_dir"].get<std::string>();
        if (j.contains("sequential_download")) {
            sequential_download = j["sequential_download"].get<bool>();
        }
        if (j.contains("piece_timeout_seconds")) {
            piece_timeout_seconds = j["piece_timeout_seconds"].get<int>();
        }

        // Upload/Seeding settings
        if (j.contains("seed_after_download")) {
            seed_after_download = j["seed_after_download"].get<bool>();
        }
        if (j.contains("seed_ratio_limit")) {
            seed_ratio_limit = j["seed_ratio_limit"].get<double>();
        }
        if (j.contains("seed_time_limit_hours")) {
            seed_time_limit_hours = j["seed_time_limit_hours"].get<int>();
        }

        // Tracker settings
        if (j.contains("tracker_announce_interval")) {
            tracker_announce_interval = j["tracker_announce_interval"].get<int>();
        }
        if (j.contains("tracker_timeout_seconds")) {
            tracker_timeout_seconds = j["tracker_timeout_seconds"].get<int>();
        }
        if (j.contains("tracker_max_retries")) {
            tracker_max_retries = j["tracker_max_retries"].get<int>();
        }

        // Logging settings
        if (j.contains("log_level")) log_level = j["log_level"].get<std::string>();
        if (j.contains("log_file")) log_file = j["log_file"].get<std::string>();
        if (j.contains("log_max_size")) log_max_size = j["log_max_size"].get<int64_t>();
        if (j.contains("log_max_files")) log_max_files = j["log_max_files"].get<int>();

        // Resume capability
        if (j.contains("enable_resume")) enable_resume = j["enable_resume"].get<bool>();
        if (j.contains("resume_dir")) resume_dir = j["resume_dir"].get<std::string>();

        // DHT settings
        if (j.contains("enable_dht")) enable_dht = j["enable_dht"].get<bool>();
        if (j.contains("dht_port")) dht_port = j["dht_port"].get<uint16_t>();
        if (j.contains("dht_bootstrap_nodes")) {
            if (!j["dht_bootstrap_nodes"].is_array()) throw std::invalid_argument("dht_bootstrap_nodes must be an array");
            std::vector<DhtBootstrapNode> nodes;
            for (const auto& node : j["dht_bootstrap_nodes"]) {
                const auto host = node.at("host").get<std::string>();
                if (!node.at("port").is_number_integer()) throw std::invalid_argument("DHT bootstrap port must be an integer");
                const auto port = node.at("port").get<int64_t>();
                if (host.empty() || port < 1 || port > 65535) throw std::invalid_argument("Invalid DHT bootstrap address");
                nodes.emplace_back(host, static_cast<uint16_t>(port));
            }
            dht_bootstrap_nodes = std::move(nodes);
        }

        // PEX settings
        if (j.contains("enable_pex")) enable_pex = j["enable_pex"].get<bool>();

        // Encryption settings
        if (j.contains("enable_encryption")) enable_encryption = j["enable_encryption"].get<bool>();
        if (j.contains("encryption_mode")) encryption_mode = j["encryption_mode"].get<std::string>();
        if (j.contains("allow_legacy_peers")) allow_legacy_peers = j["allow_legacy_peers"].get<bool>();

        // IPv6 settings
        if (j.contains("enable_ipv6")) enable_ipv6 = j["enable_ipv6"].get<bool>();
        if (j.contains("ip_version")) ip_version = j["ip_version"].get<std::string>();

        // LSD settings
        if (j.contains("enable_lsd")) enable_lsd = j["enable_lsd"].get<bool>();

        // Web Seeding settings
        if (j.contains("enable_webseeds")) enable_webseeds = j["enable_webseeds"].get<bool>();

        // uTP settings
        if (j.contains("enable_utp")) enable_utp = j["enable_utp"].get<bool>();
        if (j.contains("prefer_utp")) prefer_utp = j["prefer_utp"].get<bool>();

        LOG_INFO("Configuration loaded successfully");
        return true;

    } catch (const json::exception& e) {
        LOG_ERROR("Failed to parse config file: {}", e.what());
        return false;
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to load config file: {}", e.what());
        return false;
    }
}

bool Config::saveToFile(const std::string& filepath) const {
    try {
        json j;

        // Network settings
        j["listen_port"] = listen_port;
        j["max_peers"] = max_peers;
        j["max_connections"] = max_connections;
        j["enable_port_mapping"] = enable_port_mapping;

        // Speed limits
        j["max_download_speed"] = max_download_speed;
        j["max_upload_speed"] = max_upload_speed;
        j["global_max_download_speed"] = global_max_download_speed;
        j["global_max_upload_speed"] = global_max_upload_speed;

        // Download settings
        j["download_dir"] = download_dir;
        j["sequential_download"] = sequential_download;
        j["piece_timeout_seconds"] = piece_timeout_seconds;

        // Upload/Seeding settings
        j["seed_after_download"] = seed_after_download;
        j["seed_ratio_limit"] = seed_ratio_limit;
        j["seed_time_limit_hours"] = seed_time_limit_hours;

        // Tracker settings
        j["tracker_announce_interval"] = tracker_announce_interval;
        j["tracker_timeout_seconds"] = tracker_timeout_seconds;
        j["tracker_max_retries"] = tracker_max_retries;

        // Logging settings
        j["log_level"] = log_level;
        j["log_file"] = log_file;
        j["log_max_size"] = log_max_size;
        j["log_max_files"] = log_max_files;

        // Resume capability
        j["enable_resume"] = enable_resume;
        j["resume_dir"] = resume_dir;

        // DHT settings
        j["enable_dht"] = enable_dht;
        j["dht_port"] = dht_port;
        j["dht_bootstrap_nodes"] = json::array();
        for (const auto& node : dht_bootstrap_nodes)
            j["dht_bootstrap_nodes"].push_back({{"host", node.host}, {"port", node.port}});

        // PEX settings
        j["enable_pex"] = enable_pex;

        // Encryption settings
        j["enable_encryption"] = enable_encryption;
        j["encryption_mode"] = encryption_mode;
        j["allow_legacy_peers"] = allow_legacy_peers;

        // IPv6 settings
        j["enable_ipv6"] = enable_ipv6;
        j["ip_version"] = ip_version;

        // LSD settings
        j["enable_lsd"] = enable_lsd;

        // Web Seeding settings
        j["enable_webseeds"] = enable_webseeds;

        // uTP settings
        j["enable_utp"] = enable_utp;
        j["prefer_utp"] = prefer_utp;

        const auto destination = std::filesystem::u8path(filepath);
        auto temporary = destination; temporary += ".tmp";
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            LOG_ERROR("Failed to open config file for writing: {}", filepath);
            return false;
        }

        file << j.dump(4) << '\n';
        file.close();
        if (!file) return false;
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return false;
#else
        std::filesystem::rename(temporary, destination);
#endif
        LOG_INFO("Configuration saved to: {}", filepath);
        return true;

    } catch (const std::exception& e) {
        LOG_ERROR("Failed to save config file: {}", e.what());
        return false;
    }
}

Config Config::loadDefault() {
    Config config;
    LOG_DEBUG("Using default configuration");
    return config;
}

void Config::mergeWithCLI(const std::optional<std::string>& download_dir_cli,
                          const std::optional<int64_t>& max_download_speed_cli,
                          const std::optional<int64_t>& max_upload_speed_cli,
                          const std::optional<uint16_t>& listen_port_cli,
                          const std::optional<std::string>& log_level_cli) {
    if (download_dir_cli.has_value()) {
        download_dir = download_dir_cli.value();
        LOG_DEBUG("CLI override: download_dir = {}", download_dir);
    }

    if (max_download_speed_cli.has_value()) {
        max_download_speed = max_download_speed_cli.value();
        LOG_DEBUG("CLI override: max_download_speed = {}", max_download_speed);
    }

    if (max_upload_speed_cli.has_value()) {
        max_upload_speed = max_upload_speed_cli.value();
        LOG_DEBUG("CLI override: max_upload_speed = {}", max_upload_speed);
    }

    if (listen_port_cli.has_value()) {
        listen_port = listen_port_cli.value();
        LOG_DEBUG("CLI override: listen_port = {}", listen_port);
    }

    if (log_level_cli.has_value()) {
        log_level = log_level_cli.value();
        LOG_DEBUG("CLI override: log_level = {}", log_level);
    }
}

bool Config::validate() const {
    bool valid = true;
    for (const auto& node : dht_bootstrap_nodes) if (node.host.empty() || !node.port) {
        LOG_ERROR("Invalid DHT bootstrap address"); valid = false;
    }

    if (listen_port == 0 || listen_port > 65535) {
        LOG_ERROR("Invalid listen_port: {}", listen_port);
        valid = false;
    }

    if (max_peers <= 0 || max_peers > 1000) {
        LOG_ERROR("Invalid max_peers: {}", max_peers);
        valid = false;
    }

    if (max_connections <= 0 || max_connections > 10000) {
        LOG_ERROR("Invalid max_connections: {}", max_connections);
        valid = false;
    }

    if (max_download_speed < 0) {
        LOG_ERROR("Invalid max_download_speed: {}", max_download_speed);
        valid = false;
    }

    if (max_upload_speed < 0) {
        LOG_ERROR("Invalid max_upload_speed: {}", max_upload_speed);
        valid = false;
    }

    if (global_max_download_speed < 0 || global_max_upload_speed < 0) {
        LOG_ERROR("Global speed limits must be non-negative");
        valid = false;
    }

    if (!std::isfinite(seed_ratio_limit) || seed_ratio_limit < 0) {
        LOG_ERROR("Invalid seed_ratio_limit: {}", seed_ratio_limit);
        valid = false;
    }

    if (seed_time_limit_hours < 0) {
        LOG_ERROR("Invalid seed_time_limit_hours: {}", seed_time_limit_hours);
        valid = false;
    }

    if (log_level != "trace" && log_level != "debug" && log_level != "info" &&
        log_level != "warn" && log_level != "error") {
        LOG_ERROR("Invalid log_level: {}", log_level);
        valid = false;
    }

    return valid;
}

void Config::print() const {
    std::cout << "\n=== Configuration ===\n";
    std::cout << "Network:\n";
    std::cout << "  Listen port: " << listen_port << "\n";
    std::cout << "  Max peers: " << max_peers << "\n";
    std::cout << "  Max connections: " << max_connections << "\n";

    std::cout << "\nSpeed Limits:\n";
    if (max_download_speed > 0) {
        std::cout << "  Max download: " << (max_download_speed / 1024) << " KB/s\n";
    } else {
        std::cout << "  Max download: unlimited\n";
    }
    if (max_upload_speed > 0) {
        std::cout << "  Max upload: " << (max_upload_speed / 1024) << " KB/s\n";
    } else {
        std::cout << "  Max upload: unlimited\n";
    }

    std::cout << "\nDownload Settings:\n";
    std::cout << "  Download directory: " << download_dir << "\n";
    std::cout << "  Sequential download: " << (sequential_download ? "yes" : "no") << "\n";
    std::cout << "  Piece timeout: " << piece_timeout_seconds << " seconds\n";

    std::cout << "\nSeeding Settings:\n";
    std::cout << "  Seed after download: " << (seed_after_download ? "yes" : "no") << "\n";
    if (seed_ratio_limit > 0) {
        std::cout << "  Seed ratio limit: " << seed_ratio_limit << "\n";
    } else {
        std::cout << "  Seed ratio limit: unlimited\n";
    }
    if (seed_time_limit_hours > 0) {
        std::cout << "  Seed time limit: " << seed_time_limit_hours << " hours\n";
    } else {
        std::cout << "  Seed time limit: unlimited\n";
    }

    std::cout << "\nTracker Settings:\n";
    std::cout << "  Announce interval: " << tracker_announce_interval << " seconds\n";
    std::cout << "  Timeout: " << tracker_timeout_seconds << " seconds\n";
    std::cout << "  Max retries: " << tracker_max_retries << "\n";

    std::cout << "\nLogging:\n";
    std::cout << "  Log level: " << log_level << "\n";
    std::cout << "  Log file: " << log_file << "\n";
    std::cout << "  Log max size: " << (log_max_size / 1024 / 1024) << " MB\n";
    std::cout << "  Log max files: " << log_max_files << "\n";

    std::cout << "\nAdvanced:\n";
    std::cout << "  Resume enabled: " << (enable_resume ? "yes" : "no") << "\n";
    std::cout << "  DHT enabled: " << (enable_dht ? "yes" : "no") << "\n";
    std::cout << "  PEX enabled: " << (enable_pex ? "yes" : "no") << "\n";
    std::cout << "  LSD enabled: " << (enable_lsd ? "yes" : "no") << "\n";
    std::cout << "  Web Seeding enabled: " << (enable_webseeds ? "yes" : "no") << "\n";
    std::cout << "  uTP enabled: " << (enable_utp ? "yes" : "no") << "\n";
    if (enable_utp) {
        std::cout << "  Prefer uTP: " << (prefer_utp ? "yes" : "no") << "\n";
    }

    std::cout << "\nEncryption (MSE/PE):\n";
    std::cout << "  Encryption enabled: " << (enable_encryption ? "yes" : "no") << "\n";
    std::cout << "  Encryption mode: " << encryption_mode << "\n";
    std::cout << "  Allow legacy peers: " << (allow_legacy_peers ? "yes" : "no") << "\n";

    std::cout << "\nIPv6 Support:\n";
    std::cout << "  IPv6 enabled: " << (enable_ipv6 ? "yes" : "no") << "\n";
    std::cout << "  IP version preference: " << ip_version << "\n";
    std::cout << "====================\n\n";
}

} // namespace torrent
