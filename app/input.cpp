#include "input.h"
#include "torrent/metainfo/magnet_uri.h"
#include "torrent/session/magnet_download_manager.h"
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace torrent::app {

ResolvedInput resolveInput(const std::string& input, const Config& config) {
    if (!MagnetURI::isMagnetURI(input)) return {TorrentFile::fromFile(input), nullptr};

    const auto magnet = MagnetURI::parse(input);
    if (!magnet.isValid()) throw std::invalid_argument("Invalid magnet URI");

    ResolvedInput result;
    if (config.enable_dht) {
        result.dht = std::make_unique<dht::DHTManager>(config.dht_port);
        result.dht->start();
        result.dht->bootstrap({});
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    // Keep callback state alive until the metadata worker has joined.
    std::atomic<bool> ready{false};
    MagnetDownloadManager metadata(magnet, result.dht.get(), config.listen_port);
    metadata.start([&](const TorrentFile& torrent) {
        result.torrent = torrent;
        ready = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (metadata.isRunning() && !ready && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    metadata.stop();
    if (!ready) throw std::runtime_error(std::chrono::steady_clock::now() >= deadline
        ? "Magnet metadata download timed out" : "Failed to download verified magnet metadata");
    return result;
}

} // namespace torrent::app
