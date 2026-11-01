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
        EXPECT_THROW(PeerListener(port, false), std::runtime_error);
    }
    EXPECT_NO_THROW(PeerListener(port, false));
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
