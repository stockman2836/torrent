#include "download_task.h"
#include "torrent/core/logger.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
using torrent::app::DownloadState;

const char* stateName(DownloadState state) {
    switch (state) {
        case DownloadState::Idle: return "idle";
        case DownloadState::Loading: return "loading";
        case DownloadState::Ready: return "ready";
        case DownloadState::Downloading: return "downloading";
        case DownloadState::Seeding: return "seeding";
        case DownloadState::Stopping: return "stopping";
        case DownloadState::Stopped: return "stopped";
        case DownloadState::Complete: return "complete";
        case DownloadState::Failed: return "failed";
    }
    return "unknown";
}

// The engine still has verbose stdout diagnostics. Keep long runs bounded on disk;
// JSONL records and the rotating logger retain the useful evidence separately.
class QuietOutput {
    class Buffer : public std::streambuf {
        int_type overflow(int_type c) override { return traits_type::not_eof(c); }
        std::streamsize xsputn(const char*, std::streamsize size) override { return size; }
    } buffer_;
    std::streambuf* previous_ = std::cout.rdbuf(&buffer_);
public:
    ~QuietOutput() { std::cout.rdbuf(previous_); }
};
}

// Manual public-network check, intentionally not registered with CTest.
int main(int argc, char* argv[]) {
    try {
        if (argc < 5 || argc > 6 || (argc == 6 && std::string(argv[5]) != "--metadata-only"))
            throw std::invalid_argument("Usage: public_swarm_driver <source> <config.json> <report.json> <seconds> [--metadata-only]");
        size_t parsed = 0;
        const auto seconds = std::stoi(argv[4], &parsed);
        if (parsed != std::string(argv[4]).size() || seconds < 1 || seconds > 86400)
            throw std::invalid_argument("Duration must be 1..86400 seconds");
        const bool metadata_only = argc == 6;
        torrent::Config config;
        if (!config.loadFromFile(argv[2]) || !config.validate())
            throw std::invalid_argument("Invalid configuration file");
        // This check must establish peer transfer without an HTTP seed fallback.
        config.enable_webseeds = false;
        config.seed_after_download = false;
        const auto report_path = std::filesystem::absolute(std::filesystem::u8path(argv[3]));
        std::filesystem::create_directories(report_path.parent_path());
        torrent::Logger::init(config.log_file, torrent::Logger::Level::OFF);
        auto events_path = report_path; events_path += ".jsonl";
        std::ofstream events(events_path);
        if (!events) throw std::runtime_error("Cannot write progress records");
        QuietOutput quiet;
        torrent::app::DownloadTask task;
        const auto started = Clock::now(), deadline = started + std::chrono::seconds(seconds);
        const auto elapsed = [&] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
        };
        const auto record = [&] {
            const auto state = task.snapshot();
            events << nlohmann::json{{"elapsed_ms", elapsed()}, {"state", stateName(state.state)},
                {"progress", state.progress}, {"download_speed", state.download_speed},
                {"upload_speed", state.upload_speed}, {"listen_port", state.listen_port}}.dump() << '\n';
            events.flush();
        };
        const auto monitor = [&] {
            auto next_record = Clock::now();
            while (task.busy() && Clock::now() < deadline) {
                if (Clock::now() >= next_record) {
                    record(); next_record = Clock::now() + std::chrono::seconds(1);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        };
        task.load(argv[1], config);
        monitor();
        const bool metadata_ready = !task.busy() && task.snapshot().state == DownloadState::Ready;
        if (metadata_ready && !metadata_only && Clock::now() < deadline) {
            if (!task.start(config)) throw std::runtime_error("Task did not start");
            monitor();
        }
        const bool timed_out = task.busy();
        const auto stopping = Clock::now();
        task.stop(); task.wait();
        const auto stop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - stopping).count();
        record();
        const auto state = task.snapshot();
        const bool complete = state.state == DownloadState::Complete;
        const char* outcome = complete ? "complete" : metadata_only && metadata_ready ? "metadata"
            : state.state == DownloadState::Failed ? "failed" : state.progress > 0 ? "partial" : "no-progress";
        int64_t total = 0;
        if (state.files) for (const auto& file : *state.files) total += file.length;
        nlohmann::json report{{"source", argv[1]}, {"name", state.name}, {"info_hash", state.info_hash},
            {"outcome", outcome}, {"state", stateName(state.state)}, {"error", state.error},
            {"progress", state.progress}, {"total_bytes", total}, {"elapsed_ms", elapsed()},
            {"budget_seconds", seconds}, {"timed_out", timed_out}, {"stop_ms", stop_ms},
            {"metadata_ready", metadata_ready}, {"download_dir", config.download_dir},
            {"dht", config.enable_dht}, {"webseeds", config.enable_webseeds},
            {"max_download_speed", config.max_download_speed}, {"max_upload_speed", config.max_upload_speed}};
        std::ofstream output(report_path);
        if (!output || !(output << report.dump(2) << '\n')) throw std::runtime_error("Cannot write report");
        return complete || (metadata_only && metadata_ready) ? 0 : state.state == DownloadState::Failed ? 1 : state.progress > 0 ? 3 : 4;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
