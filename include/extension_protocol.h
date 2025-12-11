#pragma once

#include "bencode.h"
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <functional>

namespace torrent {

// BEP 10: Extension Protocol
// Allows peers to exchange extension capabilities

constexpr uint8_t MSG_EXTENDED = 20;  // Extension protocol message ID
constexpr uint8_t EXT_HANDSHAKE = 0;  // Extended handshake

// Well-known extension names
constexpr const char* EXT_NAME_METADATA = "ut_metadata";  // BEP 9
constexpr const char* EXT_NAME_PEX = "ut_pex";            // BEP 11

// Extension message handler callback
// Parameters: extension_id, payload
using ExtensionHandler = std::function<void(uint8_t, const std::vector<uint8_t>&)>;

class ExtensionProtocol {
public:
    ExtensionProtocol();

    // Register an extension
    // Returns local extension ID
    uint8_t registerExtension(const std::string& name, ExtensionHandler handler);

    // Build extended handshake message
    std::vector<uint8_t> buildExtendedHandshake() const;

    // Parse extended handshake from peer
    void parseExtendedHandshake(const std::vector<uint8_t>& payload);

    // Build extension message
    std::vector<uint8_t> buildExtensionMessage(const std::string& ext_name,
                                               const bencode::BencodeValue& data) const;

    // Handle incoming extension message
    void handleExtensionMessage(uint8_t msg_id, const std::vector<uint8_t>& payload);

    // Get peer's extension ID for a given extension name
    // Returns 0 if peer doesn't support the extension
    uint8_t getPeerExtensionId(const std::string& name) const;

    // Check if peer supports an extension
    bool peerSupportsExtension(const std::string& name) const;

    // Get peer metadata
    int64_t getPeerMetadataSize() const { return peer_metadata_size_; }
    std::string getPeerClientName() const { return peer_client_name_; }

    // Set our metadata
    void setMetadataSize(int64_t size) { our_metadata_size_ = size; }
    void setClientName(const std::string& name) { our_client_name_ = name; }

private:
    // Our extensions: name -> (local_id, handler)
    std::map<std::string, std::pair<uint8_t, ExtensionHandler>> our_extensions_;

    // Peer's extensions: name -> peer_id
    std::map<std::string, uint8_t> peer_extensions_;

    // Extension ID -> handler lookup
    std::map<uint8_t, ExtensionHandler> id_to_handler_;

    // Metadata
    int64_t our_metadata_size_ = 0;
    std::string our_client_name_ = "BitTorrent Client 1.0";

    int64_t peer_metadata_size_ = 0;
    std::string peer_client_name_;

    uint8_t next_extension_id_ = 1; // Start from 1 (0 is handshake)
};

} // namespace torrent
