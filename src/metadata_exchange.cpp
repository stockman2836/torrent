#include "metadata_exchange.h"
#include "logger.h"
#include "utils.h"
#include <cstring>

namespace torrent {

MetadataExchange::MetadataExchange(int64_t metadata_size,
                                   MetadataCompleteCallback callback)
    : metadata_size_(metadata_size)
    , on_complete_(callback)
    , complete_(false) {

    if (metadata_size_ > 0) {
        num_pieces_ = (metadata_size_ + METADATA_PIECE_SIZE - 1) / METADATA_PIECE_SIZE;
        pieces_.resize(num_pieces_);
        have_pieces_.resize(num_pieces_, false);

        LOG_INFO("Metadata exchange initialized: {} bytes, {} pieces",
                metadata_size_, num_pieces_);
    } else {
        num_pieces_ = 0;
    }
}

size_t MetadataExchange::totalPieces() const {
    return num_pieces_;
}

bool MetadataExchange::isComplete() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return complete_;
}

double MetadataExchange::progress() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (num_pieces_ == 0) return 0.0;

    size_t received = 0;
    for (bool have : have_pieces_) {
        if (have) received++;
    }

    return (double)received / num_pieces_ * 100.0;
}

bencode::BencodeValue MetadataExchange::buildRequestMessage(size_t piece_index) {
    bencode::BencodeDict dict;
    dict["msg_type"] = bencode::BencodeValue(static_cast<int64_t>(MetadataMessageType::REQUEST));
    dict["piece"] = bencode::BencodeValue(static_cast<int64_t>(piece_index));

    return bencode::BencodeValue(dict);
}

bencode::BencodeValue MetadataExchange::buildDataMessage(
    size_t piece_index,
    const std::vector<uint8_t>& metadata) {

    bencode::BencodeDict dict;
    dict["msg_type"] = bencode::BencodeValue(static_cast<int64_t>(MetadataMessageType::DATA));
    dict["piece"] = bencode::BencodeValue(static_cast<int64_t>(piece_index));
    dict["total_size"] = bencode::BencodeValue(static_cast<int64_t>(metadata.size()));

    return bencode::BencodeValue(dict);
}

bencode::BencodeValue MetadataExchange::buildRejectMessage(size_t piece_index) {
    bencode::BencodeDict dict;
    dict["msg_type"] = bencode::BencodeValue(static_cast<int64_t>(MetadataMessageType::REJECT));
    dict["piece"] = bencode::BencodeValue(static_cast<int64_t>(piece_index));

    return bencode::BencodeValue(dict);
}

void MetadataExchange::handleMessage(const std::vector<uint8_t>& payload) {
    try {
        // ut_metadata message format:
        // [bencoded dict][optional piece data]

        // Find end of bencoded dict
        size_t dict_end = 0;
        int depth = 0;
        bool in_dict = false;

        for (size_t i = 0; i < payload.size(); ++i) {
            if (payload[i] == 'd') {
                in_dict = true;
                depth++;
            } else if (payload[i] == 'e' && in_dict) {
                depth--;
                if (depth == 0) {
                    dict_end = i + 1;
                    break;
                }
            }
        }

        if (dict_end == 0) {
            LOG_WARN("Invalid ut_metadata message: no valid bencoded dict");
            return;
        }

        // Parse dict
        std::vector<uint8_t> dict_data(payload.begin(), payload.begin() + dict_end);
        bencode::BencodeValue value = bencode::decode(dict_data);

        if (!value.isDict()) {
            LOG_WARN("Invalid ut_metadata message: not a dict");
            return;
        }

        const auto& dict = value.asDict();

        // Get message type
        if (dict.find("msg_type") == dict.end() || !dict.at("msg_type").isInt()) {
            LOG_WARN("Invalid ut_metadata message: missing msg_type");
            return;
        }

        int msg_type = static_cast<int>(dict.at("msg_type").asInt());

        // Extract piece data if present
        std::vector<uint8_t> piece_data;
        if (dict_end < payload.size()) {
            piece_data.assign(payload.begin() + dict_end, payload.end());
        }

        // Handle based on type
        switch (static_cast<MetadataMessageType>(msg_type)) {
            case MetadataMessageType::REQUEST:
                handleRequest(dict);
                break;

            case MetadataMessageType::DATA:
                handleData(dict, piece_data);
                break;

            case MetadataMessageType::REJECT:
                handleReject(dict);
                break;

            default:
                LOG_WARN("Unknown ut_metadata message type: {}", msg_type);
                break;
        }

    } catch (const std::exception& e) {
        LOG_ERROR("Failed to handle ut_metadata message: {}", e.what());
    }
}

