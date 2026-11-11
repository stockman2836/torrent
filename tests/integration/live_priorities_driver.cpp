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
    do { queue.tick(); if (done()) return; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("Live priority fixture timed out");
}
}

int main(int argc, char* argv[]) {
    try {
        if (argc != 3 && argc != 4) return 2;
        Config config; if (!config.loadFromFile(argv[1]) || !config.validate()) return 2;
        std::ostream output(std::cout.rdbuf()); QuietOutput quiet;
        auto queue = std::make_unique<DownloadQueue>(); queue->setLimit(2);
        for (int i = 2; i < argc; ++i) queue->add(argv[i], config);
        const auto ready = [&] { for (const auto& entry : queue->snapshots()) if (entry.busy) return false; return true; };
        until(*queue, ready); output << "READY\n" << std::flush;
        std::string line;
        while (std::getline(std::cin, line) && line != "quit") {
            std::istringstream input(line); std::string command; input >> command;
            if (command == "start" || command == "stop" || command == "priority") {
                size_t index; if (!(input >> index)) throw std::runtime_error("Missing torrent index");
                const auto id = queue->snapshots().at(index).id;
                if (command == "start") {
                    if (!queue->enqueue(id)) throw std::runtime_error("Cannot start torrent");
                    queue->tick();
                } else if (command == "stop") {
                    queue->stop(id); until(*queue, [&] { return !queue->snapshots().at(index).busy; });
                } else {
                    std::map<size_t, FilePriority> priorities; int value;
                    while (input >> value) priorities[priorities.size()] = static_cast<FilePriority>(value);
                    try {
                        if (!queue->setFilePriorities(id, priorities)) throw std::runtime_error("Priority update rejected");
                    } catch (const std::invalid_argument&) { output << "REJECTED\n" << std::flush; continue; }
                }
                output << "OK\n" << std::flush;
            } else if (command == "limits") {
                size_t index; int64_t down, up;
                if (!(input >> index >> down >> up) || !queue->setSpeedLimits(queue->snapshots().at(index).id, down, up))
                    throw std::runtime_error("Cannot set speed limits");
                output << "OK\n" << std::flush;
            } else if (command == "save" || command == "restore") {
                queue->save("list.json");
                if (command == "restore") {
                    queue->close(); until(*queue, [&] { return queue->closed(); });
                    queue = std::make_unique<DownloadQueue>(); queue->restore("list.json", config); until(*queue, ready);
                }
                output << "OK\n" << std::flush;
            } else if (command == "status") {
                queue->tick(); auto states = nlohmann::json::array();
                for (const auto& entry : queue->snapshots()) {
                    std::vector<int> priorities(entry.download.files ? entry.download.files->size() : 0, 2);
                    for (const auto& [index, priority] : entry.config.file_priorities) if (index < priorities.size()) priorities[index] = static_cast<int>(priority);
                    int64_t received = 0; for (const auto& peer : entry.download.network.peers) received += peer.downloaded_bytes;
                    states.push_back({{"busy", entry.busy}, {"queued", entry.queued}, {"pending", entry.download.priorities_pending},
                        {"progress", entry.download.progress}, {"error", entry.download.error}, {"priorities", priorities}, {"received", received}});
                }
                output << "STATUS " << states.dump() << '\n' << std::flush;
            } else throw std::runtime_error("Unknown command");
        }
        queue->save("list.json"); queue->close(); until(*queue, [&] { return queue->closed(); });
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
