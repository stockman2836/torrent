#include "download_manager.h"
#include "logger.h"
#include "config.h"
#include <iostream>
#include <string>
#include <cstring>
#include <optional>

void printUsage(const char* program_name) {
    std::cout << "Usage: " << program_name << " <torrent_file> [options]\n";
    std::cout << "\nArguments:\n";
    std::cout << "  torrent_file              Path to .torrent file\n";
    std::cout << "\nOptions:\n";
    std::cout << "  --config <file>           Path to config file (default: ./config.json)\n";
    std::cout << "  --download-dir <path>     Directory to save downloaded files\n";
    std::cout << "  --max-download <KB/s>     Maximum download speed in KB/s\n";
    std::cout << "  --max-upload <KB/s>       Maximum upload speed in KB/s\n";
    std::cout << "  --port <port>             Listen port\n";
    std::cout << "  --log-level <level>       Log level: trace, debug, info, warn, error\n";
    std::cout << "  --help                    Show this help message\n";
    std::cout << "\nNote: CLI options override config file settings\n";
    std::cout << "\nExamples:\n";
    std::cout << "  " << program_name << " example.torrent\n";
    std::cout << "  " << program_name << " example.torrent --config my_config.json\n";
    std::cout << "  " << program_name << " example.torrent --download-dir ./downloads\n";
    std::cout << "  " << program_name << " example.torrent --max-download 500 --max-upload 100\n";
}

int main(int argc, char* argv[]) {
    std::cout << "=== BitTorrent Client v1.0 ===\n\n";

    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    // Check for help
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            return 0;
        }
    }

    // Parse arguments
    std::string torrent_file = argv[1];
    std::string config_file = "./config.json";

    // CLI overrides (std::optional to detect if user specified them)
    std::optional<std::string> download_dir_cli;
    std::optional<int64_t> max_download_speed_cli;
    std::optional<int64_t> max_upload_speed_cli;
    std::optional<uint16_t> listen_port_cli;
    std::optional<std::string> log_level_cli;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--config" && i + 1 < argc) {
            config_file = argv[++i];
        }
        else if (arg == "--download-dir" && i + 1 < argc) {
            download_dir_cli = argv[++i];
        }
        else if (arg == "--max-download" && i + 1 < argc) {
            int kb_per_sec = std::stoi(argv[++i]);
            max_download_speed_cli = kb_per_sec * 1024;  // Convert KB/s to bytes/sec
        }
        else if (arg == "--max-upload" && i + 1 < argc) {
            int kb_per_sec = std::stoi(argv[++i]);
            max_upload_speed_cli = kb_per_sec * 1024;  // Convert KB/s to bytes/sec
        }
        else if (arg == "--port" && i + 1 < argc) {
            listen_port_cli = static_cast<uint16_t>(std::stoi(argv[++i]));
        }
        else if (arg == "--log-level" && i + 1 < argc) {
            log_level_cli = argv[++i];
        }
        else {
            std::cerr << "Unknown option: " << arg << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    // Load configuration
    torrent::Config config;
    if (!config.loadFromFile(config_file)) {
        std::cout << "Using default configuration\n";
        config = torrent::Config::loadDefault();
    }

    // Merge CLI arguments (CLI overrides config file)
    config.mergeWithCLI(download_dir_cli, max_download_speed_cli,
                        max_upload_speed_cli, listen_port_cli, log_level_cli);

    // Validate configuration
    if (!config.validate()) {
        std::cerr << "Invalid configuration. Exiting.\n";
        return 1;
    }

    // Initialize logger using config
    torrent::Logger::Level console_level = torrent::Logger::Level::INFO;
    if (config.log_level == "trace") console_level = torrent::Logger::Level::TRACE;
    else if (config.log_level == "debug") console_level = torrent::Logger::Level::DEBUG;
    else if (config.log_level == "info") console_level = torrent::Logger::Level::INFO;
    else if (config.log_level == "warn") console_level = torrent::Logger::Level::WARN;
    else if (config.log_level == "error") console_level = torrent::Logger::Level::ERROR;
    else {
        std::cerr << "Invalid log level: " << config.log_level << "\n";
        return 1;
    }

    torrent::Logger::init(config.log_file, console_level, torrent::Logger::Level::DEBUG,
                          config.log_max_size, config.log_max_files);

    try {
        LOG_INFO("=== BitTorrent Client Starting ===");
        LOG_INFO("Loading torrent: {}", torrent_file);
        LOG_INFO("Configuration loaded");

        // Print configuration summary
        config.print();

        std::cout << "Loading torrent: " << torrent_file << "\n";

        std::cout << "\n";

        torrent::DownloadManager manager(torrent_file, config.download_dir, config.listen_port,
                                        config.max_download_speed, config.max_upload_speed,
                                        config.enable_dht);

        std::cout << "Starting download...\n";
        manager.start();

        // Main loop - print status
        while (manager.isRunning() && manager.progress() < 100.0) {
            manager.printStatus();
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        if (manager.progress() >= 100.0) {
            std::cout << "\n=== Download complete! ===\n";
        }

        manager.stop();

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
