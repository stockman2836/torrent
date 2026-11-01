#include "speed_limits.h"
#include <gtest/gtest.h>
#include <limits>

using namespace torrent::app;

TEST(SpeedLimits, ParsesKiBAndBothDecimalSeparators) {
    EXPECT_EQ(parseSpeedLimit("0"), 0);
    EXPECT_EQ(parseSpeedLimit(" 128 \t"), 131072);
    EXPECT_EQ(parseSpeedLimit("1.5"), 1536);
    EXPECT_EQ(parseSpeedLimit("1,5"), 1536);
    EXPECT_EQ(parseSpeedLimit("0.0001"), 1); // A small positive value must never become unlimited.
}

TEST(SpeedLimits, FormattingPreservesBytePrecisionIncludingLargeLimits) {
    for (int64_t value : {int64_t(0), int64_t(1), int64_t(1023), int64_t(1024), int64_t(1536),
         int64_t(1048576), std::numeric_limits<int64_t>::max()})
        EXPECT_EQ(parseSpeedLimit(formatSpeedLimit(value)), value);
    EXPECT_EQ(formatSpeedLimit(1536), "1.5");
}

TEST(SpeedLimits, RejectsMalformedNegativeAndOverflowingValues) {
    for (const auto* value : {"", " ", "-1", "+1", "NaN", "inf", "1e3", "1.", ".5", "1,2.3", "1 2",
         "999999999999999999999", "9007199254740992", "9007199254740991.9999999999", "0.12345678901"})
        EXPECT_THROW(parseSpeedLimit(value), std::invalid_argument) << value;
    EXPECT_THROW(formatSpeedLimit(-1), std::invalid_argument);
}
