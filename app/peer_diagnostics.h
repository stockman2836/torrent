#pragma once

#include "torrent/session/network_status.h"
#include <string>

namespace torrent::app {

std::string peerAddress(const PeerStatus& peer);
std::string peerEndText(PeerEndReason reason);
std::string peerStateText(const PeerStatus& peer, bool seeding = false);
std::string peerResultText(const PeerStatus& peer);

} // namespace torrent::app
