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

bool PeerConnection::performMSEHandshake(bool is_initiator, const std::vector<uint8_t>& info_hash) {
    LOG_INFO("PeerConnection: Starting MSE handshake (initiator={})", is_initiator);

    // Default to prefer encrypted
    MSEHandshake mse(MSEHandshake::Mode::PREFER_ENCRYPTED, info_hash);

    MSEHandshake::Result result;
    if (is_initiator) {
        result = mse.performHandshakeInitiator(*this);
    } else {
        result = mse.performHandshakeResponder(*this);
    }

    if (!result.success) {
        LOG_ERROR("PeerConnection: MSE handshake failed: {}", result.error_message);
        return false;
    }

    // Create encrypted stream
    encrypted_stream_ = std::make_unique<EncryptedStream>();
    encrypted_stream_->init(
        result.selected_method,
        std::move(mse.outgoing_cipher_),
        std::move(mse.incoming_cipher_)
    );

    LOG_INFO("PeerConnection: MSE handshake successful, method: {}",
            result.selected_method == MSEHandshake::CryptoMethod::RC4 ? "RC4" : "plaintext");

    return true;
}

bool PeerConnection::isEncrypted() const {
    return encrypted_stream_ && encrypted_stream_->isEncrypted();
}

void PeerConnection::enablePex() {
    if (pex_manager_) {
        LOG_DEBUG("PEX already enabled for peer {}:{}", ip_, port_);
        return;
    }

    // Create PEX manager
    pex_manager_ = std::make_unique<PexManager>();

    // Ensure extension protocol is initialized
    if (!extension_protocol_) {
        extension_protocol_ = std::make_unique<ExtensionProtocol>();
    }

    // Register PEX extension handler
    extension_protocol_->registerExtension(
        EXT_NAME_PEX,
        [this](uint8_t ext_id, const std::vector<uint8_t>& payload) {
            // Parse incoming PEX message
            if (!pex_manager_) {
                LOG_WARN("Received PEX message but PEX manager not initialized");
                return;
            }

            try {
                BencodeValue data = BencodeParser::parse(payload);
                std::vector<PexPeer> new_peers;
                int count = pex_manager_->parsePexMessage(data, new_peers);

                if (count > 0) {
                    LOG_INFO("PEX: Discovered {} new peers from {}:{}", count, ip_, port_);
                    // New peers will be available via pex_manager_->getKnownPeers()
                }
            } catch (const std::exception& e) {
                LOG_ERROR("Failed to parse PEX message: {}", e.what());
            }
        }
    );

    LOG_INFO("PEX enabled for peer {}:{}", ip_, port_);
}

bool PeerConnection::sendPexMessage() {
    if (!pex_manager_) {
        LOG_WARN("Cannot send PEX message: PEX not enabled");
        return false;
    }

    if (!extension_protocol_) {
        LOG_WARN("Cannot send PEX message: extension protocol not initialized");
        return false;
    }

    if (!extension_protocol_->peerSupportsExtension(EXT_NAME_PEX)) {
        LOG_DEBUG("Cannot send PEX message: peer doesn't support PEX");
        return false;
    }

    // Build PEX message
    BencodeValue pex_data = pex_manager_->buildPexMessage();

    // Send via extension protocol
    std::vector<uint8_t> message = extension_protocol_->buildExtensionMessage(
        EXT_NAME_PEX,
        pex_data
    );

    if (message.empty()) {
        LOG_WARN("Failed to build PEX extension message");
        return false;
    }

    // Send the message
    if (!sendExtendedMessage(extension_protocol_->getPeerExtensionId(EXT_NAME_PEX), message)) {
        LOG_ERROR("Failed to send PEX message to {}:{}", ip_, port_);
        return false;
    }

    pex_manager_->markUpdateSent();
    LOG_DEBUG("Sent PEX message to {}:{}", ip_, port_);
    return true;
}

bool PeerConnection::isAllowedFast(uint32_t piece_index) const {
    return allowed_fast_set_.find(piece_index) != allowed_fast_set_.end();
}

void PeerConnection::generateAllowedFastSet(size_t num_pieces, size_t k) {
    if (!supports_fast_extension_) {
        return;
    }

    // BEP 6 specifies generating allowed fast set using a hash function
    // For simplicity, we'll use a deterministic random selection based on IP and info_hash

    // Clear existing set
    allowed_fast_set_.clear();

    if (num_pieces == 0 || k == 0) {
        return;
    }

    // Limit k to reasonable value
    k = std::min(k, std::min(num_pieces, size_t(10)));

    // Generate k random piece indices
    // In a full implementation, this should use the algorithm from BEP 6
    // which uses SHA1(IP || info_hash) to generate deterministic allowed pieces
    // For now, we'll use a simple approach

    std::hash<std::string> hasher;
    size_t seed = hasher(ip_ + std::to_string(port_));

    for (size_t i = 0; i < k && allowed_fast_set_.size() < k; ++i) {
        // Simple pseudo-random piece selection
        uint32_t piece = (seed + i * 37) % num_pieces;
        allowed_fast_set_.insert(piece);
    }

    LOG_INFO("Generated allowed fast set with {} pieces for peer {}:{}",
             allowed_fast_set_.size(), ip_, port_);

    // Send ALLOWED_FAST messages to peer if they support it
    if (peer_supports_fast_extension_) {
        for (uint32_t piece : allowed_fast_set_) {
            sendAllowedFast(piece);
        }
    }
}

} // namespace torrent
