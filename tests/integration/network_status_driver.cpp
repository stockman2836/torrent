#include "download_task.h"
#include <nlohmann/json.hpp>
#include <iostream>

namespace {
struct QuietOutput {
    struct Buffer : std::streambuf {
        int_type overflow(int_type c) override { return traits_type::not_eof(c); }
        std::streamsize xsputn(const char*, std::streamsize size) override { return size; }
    } buffer;
    std::streambuf* previous = std::cout.rdbuf(&buffer);
    ~QuietOutput() { std::cout.rdbuf(previous); }
};
const char* peerState(torrent::PeerState state) {
    switch (state) {
    case torrent::PeerState::Connecting: return "connecting";
    case torrent::PeerState::Handshaking: return "handshaking";
    default: return "connected";
    }
}
const char* trackerState(torrent::TrackerState state) {
    switch (state) {
    case torrent::TrackerState::Waiting: return "waiting";
    case torrent::TrackerState::Announcing: return "announcing";
    case torrent::TrackerState::Working: return "working";
    case torrent::TrackerState::Error: return "error";
    default: return "none";
    }
}
}

int main(int argc, char* argv[]) {
    try {
        if (argc != 3) return 2;
        torrent::Config config;
        if (!config.loadFromFile(argv[2]) || !config.validate()) return 2;
        std::ostream output(std::cout.rdbuf());
        QuietOutput quiet;
        torrent::app::DownloadTask task;
        task.load(argv[1], config); task.wait();
        output << "READY\n" << std::flush;
        std::string command;
        while (std::getline(std::cin, command) && command != "quit") {
            if (command == "start") {
                if (!task.start(config)) throw std::runtime_error("Cannot start task");
                output << "STARTED\n" << std::flush;
            } else if (command == "stop") {
                task.stop(); task.wait(); output << "STOPPED\n" << std::flush;
            } else if (command == "reload") {
                if (!task.load(argv[1], config)) throw std::runtime_error("Cannot reload task");
                task.wait(); output << "RELOADED\n" << std::flush;
            } else if (command == "status") {
                const auto state = task.snapshot();
                const auto& network = state.network;
                auto peers = nlohmann::json::array();
                for (const auto& peer : network.peers) peers.push_back({{"ip", peer.ip}, {"port", peer.port},
                    {"state", peerState(peer.state)}, {"incoming", peer.incoming}, {"choked", peer.choked},
                    {"pending", peer.pending_requests}, {"downloaded", peer.downloaded_bytes}, {"uploaded", peer.uploaded_bytes}});
                output << "STATUS " << nlohmann::json{{"busy", task.busy()}, {"progress", state.progress},
                    {"error", state.error}, {"uploaded", state.uploaded_bytes}, {"port", state.listen_port},
                    {"peers", peers}, {"queued", network.queued_peers}, {"failed", network.failed_connections},
                    {"peer_error", network.last_peer_error}, {"dht", network.dht_enabled}, {"nodes", network.dht_nodes},
                    {"tracker", {{"url", network.tracker.url}, {"state", trackerState(network.tracker.state)},
                        {"peers", network.tracker.peers_returned}, {"failures", network.tracker.consecutive_failures},
                        {"error", network.tracker.error}}}}.dump() << '\n' << std::flush;
            } else throw std::runtime_error("Unknown command");
        }
        task.stop(); task.wait();
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
