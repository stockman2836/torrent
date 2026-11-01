#include "command_line.h"
#include "download_task.h"
#include <iostream>
#include <chrono>
#include <thread>

int main(int argc, char* argv[]) {
    try {
#ifdef TASK_PARTIAL
        if (argc != 3) return 2;
        torrent::app::CommandLine options;
        options.input = argv[1]; options.config_file = argv[2];
#else
        const auto options = torrent::app::CommandLine::parse(argc, argv);
#endif
        const auto config = options.loadConfig();
        torrent::app::DownloadTask task;
        task.load(options.input, config); task.wait();
        auto state = task.snapshot();
        if (state.state == torrent::app::DownloadState::Failed) throw std::runtime_error(state.error);
        if (!task.start(config)) throw std::runtime_error("Task did not start");
#ifdef TASK_PARTIAL
        if (task.start(config) || task.load(options.input, config))
            throw std::runtime_error("Concurrent commands must be rejected");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (task.busy() && task.snapshot().progress == 0 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        task.stop(); task.wait();
        state = task.snapshot();
        if (state.state != torrent::app::DownloadState::Stopped || state.progress <= 0 || state.progress >= 100)
            throw std::runtime_error("Expected a stopped partial download");
        return 0;
#endif
        task.wait();
        state = task.snapshot();
        if (state.state != torrent::app::DownloadState::Complete) throw std::runtime_error(state.error);
        std::cout << "Download complete!\nSelected files complete!\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
