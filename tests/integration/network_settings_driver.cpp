#include "download_queue.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <functional>
#include <iostream>
#include <sstream>
#include <thread>

using namespace torrent;
using namespace torrent::app;

namespace {
struct QuietOutput {
    struct Buffer : std::streambuf {
        int_type overflow(int_type c) override { return traits_type::not_eof(c); }
        std::streamsize xsputn(const char*, std::streamsize size) override { return size; }
    } buffer;
    std::streambuf* previous = std::cout.rdbuf(&buffer);
    ~QuietOutput() { std::cout.rdbuf(previous); }
};
void until(DownloadQueue& queue, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    do {
        queue.tick();
        if (done()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("Network settings fixture timed out");
}
}

int main(int argc, char* argv[]) {
    try {
        if (argc != 3 && argc != 4) return 2;
        Config config;
        if (!config.loadFromFile(argv[1]) || !config.validate()) return 2;
        std::ostream output(std::cout.rdbuf());
        QuietOutput quiet;
        auto queue = std::make_unique<DownloadQueue>();
        queue->setNetworkSettings(NetworkSettings::fromConfig(config));
        for (int i = 2; i < argc; ++i) {
            auto initial = config; initial.download_dir = "./files-" + std::to_string(i - 2);
            queue->add(argv[i], initial);
        }
        queue->setLimit(2);
        const auto ready = [&] {
            for (const auto& entry : queue->snapshots()) {
                if (entry.busy) return false;
                if (!entry.download.error.empty()) throw std::runtime_error(entry.download.error);
            }
            return true;
        };
        until(*queue, ready);
        output << "READY\n" << std::flush;
        std::string line;
        while (std::getline(std::cin, line) && line != "quit") {
            std::istringstream input(line); std::string command; input >> command;
            if (command == "start" || command == "stop") {
                size_t index; if (!(input >> index)) throw std::runtime_error("Missing torrent index");
                const auto id = queue->snapshots().at(index).id;
                if (command == "start") {
                    if (!queue->enqueue(id)) throw std::runtime_error("Cannot enqueue torrent");
                    queue->tick();
                } else {
                    queue->stop(id);
                    until(*queue, [&] { return !queue->snapshots().at(index).busy; });
                }
                output << "OK\n" << std::flush;
            } else if (command == "network") {
                NetworkSettings settings;
                int dht, pex;
                if (!(input >> settings.first_port >> settings.max_peers >> dht >> pex)) throw std::runtime_error("Missing network settings");
                settings.enable_dht = dht != 0; settings.enable_pex = pex != 0;
                try {
                    settings.validate(queue->snapshots().size());
                    auto updated = config; settings.applyTo(updated);
                    if (!updated.saveToFile("settings.json")) throw std::runtime_error("Cannot save network settings");
                    queue->setNetworkSettings(settings); config = std::move(updated);
                    output << "OK\n" << std::flush;
                } catch (const std::invalid_argument&) { output << "REJECTED\n" << std::flush; }
            } else if (command == "restore") {
                queue->save("list.json"); queue->close(); until(*queue, [&] { return queue->closed(); });
                queue = std::make_unique<DownloadQueue>();
                if (!config.loadFromFile("settings.json")) throw std::runtime_error("Cannot restore preferences");
                queue->setNetworkSettings(NetworkSettings::fromConfig(config)); queue->restore("list.json", config);
                until(*queue, ready); output << "OK\n" << std::flush;
            } else if (command == "status") {
                queue->tick(); auto states = nlohmann::json::array();
                for (const auto& entry : queue->snapshots()) states.push_back({
                    {"busy", entry.busy}, {"queued", entry.queued}, {"progress", entry.download.progress}, {"error", entry.download.error},
                    {"port", entry.download.listen_port}, {"dht", entry.download.network.dht_enabled},
                    {"nodes", entry.download.network.dht_nodes}, {"peers", entry.download.network.peers.size()},
                    {"config", {{"port", entry.config.listen_port}, {"udp", entry.config.dht_port}, {"max_peers", entry.config.max_peers},
                        {"dht", entry.config.enable_dht}, {"pex", entry.config.enable_pex}}}});
                output << "STATUS " << states.dump() << '\n' << std::flush;
            } else throw std::runtime_error("Unknown fixture command");
        }
        queue->close(); until(*queue, [&] { return queue->closed(); });
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
