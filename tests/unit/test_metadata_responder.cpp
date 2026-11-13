#include "torrent/network/peer/metadata_responder.h"
#include "torrent/network/peer/metadata_exchange.h"
#include <gtest/gtest.h>
#include <limits>

using namespace torrent;

namespace {
std::vector<uint8_t> request(int64_t piece, int64_t type = 0) {
    const auto encoded = BencodeParser::encode(BencodeValue::Dictionary{{"msg_type", type}, {"piece", piece}});
    return {encoded.begin(), encoded.end()};
}
int64_t replyType(const std::vector<uint8_t>& bytes) {
    return BencodeParser::parsePrefix(bytes).first.getDictionary().at("msg_type").getInteger();
}
}

TEST(MetadataResponder, SendsExactBinaryBlocksOutOfOrderWithSeparateHeader) {
    std::vector<uint8_t> bytes(METADATA_PIECE_SIZE + 37);
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(i);
    MetadataResponder responder(bytes);
    for (const auto index : {1, 0}) {
        const auto response = responder.reply(request(index));
        const auto parsed = BencodeParser::parsePrefix(response);
        const auto& header = parsed.first.getDictionary();
        EXPECT_EQ(header.at("msg_type").getInteger(), 1);
        EXPECT_EQ(header.at("piece").getInteger(), index);
        EXPECT_EQ(header.at("total_size").getInteger(), bytes.size());
        EXPECT_EQ((std::vector<uint8_t>{response.begin() + parsed.second, response.end()}),
            (std::vector<uint8_t>{bytes.begin() + index * METADATA_PIECE_SIZE,
                bytes.begin() + std::min(bytes.size(), (index + 1) * METADATA_PIECE_SIZE)}));
    }
}

TEST(MetadataResponder, RejectsInvalidIndicesWithoutConsumingTheReplyBudget) {
    const std::vector<uint8_t> bytes{0, 255, 'e'};
    MetadataResponder responder(bytes);
    for (const auto index : {int64_t(-1), int64_t(1), std::numeric_limits<int64_t>::max()}) {
        const auto response = BencodeParser::parse(responder.reply(request(index))).getDictionary();
        EXPECT_EQ(response.at("msg_type").getInteger(), 2);
        EXPECT_EQ(response.at("piece").getInteger(), index);
        EXPECT_EQ(response.count("total_size"), 0);
    }
    EXPECT_EQ(replyType(responder.reply(request(0))), 1);
}

TEST(MetadataResponder, BoundsDataPerConnectionAndKeepsOtherPeersIndependent) {
    const std::vector<uint8_t> bytes(METADATA_PIECE_SIZE * 2, 7);
    MetadataResponder first(bytes), second(bytes);
    for (int i = 0; i < 8; ++i) EXPECT_EQ(replyType(first.reply(request(i % 2))), 1);
    EXPECT_EQ(replyType(first.reply(request(0))), 2);
    EXPECT_EQ(replyType(second.reply(request(0))), 1);
}

TEST(MetadataResponder, RejectsUnavailableAndOversizedMetadata) {
    for (const auto size : {size_t(0), size_t(MAX_METADATA_SIZE + 1)}) {
        const std::vector<uint8_t> bytes(size, 0);
        MetadataResponder responder(bytes);
        EXPECT_EQ(responder.metadataSize(), 0);
        EXPECT_EQ(replyType(responder.reply(request(0))), 2);
    }
}

TEST(MetadataResponder, IgnoresMalformedAndUnknownMessagesWithoutConsumingBudget) {
    const std::vector<uint8_t> bytes{1};
    MetadataResponder responder(bytes);
    EXPECT_TRUE(responder.reply(request(0, 99)).empty());
    EXPECT_TRUE(responder.reply(request(0, 1)).empty());
    EXPECT_TRUE(responder.reply(request(0, 2)).empty());
    for (const auto* value : {"de", "d8:msg_typei0ee", "d8:msg_typei0e5:piece1:xe", "d8:msg_typei0e5:piecei0eex", "li1ee"})
        EXPECT_TRUE(responder.reply({value, value + std::char_traits<char>::length(value)}).empty());
    EXPECT_TRUE(responder.reply(std::vector<uint8_t>(1024, 'l')).empty());
    EXPECT_EQ(replyType(responder.reply(request(0))), 1);
}
