#include "download_task.h"
#include "network_diagnostics.h"
#include "tracker_diagnostics.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <fstream>
#include <mutex>

namespace {
struct QuietOutput {
    struct Buffer : std::streambuf {
        std::ofstream log{"engine.log"};
        std::mutex mutex;
        int_type overflow(int_type c) override {
            if (!traits_type::eq_int_type(c, traits_type::eof())) {
                const char value = traits_type::to_char_type(c); xsputn(&value, 1);
            }
            return traits_type::not_eof(c);
        }
        std::streamsize xsputn(const char* data, std::streamsize size) override {
            std::lock_guard<std::mutex> lock(mutex);
            log.write(data, size); log.flush(); return size;
        }
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
            } else if (command == "update") {
                output << "UPDATED " << (task.requestTrackerUpdate() ? 1 : 0) << '\n' << std::flush;
            } else if (command == "status") {
                const auto state = task.snapshot();
                const auto& network = state.network;
                const char* listener = "not-started";
                switch (network.listener.state) {
                case torrent::ListenerState::Listening: listener = "listening"; break;
                case torrent::ListenerState::Stopped: listener = "stopped"; break;
                case torrent::ListenerState::Error: listener = "error"; break;
                default: break;
                }
                auto peers = nlohmann::json::array();
                auto trackers = nlohmann::json::array();
                for (const auto& tracker : network.trackers) {
                    auto history = nlohmann::json::array();
                    for (const auto& event : tracker.history) history.push_back({{"time", event.time_ms}, {"event", event.event},
                        {"result", event.result == torrent::TrackerEventResult::Success ? "success" :
                            event.result == torrent::TrackerEventResult::Cancelled ? "cancelled" : "failure"},
                        {"peers", event.peers_returned}, {"error", event.error}, {"duration", event.duration_ms}});
                    trackers.push_back({{"url", tracker.url}, {"tier", tracker.tier}, {"state", trackerState(tracker.state)},
                        {"requests", tracker.requests}, {"peers", tracker.peers_returned}, {"failures", tracker.consecutive_failures},
                        {"last_response", tracker.last_response_ms}, {"error", tracker.error}, {"history", history}});
                }
                for (const auto& peer : network.peers) peers.push_back({{"ip", peer.ip}, {"port", peer.port},
                    {"state", peerState(peer.state)}, {"incoming", peer.incoming}, {"choked", peer.choked},
                    {"pending", peer.pending_requests}, {"downloaded", peer.downloaded_bytes}, {"uploaded", peer.uploaded_bytes}});
                output << "STATUS " << nlohmann::json{{"busy", task.busy()}, {"progress", state.progress},
                    {"state", static_cast<int>(state.state)},
                    {"listener", {{"state", listener}, {"port", network.listener.port}, {"ipv6", network.listener.ipv6}, {"error", network.listener.error}}},
                    {"incoming", network.incoming_handshakes}, {"local_incoming", network.loopback_handshakes},
                    {"outgoing", network.outgoing_handshakes}, {"rejected", network.rejected_incoming},
                    {"incoming_failed", network.failed_incoming_handshakes},
                    {"hint", torrent::app::diagnoseNetwork(state).summary}, {"report", torrent::app::networkReport(state)},
                    {"error", state.error}, {"uploaded", state.uploaded_bytes}, {"port", state.listen_port},
                    {"peers", peers}, {"queued", network.queued_peers}, {"failed", network.failed_connections},
                    {"peer_error", network.last_peer_error}, {"dht", network.dht_enabled}, {"nodes", network.dht_nodes},
                    {"trackers", trackers}, {"can_update", torrent::app::canUpdateTrackers(state)},
                    {"tracker", {{"url", network.tracker.url}, {"state", trackerState(network.tracker.state)},
                        {"peers", network.tracker.peers_returned}, {"failures", network.tracker.consecutive_failures},
                        {"total", network.tracker.total_trackers}, {"attempt", network.tracker.attempt},
                        {"pending", network.tracker.retry_pending},
                        {"retry_in", torrent::app::trackerWaitSeconds(network.tracker.retry_after)},
                        {"next_in", torrent::app::trackerWaitSeconds(network.tracker.next_announce)},
                        {"error", network.tracker.error}}}}.dump() << '\n' << std::flush;
            } else throw std::runtime_error("Unknown command");
        }
        task.stop(); task.wait();
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
