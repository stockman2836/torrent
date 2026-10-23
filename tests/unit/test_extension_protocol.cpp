#include "torrent/network/peer/extension_protocol.h"
#include <gtest/gtest.h>

using namespace torrent;

namespace {
std::vector<uint8_t> handshake(int64_t id) {
    const auto encoded = BencodeParser::encode(BencodeValue::Dictionary{
        {"m", BencodeValue::Dictionary{{"ut_metadata", id}}}, {"metadata_size", int64_t(23456)}});
    return {encoded.begin(), encoded.end()};
}
}

TEST(ExtensionProtocol, HandshakeContainsOnlyBencodePayload) {
    ExtensionProtocol protocol;
    const auto id = protocol.registerExtension(EXT_NAME_METADATA, [](uint8_t, const auto&) {});
    const auto value = BencodeParser::parse(protocol.buildExtendedHandshake());
    ASSERT_TRUE(value.isDictionary());
    EXPECT_EQ(value.getDictionary().at("m").getDictionary().at(EXT_NAME_METADATA).getInteger(), id);
}

TEST(ExtensionProtocol, DispatchesLocalIdWithoutRemovingPayloadBytes) {
    ExtensionProtocol protocol;
    std::vector<uint8_t> received;
    const auto localId = protocol.registerExtension(EXT_NAME_METADATA,
        [&](uint8_t id, const auto& bytes) { EXPECT_EQ(id, 1); received = bytes; });
    protocol.parseExtendedHandshake(handshake(7));
    EXPECT_NE(localId, protocol.getPeerExtensionId(EXT_NAME_METADATA));
    const std::vector<uint8_t> payload{'d', 'e', 0, 255};
    protocol.handleExtensionMessage(localId, payload);
    EXPECT_EQ(received, payload);
}

TEST(ExtensionProtocol, BuildsUnframedRequestForRemoteExtension) {
    ExtensionProtocol protocol;
    protocol.parseExtendedHandshake(handshake(7));
    const auto bytes = protocol.buildExtensionMessage(EXT_NAME_METADATA,
        BencodeValue::Dictionary{{"msg_type", int64_t(0)}, {"piece", int64_t(1)}});
    const auto value = BencodeParser::parse(bytes);
    EXPECT_EQ(value.getDictionary().at("piece").getInteger(), 1);
    EXPECT_EQ(protocol.getPeerMetadataSize(), 23456);
}

TEST(ExtensionProtocol, ZeroDisablesExtensionAndInvalidIdsAreIgnored) {
    ExtensionProtocol protocol;
    protocol.parseExtendedHandshake(handshake(7));
    EXPECT_TRUE(protocol.peerSupportsExtension(EXT_NAME_METADATA));
    protocol.parseExtendedHandshake(handshake(0));
    EXPECT_FALSE(protocol.peerSupportsExtension(EXT_NAME_METADATA));
    EXPECT_TRUE(protocol.buildExtensionMessage(EXT_NAME_METADATA, BencodeValue(int64_t(0))).empty());
    for (const auto id : {-1, 256}) {
        protocol.parseExtendedHandshake(handshake(id));
        EXPECT_FALSE(protocol.peerSupportsExtension(EXT_NAME_METADATA));
    }
}
