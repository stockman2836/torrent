#include "download_queue.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>

using namespace torrent;
using namespace torrent::app;

int main(int argc, char* argv[]) {
    try {
        if (argc != 5) return 2;
        Config config;
        if (!config.loadFromFile(argv[3])) return 2;
        const std::string mode = argv[4];
        DownloadQueue queue;
        const auto first = queue.add(argv[1], config), second = queue.add(argv[2], config);
        queue.setLimit(mode == "parallel" || mode == "collision" ? 2 : 1);
        if (mode == "limited") queue.setSpeedLimits(first, 16384, 0);
        if (mode == "live" || mode == "limited-stop") queue.setSpeedLimits(first, 1024, 0);
        if (mode == "upload" || mode == "upload-live") queue.setSpeedLimits(first, 0, 8192);
        if (mode == "upload-stop") queue.setSpeedLimits(first, 0, 1);
        queue.enqueue(first); queue.enqueue(second);
        bool interrupted = false, resumed = false, changed = false;
        const auto started = std::chrono::steady_clock::now();
        auto stop_time = started;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline) {
            queue.tick();
            const auto entries = queue.snapshots();
            const auto a = std::find_if(entries.begin(), entries.end(), [first](const auto& e) { return e.id == first; });
            const auto b = std::find_if(entries.begin(), entries.end(), [second](const auto& e) { return e.id == second; });
            for (const auto& entry : entries) if (entry.download.state == DownloadState::Failed)
                throw std::runtime_error(entry.download.error);
            if ((mode == "live" || mode == "upload-live") && !changed &&
                std::chrono::steady_clock::now() - started > (mode == "live" ? std::chrono::seconds(1) : std::chrono::seconds(2))) {
                if (!queue.setSpeedLimits(first, 0, 0)) throw std::runtime_error("Cannot change active speed limits");
                changed = true;
            }
            if ((mode == "limited-stop" || mode == "upload-stop") && !interrupted &&
                std::chrono::steady_clock::now() - started > std::chrono::seconds(1)) {
                queue.stop(first); interrupted = true; stop_time = std::chrono::steady_clock::now();
            }
            if ((mode == "limited-stop" || mode == "upload-stop") && interrupted && !resumed &&
                a != entries.end() && !a->busy && a->download.state == DownloadState::Stopped &&
                b != entries.end() && b->download.state == DownloadState::Complete) {
                if (std::chrono::steady_clock::now() - stop_time > std::chrono::seconds(3))
                    throw std::runtime_error("Stopping a throttled transfer took too long");
                if (mode == "limited-stop") {
                    queue.setSpeedLimits(first, 0, 0);
                    if (!queue.enqueue(first)) throw std::runtime_error("Cannot resume a throttled transfer");
                }
                resumed = true;
            }
            if ((mode == "stop" || mode == "remove") && !interrupted && a != entries.end() && a->download.progress > 0) {
                if (a->download.progress >= 100) throw std::runtime_error("Expected a partial transfer");
                if (mode == "stop") queue.stop(first); else queue.remove(first);
                interrupted = true;
            }
            if (mode == "stop" && interrupted && !resumed && a != entries.end() &&
                a->download.state == DownloadState::Stopped && b != entries.end() && b->download.state == DownloadState::Complete) {
                if (!queue.enqueue(first)) throw std::runtime_error("Could not resume the stopped entry");
                resumed = true;
            }
            const bool complete = b != entries.end() && b->download.state == DownloadState::Complete &&
                (mode == "remove" ? a == entries.end() : a != entries.end() &&
                    (mode == "upload-stop" ? resumed && a->download.state == DownloadState::Stopped : a->download.state == DownloadState::Complete));
            if (complete) {
                queue.close();
                while (!queue.closed() && std::chrono::steady_clock::now() < deadline) {
                    queue.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                if (!queue.closed()) throw std::runtime_error("Queue did not close");
                std::cout << "PASS queue " << mode << '\n'; return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        throw std::runtime_error("Queue timed out");
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
