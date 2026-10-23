#include "torrent/network/peer/peer_connection.h"
#include "torrent/metainfo/block.h"
#include "torrent/core/utils.h"
#include "torrent/core/logger.h"
#include "torrent/network/peer/extension_protocol.h"
#include "torrent/network/peer/mse_handshake.h"
#include "torrent/network/discovery/pex_manager.h"
#include <cstring>
#include <iostream>
#include <chrono>
#include <thread>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <errno.h>
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close
#endif


namespace torrent {

bool PeerConnection::performHandshake() {
    if (!connected_) {
        LOG_ERROR("Cannot perform handshake with {}:{} - not connected", ip_, port_);
        return false;
    }

    if (handshake_completed_) {
        LOG_WARN("Handshake with {}:{} already completed", ip_, port_);
        return true;
    }

    std::cout << "Performing BitTorrent handshake with " << ip_ << ":" << port_ << "...\n";

    // BitTorrent handshake format:
    // <pstrlen><pstr><reserved><info_hash><peer_id>
    // pstrlen = 19 (1 byte)
    // pstr = "BitTorrent protocol" (19 bytes)
    // reserved = 8 bytes (all zeros for basic protocol)
    // info_hash = 20 bytes
    // peer_id = 20 bytes
    // Total: 68 bytes

    // Build handshake message
    std::vector<uint8_t> handshake;
    handshake.reserve(68);

    // Protocol string length
    handshake.push_back(19);

    // Protocol string
    std::string protocol = "BitTorrent protocol";
    handshake.insert(handshake.end(), protocol.begin(), protocol.end());

    // Reserved bytes (8 bytes)
    // Byte 7, bit 0x04: Fast Extension (BEP 6)
    // Byte 5, bit 0x10: Extension Protocol (BEP 10)
    std::vector<uint8_t> reserved(8, 0);
    if (supports_fast_extension_) {
        reserved[7] |= 0x04;  // Fast Extension support
    }
    reserved[5] |= 0x10;  // Extension Protocol support
    handshake.insert(handshake.end(), reserved.begin(), reserved.end());

    // Info hash (20 bytes)
    if (info_hash_.size() != 20) {
        std::cerr << "ERROR: Invalid info_hash size: " << info_hash_.size() << " (expected 20)\n";
        return false;
    }
    handshake.insert(handshake.end(), info_hash_.begin(), info_hash_.end());

    // Peer ID (20 bytes)
    if (peer_id_.size() != 20) {
        std::cerr << "ERROR: Invalid peer_id size: " << peer_id_.size() << " (expected 20)\n";
        return false;
    }
    handshake.insert(handshake.end(), peer_id_.begin(), peer_id_.end());

    // Sanity check
    if (handshake.size() != 68) {
        std::cerr << "ERROR: Invalid handshake size: " << handshake.size() << " (expected 68)\n";
        return false;
    }

    // Send handshake
    std::cout << "  Sending handshake (" << handshake.size() << " bytes)...\n";
    if (!sendData(handshake.data(), handshake.size())) {
        std::cerr << "ERROR: Failed to send handshake\n";
        disconnect();
        return false;
    }

    // Receive handshake response (same format, 68 bytes total)
    std::cout << "  Waiting for handshake response...\n";
    std::vector<uint8_t> response(68);
    if (!receiveData(response.data(), response.size())) {
        std::cerr << "ERROR: Failed to receive handshake response\n";
        disconnect();
        return false;
    }

    // Validate response - protocol string length
    if (response[0] != 19) {
        std::cerr << "ERROR: Invalid handshake response - wrong protocol length: "
                  << static_cast<int>(response[0]) << " (expected 19)\n";
        disconnect();
        return false;
    }

    // Validate response - protocol string
    std::string received_protocol(response.begin() + 1, response.begin() + 20);
    if (received_protocol != "BitTorrent protocol") {
        std::cerr << "ERROR: Invalid handshake response - wrong protocol string: '"
                  << received_protocol << "'\n";
        disconnect();
        return false;
    }

    // Extract reserved bytes (bytes 20-27) and check for extension support
    std::vector<uint8_t> reserved_bytes(response.begin() + 20, response.begin() + 28);

    // Check if peer supports Fast Extension (BEP 6)
    // Byte 7, bit 0x04
    if (reserved_bytes[7] & 0x04) {
        peer_supports_fast_extension_ = true;
        LOG_INFO("Peer supports Fast Extension (BEP 6)");
    }

    // Check if peer supports Extension Protocol (BEP 10)
    if (reserved_bytes[5] & 0x10) {
        LOG_DEBUG("Peer supports Extension Protocol (BEP 10)");
    }

    // Verify info_hash matches (bytes 28-47)
    std::vector<uint8_t> received_info_hash(response.begin() + 28, response.begin() + 48);
    if (received_info_hash != info_hash_) {
        std::cerr << "ERROR: Invalid handshake response - info_hash mismatch\n";
        std::cerr << "  Expected: ";
        for (size_t i = 0; i < std::min(info_hash_.size(), size_t(8)); ++i) {
            std::cerr << std::hex << static_cast<int>(info_hash_[i]) << " ";
        }
        std::cerr << "...\n  Received: ";
        for (size_t i = 0; i < std::min(received_info_hash.size(), size_t(8)); ++i) {
            std::cerr << std::hex << static_cast<int>(received_info_hash[i]) << " ";
        }
        std::cerr << std::dec << "...\n";
        disconnect();
        return false;
    }

    // Extract peer_id (bytes 48-67)
    remote_peer_id_.assign(response.begin() + 48, response.end());

    // Validate peer_id size
    if (remote_peer_id_.size() != 20) {
        std::cerr << "ERROR: Invalid peer_id size in response: "
                  << remote_peer_id_.size() << " (expected 20)\n";
        disconnect();
        return false;
    }

    // Mark handshake as completed
    handshake_completed_ = true;

    std::cout << "  [SUCCESS] Handshake completed!\n";
    std::cout << "  Remote Peer ID: ";
    for (size_t i = 0; i < std::min(remote_peer_id_.size(), size_t(20)); ++i) {
        char c = remote_peer_id_[i];
        if (c >= 32 && c <= 126) {
            std::cout << c;
        } else {
            std::cout << '.';
        }
    }
    std::cout << "\n";

    return true;
}

bool PeerConnection::performHandshake(const std::vector<bool>& our_bitfield) {
    // First, perform the standard handshake
    if (!performHandshake()) {
        return false;
    }

    // After successful handshake, send our bitfield if we have any pieces
    bool has_any_piece = false;
    for (bool piece : our_bitfield) {
        if (piece) {
            has_any_piece = true;
            break;
        }
    }

    if (has_any_piece) {
        std::cout << "Sending our bitfield to peer...\n";
        if (!sendBitfield(our_bitfield)) {
            std::cerr << "WARNING: Failed to send bitfield, but handshake succeeded\n";
            // Don't fail the handshake just because bitfield send failed
        } else {
            std::cout << "Successfully sent bitfield to peer\n";
        }
    } else {
        std::cout << "Not sending bitfield (no pieces downloaded yet)\n";
    }

    return true;
}

} // namespace torrent
