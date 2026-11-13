#include "command_line.h"
#include <charconv>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string_view>
#include <set>
#include <sstream>

namespace torrent::app {
namespace {

int64_t number(const std::string& text, const std::string& option, int64_t maximum) {
    int64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value < 0 || value > maximum) {
        throw std::invalid_argument("Invalid value for " + option + ": " + text);
    }
    return value;
}

} // namespace

CommandLine CommandLine::parse(int argc, char* argv[]) {
    CommandLine options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") { options.help = true; return options; }
    }
    if (argc < 2) throw std::invalid_argument("A torrent file or magnet link is required");
    options.input = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--list-files") { options.list_files = true; continue; }
        if (arg != "--config" && arg != "--download-dir" && arg != "--max-download" &&
            arg != "--max-upload" && arg != "--port" && arg != "--log-level" && arg != "--files" && arg != "--priority") {
            throw std::invalid_argument("Unknown option: " + arg);
        }
        if (++i >= argc) throw std::invalid_argument("Missing value for " + arg);
        const std::string value = argv[i];
        if (arg == "--config") options.config_file = value;
        else if (arg == "--download-dir") options.download_dir = value;
        else if (arg == "--log-level") options.log_level = value;
        else if (arg == "--priority") {
            if (value.empty() || value.back() == ',') throw std::invalid_argument("Expected file:priority pairs");
            std::istringstream input(value);
            std::string token;
            while (std::getline(input, token, ',')) {
                const auto colon = token.find(':');
                if (colon == std::string::npos) throw std::invalid_argument("Expected file:priority pairs");
                const auto id = number(token.substr(0, colon), arg, std::numeric_limits<int64_t>::max());
                if (id == 0) throw std::invalid_argument("File numbers start at 1");
                const auto level = token.substr(colon + 1);
                FilePriority priority;
                if (level == "skip") priority = FilePriority::Skip;
                else if (level == "low") priority = FilePriority::Low;
                else if (level == "normal") priority = FilePriority::Normal;
                else if (level == "high") priority = FilePriority::High;
                else throw std::invalid_argument("Unknown priority: " + level);
                options.file_priorities[static_cast<size_t>(id - 1)] = priority;
            }
        }
        else if (arg == "--files") {
            if (value.empty() || value.back() == ',') throw std::invalid_argument("Expected file numbers separated by commas");
            std::istringstream input(value);
            std::string token;
            std::set<size_t> indices;
            while (std::getline(input, token, ',')) {
                const auto id = number(token, arg, std::numeric_limits<int64_t>::max());
                if (id == 0) throw std::invalid_argument("File numbers start at 1");
                indices.insert(static_cast<size_t>(id - 1));
            }
            options.file_indices.assign(indices.begin(), indices.end());
        }
        else if (arg == "--port") {
            const auto port = number(value, arg, 65535);
            if (port == 0) throw std::invalid_argument("Port must be between 1 and 65535");
            options.listen_port = static_cast<uint16_t>(port);
        } else {
            const auto speed = number(value, arg, std::numeric_limits<int64_t>::max() / 1024) * 1024;
            if (arg == "--max-download") options.max_download_speed = speed;
            else options.max_upload_speed = speed;
        }
    }
    return options;
}

Config CommandLine::loadConfig() const {
    Config config;
    if (!config.loadFromFile(config_file)) config = Config::loadDefault();
    config.mergeWithCLI(download_dir, max_download_speed, max_upload_speed, listen_port, log_level);
    config.file_indices = file_indices;
    config.file_priorities = file_priorities;
    if (!config.validate()) throw std::invalid_argument("Invalid configuration");
    return config;
}

void printUsage(std::ostream& out, const char* program) {
    out << "Usage: " << program << " <torrent_file|magnet_uri> [options]\n\n"
        << "  --config <file>        Configuration file (default: ./config.json)\n"
        << "  --download-dir <dir>   Download directory\n"
        << "  --max-download <KB/s>  Download limit (0 = unlimited)\n"
        << "  --max-upload <KB/s>    Upload limit (0 = unlimited)\n"
        << "  --port <port>         Listen port\n"
        << "  --log-level <level>   trace, debug, info, warn or error\n"
        << "  --list-files          List file numbers, sizes and paths without downloading\n"
        << "  --files <1,3,...>      Download only these file numbers (default: all files)\n"
        << "  --priority <2:high,4:low>  Set skip, low, normal or high file priorities\n"
        << "  --help               Show this help\n\n"
        << "CLI options override configuration file settings.\n";
}

} // namespace torrent::app
