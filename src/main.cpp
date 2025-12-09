#include "download_manager.h"
#include "logger.h"
#include <iostream>
#include <string>
#include <cstring>

void printUsage(const char* program_name) {
    std::cout << "Usage: " << program_name << " <torrent_file> [options]\n";
    std::cout << "\nArguments:\n";
    std::cout << "  torrent_file              Path to .torrent file\n";
    std::cout << "\nOptions:\n";
    std::cout << "  --download-dir <path>     Directory to save downloaded files (default: ./downloads)\n";
    std::cout << "  --max-download <KB/s>     Maximum download speed in KB/s (default: unlimited)\n";
    std::cout << "  --max-upload <KB/s>       Maximum upload speed in KB/s (default: unlimited)\n";
    std::cout << "  --port <port>             Listen port (default: 6881)\n";
    std::cout << "  --log-level <level>       Log level: trace, debug, info, warn, error (default: info)\n";
    std::cout << "  --help                    Show this help message\n";
    std::cout << "\nExamples:\n";
    std::cout << "  " << program_name << " example.torrent\n";
    std::cout << "  " << program_name << " example.torrent --download-dir ./downloads\n";
    std::cout << "  " << program_name << " example.torrent --max-download 500 --max-upload 100\n";
    std::cout << "  " << program_name << " example.torrent --max-download 1024\n";
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
    std::string download_dir = "./downloads";
    int64_t max_download_speed = 0;  // 0 = unlimited (bytes/sec)
    int64_t max_upload_speed = 0;    // 0 = unlimited (bytes/sec)
    uint16_t listen_port = 6881;
    std::string log_level = "info";

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--download-dir" && i + 1 < argc) {
            download_dir = argv[++i];
        }
        else if (arg == "--max-download" && i + 1 < argc) {
            int kb_per_sec = std::stoi(argv[++i]);
            max_download_speed = kb_per_sec * 1024;  // Convert KB/s to bytes/sec
        }
        else if (arg == "--max-upload" && i + 1 < argc) {
            int kb_per_sec = std::stoi(argv[++i]);
            max_upload_speed = kb_per_sec * 1024;  // Convert KB/s to bytes/sec
        }
        else if (arg == "--port" && i + 1 < argc) {
            listen_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        }
        else if (arg == "--log-level" && i + 1 < argc) {
            log_level = argv[++i];
        }
        else {
            std::cerr << "Unknown option: " << arg << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    // Initialize logger
    torrent::Logger::Level console_level = torrent::Logger::Level::INFO;
    if (log_level == "trace") console_level = torrent::Logger::Level::TRACE;
    else if (log_level == "debug") console_level = torrent::Logger::Level::DEBUG;
    else if (log_level == "info") console_level = torrent::Logger::Level::INFO;
    else if (log_level == "warn") console_level = torrent::Logger::Level::WARN;
    else if (log_level == "error") console_level = torrent::Logger::Level::ERROR;
    else {
        std::cerr << "Invalid log level: " << log_level << "\n";
        return 1;
    }

    torrent::Logger::init("torrent_client.log", console_level, torrent::Logger::Level::DEBUG);

    try {
        LOG_INFO("=== BitTorrent Client Starting ===");
        LOG_INFO("Loading torrent: {}", torrent_file);
        LOG_INFO("Download directory: {}", download_dir);
        LOG_INFO("Listen port: {}", listen_port);

        std::cout << "Loading torrent: " << torrent_file << "\n";
        std::cout << "Download directory: " << download_dir << "\n";
        std::cout << "Listen port: " << listen_port << "\n";

        if (max_download_speed > 0) {
            std::cout << "Max download speed: " << (max_download_speed / 1024) << " KB/s\n";
        } else {
            std::cout << "Max download speed: unlimited\n";
        }

        if (max_upload_speed > 0) {
            std::cout << "Max upload speed: " << (max_upload_speed / 1024) << " KB/s\n";
        } else {
            std::cout << "Max upload speed: unlimited\n";
        }

        std::cout << "\n";

        torrent::DownloadManager manager(torrent_file, download_dir, listen_port,
                                        max_download_speed, max_upload_speed);

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
