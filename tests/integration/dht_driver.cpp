#include "download_task.h"
#include "torrent/network/dht/dht_manager.h"
#include "torrent/core/utils.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

int main(int argc, char* argv[]) {
    try {
        if (argc != 4) return 2;
        torrent::Config config;
        if (!config.loadFromFile(argv[1]) || !config.validate()) return 2;
        const std::string mode = argv[2];
        std::string command;
        if (mode == "metadata-cancel") {
            torrent::app::DownloadTask task;
            task.load(argv[3], config);
            std::cout << "DHT_READY\n" << std::flush;
            std::getline(std::cin, command); task.stop(); task.wait();
            if (task.snapshot().state != torrent::app::DownloadState::Stopped) throw std::runtime_error("Metadata did not stop");
            task.unload(); task.wait();
        } else {
            torrent::dht::DHTManager dht(config.dht_port);
            dht.start(); dht.bootstrap(config.dht_bootstrap_nodes);
            const auto hash = torrent::utils::fromHex(argv[3]);
            if (mode == "late") {
                if (!dht.getPeers(hash, 1).empty()) throw std::runtime_error("Expected first lookup to time out");
                std::cout << "DHT_TIMEOUT\n" << std::flush;
            }
            std::atomic<bool> cancelled{false};
            std::vector<torrent::dht::DHTpeer> peers;
            std::thread lookup([&] { peers = dht.getPeers(hash, 10, [&] { return cancelled.load(); }); });
            std::cout << "DHT_READY\n" << std::flush;
            if (mode == "cancel") {
                std::getline(std::cin, command); cancelled = true;
            }
            lookup.join();
            for (const auto& peer : peers) std::cout << "DHT_PEER " << peer.ip << ' ' << peer.port << '\n';
            if (mode != "cancel") {
                if (peers.size() != 1) throw std::runtime_error("Expected one unique DHT peer");
                dht.announcePeer(hash, config.listen_port);
                std::cout << "DHT_FOUND\n" << std::flush;
                std::getline(std::cin, command);
            }
            dht.stop();
        }
        std::cout << "DHT_STOPPED\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
