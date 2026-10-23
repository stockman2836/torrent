#include "torrent/network/peer/metadata_exchange.h"
#include <gtest/gtest.h>
#include <stdexcept>

using namespace torrent;

namespace {
std::vector<uint8_t> message(int64_t index, int64_t total, const std::vector<uint8_t>& data) {
    const auto header = BencodeParser::encode(BencodeValue::Dictionary{
        {"msg_type", int64_t(1)}, {"piece", index}, {"total_size", total},
        {"x", std::string("embedded:de:i123e")}});
    std::vector<uint8_t> result(header.begin(), header.end());
    result.insert(result.end(), data.begin(), data.end());
    return result;
}
}

TEST(MetadataExchange, AssemblesBinaryBlocksOutOfOrderAndCallsBackWithoutLock) {
    std::vector<uint8_t> expected(METADATA_PIECE_SIZE + 7);
    for (size_t i = 0; i < expected.size(); ++i) expected[i] = static_cast<uint8_t>(i);
    int calls = 0;
    MetadataExchange* observed = nullptr;
    MetadataExchange exchange(expected.size(), [&](const auto& metadata) {
        ++calls;
        EXPECT_EQ(metadata, expected);
        EXPECT_TRUE(observed->isComplete());
        EXPECT_DOUBLE_EQ(observed->progress(), 100.0);
        EXPECT_EQ(observed->getMetadata(), expected);
    });
    observed = &exchange;
    exchange.handleMessage(message(1, expected.size(), {expected.begin() + METADATA_PIECE_SIZE, expected.end()}));
    EXPECT_DOUBLE_EQ(exchange.progress(), 50.0);
    exchange.handleMessage(message(0, expected.size(), {expected.begin(), expected.begin() + METADATA_PIECE_SIZE}));
    exchange.handleMessage(message(0, expected.size(), {expected.begin(), expected.begin() + METADATA_PIECE_SIZE}));
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(exchange.hasFailed());
}

TEST(MetadataExchange, RejectsInvalidAdvertisedSizesBeforeAllocation) {
    for (const auto size : {int64_t(-1), int64_t(0), MAX_METADATA_SIZE + 1}) {
        EXPECT_THROW(MetadataExchange(size, {}), std::invalid_argument);
    }
}

TEST(MetadataExchange, RejectsWrongTotalSizeAndPieceIndices) {
    for (const auto index : {int64_t(-1), int64_t(1)}) {
        MetadataExchange exchange(5, {});
        exchange.handleMessage(message(index, 5, {1, 2, 3, 4, 5}));
        EXPECT_TRUE(exchange.hasFailed());
        EXPECT_FALSE(exchange.isComplete());
    }
    MetadataExchange exchange(5, {});
    exchange.handleMessage(message(0, 6, {1, 2, 3, 4, 5}));
    EXPECT_TRUE(exchange.hasFailed());
}

TEST(MetadataExchange, RejectsShortBlocksAndOversizedLastBlocks) {
    MetadataExchange shortBlock(METADATA_PIECE_SIZE + 5, {});
    shortBlock.handleMessage(message(0, METADATA_PIECE_SIZE + 5, {1, 2, 3}));
    EXPECT_TRUE(shortBlock.hasFailed());
    MetadataExchange lastBlock(METADATA_PIECE_SIZE + 5, {});
    lastBlock.handleMessage(message(1, METADATA_PIECE_SIZE + 5, {1, 2, 3, 4, 5, 6}));
    EXPECT_TRUE(lastBlock.hasFailed());
}

TEST(MetadataExchange, RejectResponseTerminatesCurrentPeerAttempt) {
    MetadataExchange exchange(5, {});
    exchange.markPieceRequested(0);
    const auto encoded = BencodeParser::encode(exchange.buildRejectMessage(0));
    exchange.handleMessage({encoded.begin(), encoded.end()});
    EXPECT_TRUE(exchange.hasFailed());
    EXPECT_EQ(exchange.getNextPieceToRequest(), -1);
}

TEST(MetadataExchange, IgnoresUnknownMessageTypes) {
    MetadataExchange exchange(5, {});
    const auto encoded = BencodeParser::encode(BencodeValue::Dictionary{{"msg_type", int64_t(99)}});
    exchange.handleMessage({encoded.begin(), encoded.end()});
    EXPECT_FALSE(exchange.hasFailed());
    EXPECT_DOUBLE_EQ(exchange.progress(), 0.0);
}

TEST(MetadataExchange, MalformedDictionaryDoesNotCompleteMetadata) {
    MetadataExchange exchange(5, {});
    exchange.handleMessage({'d', 'e', 'e'});
    EXPECT_TRUE(exchange.hasFailed());
    EXPECT_FALSE(exchange.isComplete());
}
