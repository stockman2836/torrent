#include "download_manager.h"
#include "magnet_uri.h"
#include "magnet_download_manager.h"
#include "logger.h"
#include "config.h"
#include <iostream>
#include <string>
#include <cstring>
#include <optional>

void printUsage(const char* program_name) {
    std::cout << "Usage: " << program_name << " <torrent_file|magnet_uri> [options]\n";
    std::cout << "\nArguments:\n";
    std::cout << "  torrent_file              Path to .torrent file\n";
    std::cout << "  magnet_uri                Magnet link (magnet:?...)\n";
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
    std::cout << "  " << program_name << " \"magnet:?xt=urn:btih:...\"\n";
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
    std::string torrent_input = argv[1];  // Can be .torrent file or magnet URI
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
        LOG_INFO("Input: {}", torrent_input);
        LOG_INFO("Configuration loaded");

        // Print configuration summary
        config.print();

        // Check if input is a magnet link or torrent file
        if (torrent::MagnetURI::isMagnetURI(torrent_input)) {
            std::cout << "Detected magnet link\n";

            // Parse magnet URI
            torrent::MagnetURI magnet_uri = torrent::MagnetURI::parse(torrent_input);

            if (!magnet_uri.isValid()) {
                std::cerr << "Invalid magnet URI\n";
                return 1;
            }

            std::cout << "Magnet Link Info:\n";
            std::cout << "  Info Hash: " << magnet_uri.infoHashHex() << "\n";
            if (!magnet_uri.displayName().empty()) {
                std::cout << "  Name: " << magnet_uri.displayName() << "\n";
            }
            std::cout << "  Trackers: " << magnet_uri.trackers().size() << "\n\n";

            // Initialize DHT if enabled
            std::unique_ptr<torrent::dht::DHTManager> dht_manager;
            if (config.enable_dht) {
                std::cout << "Initializing DHT...\n";
                dht_manager = std::make_unique<torrent::dht::DHTManager>(
                    config.listen_port,
                    torrent::utils::generatePeerId()
                );
                dht_manager->start();
                LOG_INFO("DHT initialized with {} bootstrap nodes", dht_manager->getNodeCount());

                // Give DHT some time to bootstrap
                std::cout << "Bootstrapping DHT (waiting 5 seconds)...\n";
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }

            // Download metadata using MagnetDownloadManager
            std::cout << "Starting metadata download...\n";
            torrent::MagnetDownloadManager magnet_manager(
                magnet_uri,
                dht_manager.get(),
                config.listen_port
            );

            // Flag to track if metadata was downloaded
            std::atomic<bool> metadata_ready(false);
            torrent::TorrentFile downloaded_torrent;

            // Start metadata download
            magnet_manager.start([&](const torrent::TorrentFile& torrent_file) {
                downloaded_torrent = torrent_file;
                metadata_ready = true;
                std::cout << "\n=== Metadata Downloaded Successfully ===\n";
                downloaded_torrent.printInfo();
                std::cout << "\n";
            });

            // Wait for metadata download (with timeout)
            auto start_time = std::chrono::steady_clock::now();
            constexpr int timeout_seconds = 60;

            while (magnet_manager.isRunning()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));

                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - start_time
                ).count();

                if (elapsed > timeout_seconds) {
                    std::cerr << "Timeout: Failed to download metadata within " << timeout_seconds << " seconds\n";
                    magnet_manager.stop();
                    return 1;
                }

                if (metadata_ready) {
                    break;
                }
            }

            if (!metadata_ready) {
                std::cerr << "Failed to download metadata\n";
                return 1;
            }

            // Now transition to regular download with the downloaded TorrentFile
            std::cout << "Starting piece download...\n";
            torrent::DownloadManager manager(downloaded_torrent, config.download_dir, config.listen_port,
                                            config.max_download_speed, config.max_upload_speed,
                                            config.enable_dht, std::move(dht_manager));

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
        }
        else {
            // Regular .torrent file
            std::cout << "Loading torrent file: " << torrent_input << "\n\n";

            torrent::DownloadManager manager(torrent_input, config.download_dir, config.listen_port,
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
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
