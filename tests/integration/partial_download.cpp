#include "torrent/core/config.h"
#include "torrent/core/logger.h"
#include "torrent/metainfo/torrent_file.h"
#include "torrent/session/download_manager.h"
#include <chrono>
#include <iostream>
#include <thread>

// Exercise the public session stop API after a verified piece, then let the
// integration test restart the ordinary CLI against the saved files/state.
int main(int argc, char* argv[]) {
    if (argc != 3) return 2;
    torrent::Config config;
    if (!config.loadFromFile(argv[2])) return 2;
    torrent::Logger::init(config.log_file);
    torrent::DownloadManager download(torrent::TorrentFile::fromFile(argv[1]), config);
    download.start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (download.progress() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto progress = download.progress();
    download.stop();
    if (progress <= 0 || progress >= 100) {
        std::cerr << "Expected a partial download, got " << progress << "%\n";
        return 1;
    }
    return 0;
}
