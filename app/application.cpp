#include "application.h"
#include "command_line.h"
#include "input.h"
#include "torrent/core/logger.h"
#include "torrent/session/download_manager.h"
#include "torrent/metainfo/file_selection.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace torrent::app {
namespace {

Logger::Level logLevel(const std::string& level) {
    if (level == "trace") return Logger::Level::TRACE;
    if (level == "debug") return Logger::Level::DEBUG;
    if (level == "info") return Logger::Level::INFO;
    if (level == "warn") return Logger::Level::WARN;
    if (level == "error") return Logger::Level::ERROR;
    throw std::invalid_argument("Invalid log level: " + level);
}

} // namespace

int run(int argc, char* argv[]) {
    try {
        const auto options = CommandLine::parse(argc, argv);
        if (options.help) { printUsage(std::cout, argv[0]); return 0; }

        const auto config = options.loadConfig();
        Logger::init(config.log_file, logLevel(config.log_level), Logger::Level::DEBUG,
                     config.log_max_size, config.log_max_files);
        auto input = resolveInput(options.input, config);
        const FileSelection selection(input.torrent, config.file_indices, config.file_priorities);
        const bool custom_selection = !config.file_indices.empty() || !config.file_priorities.empty();
        if (options.list_files) {
            std::cout << "ID\tBytes\tPath\tPriority\n";
            for (size_t i = 0; i < input.torrent.files().size(); ++i) {
                const auto& file = input.torrent.files()[i];
                std::cout << i + 1 << '\t' << file.length << '\t' << file.path << '\t'
                          << filePriorityName(selection.filePriorities()[i]) << '\n';
            }
            return 0;
        }
        if (custom_selection) std::cout << "Selected file bytes: " << selection.selectedBytes() << '\n';
        DownloadManager download(std::move(input.torrent), config, std::move(input.dht));
        download.start();
        while (download.isRunning() && download.progress() < 100.0) {
            download.printStatus();
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        const bool complete = download.progress() >= 100.0;
        if (complete) std::cout << (!custom_selection
            ? "\n=== Download complete! ===\n" : "\n=== Selected files complete! ===\n");
        download.stop();
        return complete ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}

} // namespace torrent::app
