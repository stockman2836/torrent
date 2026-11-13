#include "network_settings.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <chrono>

using namespace torrent;
using namespace torrent::app;

TEST(NetworkSettings, ParsesWholeNumbersAndBothDiscoveryFlags) {
    const auto settings = NetworkSettings::parse(" 24501 ", " 120 ", false, true);
    EXPECT_EQ(settings.first_port, 24501);
    EXPECT_EQ(settings.max_peers, 120);
    EXPECT_FALSE(settings.enable_dht);
    EXPECT_TRUE(settings.enable_pex);
    EXPECT_NO_THROW((NetworkSettings{1024, 1, false, false}).validate());
    EXPECT_NO_THROW((NetworkSettings{65534, 1000, true, true}).validate());
}

TEST(NetworkSettings, RejectsWrappedPortsInvalidLimitsAndAmbiguousNumbers) {
    for (const auto* port : {"", "0", "-1", "1023", "65535", "65536", "9999999999999999999", "6881x", "6.881", "1e4", "+6881"})
        EXPECT_THROW(NetworkSettings::parse(port, "50", true, true), std::invalid_argument) << port;
    for (const auto* peers : {"", "0", "-1", "1001", "1.5", "50x", "9999999999999999999"})
        EXPECT_THROW(NetworkSettings::parse("6881", peers, true, true), std::invalid_argument) << peers;
}

TEST(NetworkSettings, LeavesRoomForEveryTorrentsPortPair) {
    const NetworkSettings last_pair{65534, 50, true, true};
    EXPECT_NO_THROW(last_pair.validate(1));
    EXPECT_THROW(last_pair.validate(2), std::invalid_argument);
    EXPECT_NO_THROW((NetworkSettings{65532, 50, true, true}).validate(2));
    EXPECT_THROW((NetworkSettings{65533, 50, true, true}).validate(2), std::invalid_argument);
}

TEST(NetworkSettings, ChangesOnlyTheSelectedNetworkFields) {
    Config config;
    config.download_dir = "keep-files";
    config.max_download_speed = 12345;
    config.seed_after_download = true;
    config.enable_lsd = false;
    config.dht_bootstrap_nodes = {{"127.0.0.1", 49123}};
    const NetworkSettings settings{24501, 80, false, true};
    settings.applyTo(config);
    EXPECT_EQ(config.listen_port, 24501);
    EXPECT_EQ(config.dht_port, 24502);
    EXPECT_EQ(config.max_peers, 80);
    EXPECT_FALSE(config.enable_dht);
    EXPECT_TRUE(config.enable_pex);
    EXPECT_EQ(config.download_dir, "keep-files");
    EXPECT_EQ(config.max_download_speed, 12345);
    EXPECT_TRUE(config.seed_after_download);
    EXPECT_FALSE(config.enable_lsd);
    ASSERT_EQ(config.dht_bootstrap_nodes.size(), 1u);
    EXPECT_EQ(config.dht_bootstrap_nodes[0].port, 49123);
}

TEST(NetworkSettings, RouterMappingRequiresExplicitOptInAndRoundTripsInPreferences) {
    Config config;
    EXPECT_FALSE(config.enable_port_mapping);
    EXPECT_FALSE(NetworkSettings::fromConfig(config).enable_port_mapping);
    auto settings = NetworkSettings::parse("24567", "8", false, true, true);
    settings.applyTo(config);
    EXPECT_TRUE(config.enable_port_mapping);
    const auto path = std::filesystem::temp_directory_path() / ("mapping-settings-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    ASSERT_TRUE(config.saveToFile(path.u8string()));
    Config restored;
    ASSERT_TRUE(restored.loadFromFile(path.u8string()));
    EXPECT_TRUE(restored.enable_port_mapping);
    std::ofstream(path) << "{}";
    Config legacy;
    EXPECT_TRUE(legacy.loadFromFile(path.u8string()));
    EXPECT_FALSE(legacy.enable_port_mapping);
    std::filesystem::remove(path);
}
