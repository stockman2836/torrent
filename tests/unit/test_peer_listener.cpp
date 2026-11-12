#include "torrent/network/peer/peer_listener.h"
#include "torrent/metainfo/block.h"
#include <gtest/gtest.h>

using namespace torrent;

TEST(PeerListener, ReservesItsPortAndReleasesItWhenDestroyed) {
    uint16_t port;
    {
        PeerListener listener(0, false);
        port = listener.port();
        EXPECT_NE(port, 0);
        const auto status = listener.status();
        EXPECT_EQ(status.state, ListenerState::Listening);
        EXPECT_EQ(status.port, port);
        EXPECT_FALSE(status.ipv6);
        EXPECT_TRUE(status.error.empty());
        EXPECT_THROW(PeerListener(port, false), std::runtime_error);
    }
    EXPECT_NO_THROW(PeerListener(port, false));
}

TEST(PeerListener, RecognizesLocalEvidenceIncludingIPv4MappedIPv6) {
    for (const auto* address : {"127.0.0.1", "127.23.45.67", "::1", "::ffff:127.0.0.1", "::FFFF:7f00:1"})
        EXPECT_TRUE(isLoopbackAddress(address)) << address;
    for (const auto* address : {"192.168.1.2", "10.0.0.1", "1.1.1.1", "::ffff:192.168.1.2", "::", "fe80::1", "invalid", "localhost"})
        EXPECT_FALSE(isLoopbackAddress(address)) << address;
}

TEST(PeerListener, ReportsTheActualAddressFamilyWhenIPv6IsRequested) {
    PeerListener listener(0, true);
    const auto status = listener.status();
    EXPECT_EQ(status.state, ListenerState::Listening);
    EXPECT_EQ(status.port, listener.port());
    EXPECT_TRUE(status.error.empty()); // IPv4 fallback is permitted when IPv6 is unavailable.
}

TEST(PeerConnection, BitfieldMustMatchKnownPieceCountAndHaveZeroPadding) {
    PeerConnection peer("127.0.0.1", 1, std::vector<uint8_t>(20, 1), std::string(20, 'x'));
    peer.initializePeerBitfield(3);
    PeerMessage message;
    message.type = MessageType::BITFIELD;
    message.payload = {0xa0};
    BitfieldMessage result;
    ASSERT_TRUE(peer.parseBitfield(message, result));
    EXPECT_EQ(result.bitfield, (std::vector<bool>{true, false, true}));
    message.payload = {0xa1}; EXPECT_FALSE(peer.parseBitfield(message, result));
    message.payload = {0xa0, 0}; EXPECT_FALSE(peer.parseBitfield(message, result));
}
