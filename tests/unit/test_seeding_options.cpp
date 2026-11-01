#include "seeding_options.h"
#include <gtest/gtest.h>
#include <limits>

using namespace torrent;
using namespace torrent::app;
using namespace std::chrono_literals;

TEST(SeedingOptions, DisabledAndEmptyTorrentsFinishImmediately) {
    EXPECT_TRUE((SeedingOptions{false, 0, 0}.finished(0, 100, 0s)));
    EXPECT_TRUE((SeedingOptions{true, 0, 0}.finished(0, 0, 0s)));
    EXPECT_FALSE((SeedingOptions{true, 0, 0}.finished(999999, 100, 999999s)));
}

TEST(SeedingOptions, RatioUsesTheVerifiedSizeIncludingResumedDownloads) {
    SeedingOptions options{true, 1.1, 0};
    EXPECT_FALSE(options.finished(1099, 1000, 0s));
    EXPECT_TRUE(options.finished(1100, 1000, 0s));
    EXPECT_TRUE(options.finished(1101, 1000, 0s));
}

TEST(SeedingOptions, FirstReachedLimitWinsAndLargeDurationsDoNotOverflow) {
    SeedingOptions options{true, 2, 3};
    EXPECT_FALSE(options.finished(100, 100, 10799s));
    EXPECT_TRUE(options.finished(100, 100, 10800s));
    EXPECT_TRUE(options.finished(200, 100, 1s));
    options = {true, 0, std::numeric_limits<int>::max()};
    EXPECT_FALSE(options.finished(0, 100, 2147483647s));
    EXPECT_TRUE(options.finished(0, 100, std::chrono::seconds(int64_t(std::numeric_limits<int>::max()) * 3600)));
}

TEST(SeedingOptions, InvalidLimitsAreRejectedWithoutChangingConfiguration) {
    Config config;
    for (double ratio : {-1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()})
        EXPECT_THROW((SeedingOptions{true, ratio, 0}.applyTo(config)), std::invalid_argument);
    EXPECT_THROW((SeedingOptions{true, 1, -1}.applyTo(config)), std::invalid_argument);
    EXPECT_FALSE(config.seed_after_download);
    config.seed_ratio_limit = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(config.validate());
}

TEST(SeedingOptions, ConfigKeepsBothLimitsAndEnabledState) {
    Config config;
    SeedingOptions{true, 1.5, 4}.applyTo(config);
    const auto loaded = SeedingOptions::fromConfig(config);
    EXPECT_TRUE(loaded.enabled);
    EXPECT_DOUBLE_EQ(loaded.ratio_limit, 1.5);
    EXPECT_EQ(loaded.time_limit_hours, 4);
}
