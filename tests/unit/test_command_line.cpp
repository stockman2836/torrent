#include "command_line.h"
#include <gtest/gtest.h>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

using torrent::app::CommandLine;

namespace {
CommandLine parse(std::initializer_list<std::string> arguments) {
    std::vector<std::string> strings{"torrent_client"};
    strings.insert(strings.end(), arguments);
    std::vector<char*> argv;
    for (auto& text : strings) argv.push_back(text.data());
    return CommandLine::parse(static_cast<int>(argv.size()), argv.data());
}
}

TEST(CommandLine, ParsesInputAndOverrides) {
    const auto options = parse({"file.torrent", "--port", "65535", "--max-download", "500",
                                "--max-upload", "0", "--download-dir", "data", "--config", "custom.json"});
    EXPECT_EQ(options.input, "file.torrent");
    EXPECT_EQ(options.listen_port, 65535);
    EXPECT_EQ(options.max_download_speed, 512000);
    EXPECT_EQ(options.max_upload_speed, 0);
    EXPECT_EQ(options.download_dir, "data");
    EXPECT_EQ(options.config_file, "custom.json");
}

TEST(CommandLine, AcceptsHelpWithoutInput) {
    EXPECT_TRUE(parse({"--help"}).help);
    EXPECT_TRUE(parse({"-h"}).help);
}

TEST(CommandLine, RejectsMissingInputAndOptionValues) {
    EXPECT_THROW(parse({}), std::invalid_argument);
    EXPECT_THROW(parse({"file.torrent", "--port"}), std::invalid_argument);
    EXPECT_THROW(parse({"file.torrent", "--unknown", "123"}), std::invalid_argument);
}

TEST(CommandLine, RejectsInvalidPortsWithoutWrapping) {
    for (const auto* value : {"0", "65536", "-1", "6881abc", "abc", "9999999999999999999999"}) {
        EXPECT_THROW(parse({"file.torrent", "--port", value}), std::invalid_argument) << value;
    }
}

TEST(CommandLine, RejectsInvalidAndOverflowingSpeedLimits) {
    for (const auto* value : {"-1", "1.5", "500abc", "9223372036854775807"}) {
        EXPECT_THROW(parse({"file.torrent", "--max-download", value}), std::invalid_argument) << value;
    }
}
