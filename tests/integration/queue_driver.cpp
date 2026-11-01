#include "download_queue.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>
#include <optional>

using namespace torrent;
using namespace torrent::app;

int main(int argc, char* argv[]) {
    try {
        if (argc != 5) return 2;
        Config config;
        if (!config.loadFromFile(argv[3])) return 2;
        const std::string mode = argv[4];
        const bool seeding = mode.rfind("seed-", 0) == 0;
        if (mode == "seed-resume") {
            DownloadTask initial;
            initial.load(argv[1], config); initial.wait();
            initial.start(config); initial.wait();
            if (initial.snapshot().state != DownloadState::Complete) throw std::runtime_error("Initial download failed");
        }
        DownloadQueue queue;
        const auto first = queue.add(argv[1], config), second = queue.add(argv[2], config);
        queue.setLimit(mode == "parallel" || mode == "collision" ? 2 : 1);
        if (mode == "limited") queue.setSpeedLimits(first, 16384, 0);
        if (mode == "live" || mode == "limited-stop") queue.setSpeedLimits(first, 1024, 0);
        if (mode == "upload" || mode == "upload-live") queue.setSpeedLimits(first, 0, 8192);
        if (mode == "upload-stop") queue.setSpeedLimits(first, 0, 1);
        if (seeding) queue.setSeedingOptions(first, {true, mode == "seed-ratio" || mode == "seed-resume" ? 0.25 : 0, 0});
        queue.enqueue(first); queue.enqueue(second);
        bool interrupted = false, resumed = false, changed = false;
        bool saw_seed = false, second_finished_during_seed = false;
        std::optional<std::chrono::steady_clock::time_point> seeding_started;
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
            if (seeding && a != entries.end() && a->download.state == DownloadState::Seeding) {
                saw_seed = true;
                if (!seeding_started) seeding_started = std::chrono::steady_clock::now();
                if (b != entries.end() && b->download.state == DownloadState::Complete) second_finished_during_seed = true;
                if (mode == "seed-collision" && !interrupted &&
                    std::chrono::steady_clock::now() - *seeding_started > std::chrono::milliseconds(400)) {
                    if (b == entries.end() || !b->queued || b->busy) throw std::runtime_error("A writer started while the same files were seeding");
                    queue.stop(first); interrupted = true;
                }
                if ((mode == "seed-unlimited" || mode == "seed-disable") && a->download.uploaded_bytes >= 32768 && second_finished_during_seed && !interrupted) {
                    if (mode == "seed-unlimited") queue.stop(first);
                    else queue.setSeedingOptions(first, {false, 0, 0});
                    interrupted = true;
                }
            }
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
                if (seeding && (!saw_seed || (mode != "seed-collision" && !second_finished_during_seed)))
                    throw std::runtime_error("Seeding held a download slot or never became active");
                if (seeding && mode != "seed-collision" && a->download.seed_ratio < 0.25)
                    throw std::runtime_error("Missing seeding ratio statistics");
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
