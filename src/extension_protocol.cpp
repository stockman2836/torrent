#include "extension_protocol.h"
#include "logger.h"
#include <cstring>

namespace torrent {

ExtensionProtocol::ExtensionProtocol() {}

uint8_t ExtensionProtocol::registerExtension(const std::string& name,
                                             ExtensionHandler handler) {
    if (our_extensions_.find(name) != our_extensions_.end()) {
        return our_extensions_[name].first; // Already registered
    }

    uint8_t id = next_extension_id_++;
    our_extensions_[name] = {id, handler};
    id_to_handler_[id] = handler;

    LOG_DEBUG("Registered extension '{}' with ID {}", name, id);
    return id;
}

std::vector<uint8_t> ExtensionProtocol::buildExtendedHandshake() const {
    bencode::BencodeDict dict;

    // Add extension mappings
    bencode::BencodeDict extensions;
    for (const auto& [name, id_handler] : our_extensions_) {
        extensions[name] = bencode::BencodeValue(static_cast<int64_t>(id_handler.first));
    }
    dict["m"] = bencode::BencodeValue(extensions);

    // Add metadata size if available
    if (our_metadata_size_ > 0) {
        dict["metadata_size"] = bencode::BencodeValue(our_metadata_size_);
    }

    // Add client version
    dict["v"] = bencode::BencodeValue(our_client_name_);

    // Add uTP port if enabled (BEP 29)
    if (our_utp_port_ > 0) {
        dict["p"] = bencode::BencodeValue(static_cast<int64_t>(our_utp_port_));
        LOG_DEBUG("Advertising uTP support on port {}", our_utp_port_);
    }

    // Encode
    bencode::BencodeValue value(dict);
    std::vector<uint8_t> encoded = bencode::encode(value);

    // Build message: [msg_id=20][ext_id=0][bencoded_dict]
    std::vector<uint8_t> message;
    message.push_back(MSG_EXTENDED);
    message.push_back(EXT_HANDSHAKE);
    message.insert(message.end(), encoded.begin(), encoded.end());

    return message;
}

void ExtensionProtocol::parseExtendedHandshake(const std::vector<uint8_t>& payload) {
    try {
        bencode::BencodeValue value = bencode::decode(payload);

        if (!value.isDict()) {
            LOG_WARN("Invalid extended handshake: not a dictionary");
            return;
        }

        const auto& dict = value.asDict();

        // Parse extension mappings
        if (dict.find("m") != dict.end() && dict.at("m").isDict()) {
            const auto& extensions = dict.at("m").asDict();

            for (const auto& [name, id_val] : extensions) {
                if (id_val.isInt()) {
                    uint8_t peer_id = static_cast<uint8_t>(id_val.asInt());
                    peer_extensions_[name] = peer_id;
                    LOG_DEBUG("Peer supports extension '{}' with ID {}", name, peer_id);
                }
            }
        }

        // Parse metadata size
        if (dict.find("metadata_size") != dict.end() && dict.at("metadata_size").isInt()) {
            peer_metadata_size_ = dict.at("metadata_size").asInt();
            LOG_DEBUG("Peer metadata size: {} bytes", peer_metadata_size_);
        }

        // Parse client version
        if (dict.find("v") != dict.end() && dict.at("v").isString()) {
            peer_client_name_ = dict.at("v").asString();
            LOG_DEBUG("Peer client: {}", peer_client_name_);
        }

        // Parse uTP port (BEP 29)
        if (dict.find("p") != dict.end() && dict.at("p").isInt()) {
            peer_utp_port_ = static_cast<uint16_t>(dict.at("p").asInt());
            LOG_INFO("Peer supports uTP on port {}", peer_utp_port_);
        }

    } catch (const std::exception& e) {
        LOG_ERROR("Failed to parse extended handshake: {}", e.what());
    }
}

std::vector<uint8_t> ExtensionProtocol::buildExtensionMessage(
    const std::string& ext_name,
    const bencode::BencodeValue& data) const {

    // Get peer's extension ID
    auto it = peer_extensions_.find(ext_name);
    if (it == peer_extensions_.end()) {
        LOG_WARN("Peer doesn't support extension '{}'", ext_name);
        return {};
    }

    uint8_t peer_ext_id = it->second;

    // Encode data
    std::vector<uint8_t> encoded = bencode::encode(data);

    // Build message: [msg_id=20][peer_ext_id][bencoded_data]
    std::vector<uint8_t> message;
    message.push_back(MSG_EXTENDED);
    message.push_back(peer_ext_id);
    message.insert(message.end(), encoded.begin(), encoded.end());

    return message;
}

void ExtensionProtocol::handleExtensionMessage(uint8_t msg_id,
                                               const std::vector<uint8_t>& payload) {
    if (payload.empty()) {
        return;
    }

    uint8_t ext_id = payload[0];
    std::vector<uint8_t> data(payload.begin() + 1, payload.end());

    if (ext_id == EXT_HANDSHAKE) {
        parseExtendedHandshake(data);
        return;
    }

    // Find handler for this extension ID
    auto it = id_to_handler_.find(ext_id);
    if (it != id_to_handler_.end()) {
        it->second(ext_id, data);
    } else {
        LOG_DEBUG("Received extension message for unknown ID: {}", ext_id);
    }
}

uint8_t ExtensionProtocol::getPeerExtensionId(const std::string& name) const {
    auto it = peer_extensions_.find(name);
    if (it != peer_extensions_.end()) {
        return it->second;
    }
    return 0; // Not supported
}

bool ExtensionProtocol::peerSupportsExtension(const std::string& name) const {
    return peer_extensions_.find(name) != peer_extensions_.end();
}

} // namespace torrent
