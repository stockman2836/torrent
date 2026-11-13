#include "torrent/network/peer/peer_connection.h"
#include <algorithm>
#include <array>

namespace torrent {

bool PeerConnection::sendHandshake() {
    if (info_hash_.size() != 20 || peer_id_.size() != 20) return false;
    std::array<uint8_t, 68> bytes{};
    const std::string protocol = "BitTorrent protocol";
    bytes[0] = 19;
    std::copy(protocol.begin(), protocol.end(), bytes.begin() + 1);
    bytes[25] = 0x10; // BEP 10.
    if (supports_fast_extension_) bytes[27] = 0x04;
    std::copy(info_hash_.begin(), info_hash_.end(), bytes.begin() + 28);
    std::copy(peer_id_.begin(), peer_id_.end(), bytes.begin() + 48);
    return sendData(bytes.data(), bytes.size());
}

bool PeerConnection::receiveHandshake() {
    std::array<uint8_t, 68> bytes{};
    if (!receiveData(bytes.data(), 1) || bytes[0] != 19 ||
        !receiveData(bytes.data() + 1, bytes.size() - 1)) return false;
    if (std::string(bytes.begin() + 1, bytes.begin() + 20) != "BitTorrent protocol" || info_hash_.size() != 20 ||
        !std::equal(info_hash_.begin(), info_hash_.end(), bytes.begin() + 28)) return false;
    remote_peer_id_.assign(bytes.begin() + 48, bytes.end());
    if (remote_peer_id_ == peer_id_) return false;
    peer_supports_fast_extension_ = (bytes[27] & 0x04) != 0;
    peer_supports_extensions_ = (bytes[25] & 0x10) != 0;
    return true;
}

bool PeerConnection::performHandshake() {
    return performHandshake({});
}

bool PeerConnection::sendAvailability(const std::vector<bool>& bitfield) {
    // BEP 6 requires exactly one availability message immediately after the
    // handshake, including metadata-only connections with no known pieces yet.
    if (supports_fast_extension_ && peer_supports_fast_extension_) {
        if (std::none_of(bitfield.begin(), bitfield.end(), [](bool have) { return have; })) return sendHaveNone();
        if (std::all_of(bitfield.begin(), bitfield.end(), [](bool have) { return have; })) return sendHaveAll();
    }
    return bitfield.empty() || sendBitfield(bitfield);
}

bool PeerConnection::performHandshake(const std::vector<bool>& bitfield) {
    if (!connected_) return false;
    if (handshake_completed_) return true;
    if (!sendHandshake() || !receiveHandshake() || !sendAvailability(bitfield) || !sendUnchoke()) {
        disconnect(); return false;
    }
    handshake_completed_ = true;
    return true;
}

bool PeerConnection::acceptHandshake(const std::vector<bool>& bitfield) {
    if (!connected_) return false;
    if (handshake_completed_) return true;
    read_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    const bool valid = receiveHandshake();
    read_deadline_ = std::chrono::steady_clock::time_point::max();
    if (!valid || !sendHandshake() || !sendAvailability(bitfield) || !sendUnchoke()) {
        disconnect(); return false;
    }
    handshake_completed_ = true;
    return true;
}

} // namespace torrent
