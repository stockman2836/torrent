#include "torrent/core/utils.h"
#include <gtest/gtest.h>

using torrent::utils::displayText;

TEST(DisplayText, KeepsUtf8AndRemovesControlCharactersFromClientNames) {
    EXPECT_EQ(displayText(u8"Клиент/1.0 😀"), u8"Клиент/1.0 😀");
    EXPECT_EQ(displayText(std::string("A\0\n\tB", 5)), "A   B");
    EXPECT_EQ(displayText("\r\n\t"), "");
    EXPECT_EQ(displayText("  Client/1.0  "), "Client/1.0");
}

TEST(DisplayText, NeverCutsUtf8AtTheOutputBound) {
    const auto value = std::string(255, 'a') + u8"я";
    EXPECT_EQ(displayText(value).size(), 255);
    EXPECT_EQ(displayText(value, 257), value);
    EXPECT_TRUE(displayText(value, 0).empty());
    EXPECT_EQ(displayText(std::string(10000, 'a')).size(), 256);
}

TEST(DisplayText, ReplacesInvalidOverlongSurrogateAndTruncatedSequences) {
    for (const auto* invalid : {"\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82", "\xff"}) {
        const std::string value(invalid);
        EXPECT_EQ(displayText(value), std::string(value.size(), '?'));
    }
}
