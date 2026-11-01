#include "torrent/core/utils.h"
#include <gtest/gtest.h>
#include <atomic>
#include <future>
#include <limits>

using torrent::utils::TokenBucket;
using namespace std::chrono_literals;

TEST(TokenBucket, OversizedRequestCompletesAtTheLimitedRate) {
    TokenBucket bucket(100000);
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + 4s;
    EXPECT_TRUE(bucket.waitAndConsume(250000, [&] { return std::chrono::steady_clock::now() > deadline; }));
    EXPECT_GE(std::chrono::steady_clock::now() - start, 1300ms);
}

TEST(TokenBucket, CancelReleasesLowRateWait) {
    TokenBucket bucket(1);
    std::atomic<bool> cancel{false};
    auto result = std::async(std::launch::async, [&] { return bucket.waitAndConsume(16384, [&] { return cancel.load(); }); });
    EXPECT_EQ(result.wait_for(60ms), std::future_status::timeout);
    cancel = true;
    ASSERT_EQ(result.wait_for(1s), std::future_status::ready);
    EXPECT_FALSE(result.get());
}

TEST(TokenBucket, RemovingLimitReleasesAnAlreadyWaitingRequest) {
    TokenBucket bucket(1);
    auto result = std::async(std::launch::async, [&] { return bucket.waitAndConsume(16384); });
    EXPECT_EQ(result.wait_for(60ms), std::future_status::timeout);
    bucket.setRate(0);
    ASSERT_EQ(result.wait_for(1s), std::future_status::ready);
    EXPECT_TRUE(result.get());
    EXPECT_TRUE(bucket.tryConsume(100000000));
}

TEST(TokenBucket, ThreadsShareOneBudgetAndLargeRatesDoNotOverflow) {
    TokenBucket bucket(10000);
    ASSERT_TRUE(bucket.tryConsume(10000));
    const auto start = std::chrono::steady_clock::now();
    auto a = std::async(std::launch::async, [&] { return bucket.waitAndConsume(2000); });
    auto b = std::async(std::launch::async, [&] { return bucket.waitAndConsume(2000); });
    EXPECT_TRUE(a.get()); EXPECT_TRUE(b.get());
    EXPECT_GE(std::chrono::steady_clock::now() - start, 300ms);
    bucket.setRate(std::numeric_limits<int64_t>::max());
    EXPECT_EQ(bucket.getRate(), std::numeric_limits<int64_t>::max());
    EXPECT_THROW(bucket.setRate(-1), std::invalid_argument);
    EXPECT_THROW(TokenBucket(-1), std::invalid_argument);
}
