#include "download_manager.h"
#include <iostream>
#include <string>

void printUsage(const char* program_name) {
    std::cout << "Usage: " << program_name << " <torrent_file> [download_dir]\n";
    std::cout << "\nOptions:\n";
    std::cout << "  torrent_file   Path to .torrent file\n";
    std::cout << "  download_dir   Directory to save downloaded files (default: ./downloads)\n";
    std::cout << "\nExample:\n";
    std::cout << "  " << program_name << " example.torrent ./downloads\n";
}

int main(int argc, char* argv[]) {
    std::cout << "=== BitTorrent Client v1.0 ===\n\n";

    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    std::string torrent_file = argv[1];
    std::string download_dir = (argc >= 3) ? argv[2] : "./downloads";

    try {
        std::cout << "Loading torrent: " << torrent_file << "\n";
        std::cout << "Download directory: " << download_dir << "\n\n";

        torrent::DownloadManager manager(torrent_file, download_dir);

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
