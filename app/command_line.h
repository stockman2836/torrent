#pragma once

#include "torrent/core/config.h"
#include <iosfwd>
#include <optional>
#include <string>

namespace torrent::app {

struct CommandLine {
    bool help = false;
    std::string input;
    std::string config_file = "./config.json";
    std::optional<std::string> download_dir;
    std::optional<int64_t> max_download_speed;
    std::optional<int64_t> max_upload_speed;
    std::optional<uint16_t> listen_port;
    std::optional<std::string> log_level;

    static CommandLine parse(int argc, char* argv[]);
    Config loadConfig() const;
};

void printUsage(std::ostream& out, const char* program);

} // namespace torrent::app