int MetadataExchange::getNextPieceToRequest() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (complete_) {
        return -1;
    }

    // Find a piece we don't have and haven't requested
    for (size_t i = 0; i < num_pieces_; ++i) {
        if (!have_pieces_[i] && requested_pieces_.find(i) == requested_pieces_.end()) {
            return static_cast<int>(i);
        }
    }

    return -1; // All pieces requested or received
}

void MetadataExchange::markPieceRequested(size_t piece_index) {
    std::lock_guard<std::mutex> lock(mutex_);
    requested_pieces_.insert(piece_index);
}

void MetadataExchange::setMetadata(const std::vector<uint8_t>& metadata) {
    std::lock_guard<std::mutex> lock(mutex_);
    full_metadata_ = metadata;
    metadata_size_ = metadata.size();
    num_pieces_ = (metadata_size_ + METADATA_PIECE_SIZE - 1) / METADATA_PIECE_SIZE;

    LOG_INFO("Metadata set for sharing: {} bytes, {} pieces", metadata_size_, num_pieces_);
}

std::vector<uint8_t> MetadataExchange::getMetadata() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (complete_ && !full_metadata_.empty()) {
        return full_metadata_;
    }

    // Assemble from pieces
    std::vector<uint8_t> metadata;
    for (const auto& piece : pieces_) {
        metadata.insert(metadata.end(), piece.begin(), piece.end());
    }

    return metadata;
}

void MetadataExchange::handleRequest(const bencode::BencodeDict& dict) {
    // Peer is requesting a metadata piece from us
    if (dict.find("piece") == dict.end() || !dict.at("piece").isInt()) {
        return;
    }

    size_t piece_index = static_cast<size_t>(dict.at("piece").asInt());

    LOG_DEBUG("Peer requested metadata piece {}", piece_index);

    // TODO: Send piece if we have full metadata
    // This would be handled by the caller (peer connection)
}

void MetadataExchange::handleData(const bencode::BencodeDict& dict,
                                  const std::vector<uint8_t>& piece_data) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (dict.find("piece") == dict.end() || !dict.at("piece").isInt()) {
        return;
    }

    size_t piece_index = static_cast<size_t>(dict.at("piece").asInt());

    if (piece_index >= num_pieces_) {
        LOG_WARN("Received invalid metadata piece index: {}", piece_index);
        return;
    }

    if (have_pieces_[piece_index]) {
        LOG_DEBUG("Already have metadata piece {}", piece_index);
        return;
    }

    // Store piece
    pieces_[piece_index] = piece_data;
    have_pieces_[piece_index] = true;
    requested_pieces_.erase(piece_index);

    LOG_INFO("Received metadata piece {} ({} bytes). Progress: {:.1f}%",
            piece_index, piece_data.size(), progress());

    // Check if complete
    checkCompletion();
}

void MetadataExchange::handleReject(const bencode::BencodeDict& dict) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (dict.find("piece") == dict.end() || !dict.at("piece").isInt()) {
        return;
    }

    size_t piece_index = static_cast<size_t>(dict.at("piece").asInt());

    LOG_WARN("Peer rejected metadata piece request: {}", piece_index);

    // Remove from requested set so we can try again
    requested_pieces_.erase(piece_index);
}

void MetadataExchange::checkCompletion() {
    // Check if all pieces received
    for (bool have : have_pieces_) {
        if (!have) {
            return; // Not complete yet
        }
    }

    // Assemble full metadata
    full_metadata_.clear();
    for (const auto& piece : pieces_) {
        full_metadata_.insert(full_metadata_.end(), piece.begin(), piece.end());
    }

    // Trim to exact size
    if (full_metadata_.size() > static_cast<size_t>(metadata_size_)) {
        full_metadata_.resize(metadata_size_);
    }

    complete_ = true;

    LOG_INFO("Metadata download complete! Total size: {} bytes", full_metadata_.size());

    // Call completion callback
    if (on_complete_) {
        on_complete_(full_metadata_);
    }
}

} // namespace torrent
