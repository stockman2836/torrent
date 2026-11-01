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
        queue.enqueue(first); queue.enqueue(second);
        bool interrupted = false, resumed = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline) {
            queue.tick();
            const auto entries = queue.snapshots();
            const auto a = std::find_if(entries.begin(), entries.end(), [first](const auto& e) { return e.id == first; });
            const auto b = std::find_if(entries.begin(), entries.end(), [second](const auto& e) { return e.id == second; });
            for (const auto& entry : entries) if (entry.download.state == DownloadState::Failed)
                throw std::runtime_error(entry.download.error);
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
                (mode == "remove" ? a == entries.end() : a != entries.end() && a->download.state == DownloadState::Complete);
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
