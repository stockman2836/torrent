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

void PeerConnection::initializePeerBitfield(size_t num_pieces) {
    peer_bitfield_.clear();
    peer_bitfield_.resize(num_pieces, false);
    std::cout << "Initialized peer bitfield with " << num_pieces << " pieces\n";
}

bool PeerConnection::peerHasPiece(uint32_t piece_index) const {
    if (piece_index >= peer_bitfield_.size()) {
        return false;
    }
    return peer_bitfield_[piece_index];
}

size_t PeerConnection::getPeerPieceCount() const {
    size_t count = 0;
    for (bool has_piece : peer_bitfield_) {
        if (has_piece) {
            count++;
        }
    }
    return count;
}

bool PeerConnection::isPeerSeeder() const {
    if (peer_bitfield_.empty()) {
        return false;
    }

    // A peer is a seeder if it has all pieces
    for (bool has_piece : peer_bitfield_) {
        if (!has_piece) {
            return false;
        }
    }
    return true;
}

bool PeerConnection::requestPiece(uint32_t piece_index, const std::vector<Block>& blocks) {
    if (!isReadyForRequests()) {
        std::cerr << "Cannot request piece: not ready (choked=" << peer_choking_
                  << ", interested=" << am_interested_ << ")\n";
        return false;
    }

    if (!peerHasPiece(piece_index)) {
        std::cerr << "Cannot request piece #" << piece_index << ": peer doesn't have it\n";
        return false;
    }

    std::cout << "Requesting piece #" << piece_index << " (" << blocks.size() << " blocks)\n";

    // Request all blocks for this piece
    int requested = 0;
    for (const auto& block : blocks) {
        if (requestBlock(block.piece_index, block.offset, block.length)) {
            requested++;
        }
    }

    std::cout << "Successfully requested " << requested << "/" << blocks.size() << " blocks\n";
    return requested > 0;
}

bool PeerConnection::requestBlock(uint32_t piece_index, uint32_t offset, uint32_t length) {
    // With Fast Extension, we can request even when choked if piece is in allowed_fast_set
    bool can_request = isReadyForRequests();
    if (!can_request && peer_supports_fast_extension_ && isAllowedFast(piece_index)) {
        can_request = true;
        LOG_DEBUG("Requesting allowed fast piece {} even though choked", piece_index);
    }

    if (!can_request) {
        std::cerr << "Cannot request block: not ready for requests\n";
        return false;
    }

    // Check if already requested
    std::stringstream key;
    key << piece_index << ":" << offset;
    std::string key_str = key.str();

    if (pending_requests_.find(key_str) != pending_requests_.end()) {
        std::cerr << "Block " << key_str << " already requested\n";
        return false;
    }

    // Send REQUEST message
    if (!sendRequest(piece_index, offset, length)) {
        std::cerr << "Failed to send REQUEST message for block " << key_str << "\n";
        return false;
    }

    // Track the request
    pending_requests_.emplace(key_str, PendingRequest(piece_index, offset, length));

    std::cout << "Requested block: piece=" << piece_index
              << " offset=" << offset
              << " length=" << length
              << " (pending: " << pending_requests_.size() << ")\n";

    return true;
}

void PeerConnection::clearPendingRequests() {
    size_t count = pending_requests_.size();
    pending_requests_.clear();
    if (count > 0) {
        std::cout << "Cleared " << count << " pending requests\n";
    }
}

bool PeerConnection::isPendingRequest(uint32_t piece_index, uint32_t offset) const {
    std::stringstream key;
    key << piece_index << ":" << offset;
    return pending_requests_.find(key.str()) != pending_requests_.end();
}

std::vector<PendingRequest> PeerConnection::getTimedOutRequests(int timeout_seconds) {
    std::vector<PendingRequest> timed_out;
    auto now = std::chrono::steady_clock::now();

    for (auto it = pending_requests_.begin(); it != pending_requests_.end(); ) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - it->second.request_time
        ).count();

        if (elapsed >= timeout_seconds) {
            std::cout << "Request timed out: piece=" << it->second.piece_index
                      << " offset=" << it->second.offset
                      << " (waited " << elapsed << "s)\n";
            timed_out.push_back(it->second);
            it = pending_requests_.erase(it);
        } else {
            ++it;
        }
    }

    return timed_out;
}

void PeerConnection::removeRequest(uint32_t piece_index, uint32_t offset) {
    std::stringstream key;
    key << piece_index << ":" << offset;
    std::string key_str = key.str();

    auto it = pending_requests_.find(key_str);
    if (it != pending_requests_.end()) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - it->second.request_time
        ).count();

        std::cout << "Completed request: piece=" << piece_index
                  << " offset=" << offset
                  << " (took " << elapsed << "ms)\n";

        pending_requests_.erase(it);
    }
}

bool PeerConnection::addPendingUpload(uint32_t piece_index, uint32_t offset, uint32_t length) {
    std::stringstream key;
    key << piece_index << ":" << offset;
    std::string key_str = key.str();

    if (pending_uploads_.find(key_str) != pending_uploads_.end()) {
        return false; // Already pending
    }

    pending_uploads_.emplace(key_str, PendingUpload(piece_index, offset, length));
    return true;
}

void PeerConnection::removePendingUpload(uint32_t piece_index, uint32_t offset) {
    std::stringstream key;
    key << piece_index << ":" << offset;
    pending_uploads_.erase(key.str());
}

void PeerConnection::clearPendingUploads() {
    pending_uploads_.clear();
}

bool PeerConnection::isPendingUpload(uint32_t piece_index, uint32_t offset) const {
    std::stringstream key;
    key << piece_index << ":" << offset;
    return pending_uploads_.find(key.str()) != pending_uploads_.end();
}

} // namespace torrent
