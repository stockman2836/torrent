#include "download_queue.h"
#include "network_diagnostics.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <thread>

using namespace torrent;
using namespace torrent::app;
using json = nlohmann::json;
namespace {
struct Quiet {
    struct Buffer : std::streambuf {
        int_type overflow(int_type c) override { return traits_type::not_eof(c); }
        std::streamsize xsputn(const char*, std::streamsize n) override { return n; }
    } buffer;
    std::streambuf* old = std::cout.rdbuf(&buffer);
    ~Quiet() { std::cout.rdbuf(old); }
};
void until(DownloadQueue& queue, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do { queue.tick(); if (done()) return; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("Port mapping driver timed out");
}
const char* state(MappingState state) {
    switch (state) {
    case MappingState::Discovering: return "discovering";
    case MappingState::Mapping: return "mapping";
    case MappingState::Active: return "active";
    case MappingState::Partial: return "partial";
    case MappingState::Error: return "error";
    case MappingState::Removing: return "removing";
    case MappingState::Stopped: return "stopped";
    default: return "disabled";
    }
}
}
int main(int argc, char* argv[]) {
    try {
        if (argc < 4 || argc > 5) return 2;
        Config config; if (!config.loadFromFile(argv[1]) || !config.validate()) return 2;
        json options; std::ifstream(argv[2]) >> options;
        if (options.at("gateway") != "127.0.0.1") throw std::runtime_error("Fixtures require a loopback router");
        config.port_mapping_options.gateway = "127.0.0.1";
        config.port_mapping_options.discovery_address = "127.0.0.1";
        config.port_mapping_options.natpmp_port = options.at("natpmp_port").get<uint16_t>();
        config.port_mapping_options.discovery_port = options.at("discovery_port").get<uint16_t>();
        config.port_mapping_options.lease_seconds = options.value("lease", 2u);
        config.port_mapping_options.request_timeout_ms = options.value("request_timeout_ms", 250);
        config.port_mapping_options.discovery_timeout_ms = options.value("discovery_timeout_ms", 300);
        config.port_mapping_options.retry_delay_ms = options.value("retry_delay_ms", 1000);
        std::ostream output(std::cout.rdbuf()); Quiet quiet;
        auto queue = std::make_unique<DownloadQueue>();
        queue->setLimit(2); queue->setNetworkSettings(NetworkSettings::fromConfig(config));
        for (int i = 3; i < argc; ++i) { auto job = config; job.download_dir = "files-" + std::to_string(i - 3); queue->add(argv[i], job); }
        const auto ready = [&] { for (const auto& job : queue->snapshots()) if (job.busy) return false; return true; };
        until(*queue, ready); output << "READY\n" << std::flush;
        std::string line;
        while (std::getline(std::cin, line) && line != "quit") {
            std::istringstream input(line); std::string command; input >> command;
            if (command == "start" || command == "stop") {
                size_t index; if (!(input >> index)) return 2;
                const auto id = queue->snapshots().at(index).id;
                if (command == "start") { if (!queue->enqueue(id)) throw std::runtime_error("Cannot enqueue"); queue->tick(); }
                else { queue->stop(id); until(*queue, [&] { return !queue->snapshots().at(index).busy; }); }
                output << "OK\n" << std::flush;
            } else if (command == "mapping") {
                int enabled; if (!(input >> enabled)) return 2;
                config.enable_port_mapping = enabled != 0;
                if (!config.saveToFile("saved-settings.json") || !queue->setNetworkSettings(NetworkSettings::fromConfig(config))) return 2;
                output << "OK\n" << std::flush;
            } else if (command == "restore") {
                queue->save("list.json"); queue->close(); until(*queue, [&] { return queue->closed(); });
                queue = std::make_unique<DownloadQueue>(); queue->setNetworkSettings(NetworkSettings::fromConfig(config));
                queue->restore("list.json", config); until(*queue, ready); output << "OK\n" << std::flush;
            } else if (command == "status") {
                queue->tick(); auto states = json::array();
                for (const auto& job : queue->snapshots()) {
                    const auto& mapping = job.download.network.port_mapping;
                    auto ports = json::array();
                    for (const auto& port : mapping.ports) ports.push_back({{"transport", port.transport == PortTransport::Tcp ? "TCP" : "UDP"},
                        {"internal", port.internal_port}, {"external", port.external_port}, {"lease", port.lease_seconds},
                        {"active", port.active}, {"error", port.error}});
                    states.push_back({{"busy", job.busy}, {"queued", job.queued}, {"error", job.download.error}, {"progress", job.download.progress},
                        {"port", job.download.listen_port}, {"listener", job.download.network.listener.state == ListenerState::Listening},
                        {"mapping", {{"enabled", mapping.enabled}, {"state", state(mapping.state)},
                            {"protocol", mapping.protocol == MappingProtocol::NatPmp ? "NAT-PMP" : mapping.protocol == MappingProtocol::Upnp ? "UPnP" : "none"},
                            {"error", mapping.error}, {"address", mapping.external_address}, {"ports", ports}}},
                        {"report", networkReport(job.download)}});
                }
                output << "STATUS " << states.dump() << '\n' << std::flush;
            } else return 2;
        }
        queue->close(); until(*queue, [&] { return queue->closed(); });
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
