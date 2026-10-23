#include "torrent/network/peer/metadata_exchange.h"
#include "torrent/core/logger.h"
#include <algorithm>
#include <stdexcept>

namespace torrent {

MetadataExchange::MetadataExchange(int64_t size, MetadataCompleteCallback callback)
    : metadata_size_(size), num_pieces_(0), on_complete_(std::move(callback)), complete_(false) {
    if (size <= 0 || size > MAX_METADATA_SIZE) throw std::invalid_argument("Invalid metadata size");
    num_pieces_ = (size + METADATA_PIECE_SIZE - 1) / METADATA_PIECE_SIZE;
    pieces_.resize(num_pieces_);
    have_pieces_.assign(num_pieces_, false);
}

size_t MetadataExchange::totalPieces() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return num_pieces_;
}

bool MetadataExchange::isComplete() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return complete_;
}

bool MetadataExchange::hasFailed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failed_;
}

double MetadataExchange::progress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return 100.0 * std::count(have_pieces_.begin(), have_pieces_.end(), true) / num_pieces_;
}

BencodeValue MetadataExchange::buildRequestMessage(size_t index) {
    return BencodeValue::Dictionary{{"msg_type", int64_t(0)}, {"piece", int64_t(index)}};
}

BencodeValue MetadataExchange::buildDataMessage(size_t index, const std::vector<uint8_t>& metadata) {
    return BencodeValue::Dictionary{{"msg_type", int64_t(1)}, {"piece", int64_t(index)},
                                    {"total_size", int64_t(metadata.size())}};
}

BencodeValue MetadataExchange::buildRejectMessage(size_t index) {
    return BencodeValue::Dictionary{{"msg_type", int64_t(2)}, {"piece", int64_t(index)}};
}

void MetadataExchange::handleMessage(const std::vector<uint8_t>& payload) {
    try {
        const auto parsed = BencodeParser::parsePrefix(payload);
        if (!parsed.first.isDictionary()) throw std::invalid_argument("Metadata header is not a dictionary");
        const auto& dict = parsed.first.getDictionary();
        const auto type = dict.find("msg_type");
        if (type == dict.end() || !type->second.isInteger()) throw std::invalid_argument("Missing metadata message type");
        switch (type->second.getInteger()) {
            case 0: handleRequest(dict); break;
            case 1: handleData(dict, std::vector<uint8_t>(payload.begin() + parsed.second, payload.end())); break;
            case 2: handleReject(dict); break;
            default: break; // BEP 9: ignore unknown message types.
        }
    } catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        failed_ = true;
        LOG_WARN("Invalid metadata message: {}", error.what());
    }
}

int MetadataExchange::getNextPieceToRequest() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (complete_ || failed_) return -1;
    for (size_t i = 0; i < num_pieces_; ++i) {
        if (!have_pieces_[i] && requested_pieces_.count(i) == 0) return static_cast<int>(i);
    }
    return -1;
}

void MetadataExchange::markPieceRequested(size_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= num_pieces_) throw std::out_of_range("Invalid metadata piece index");
    requested_pieces_.insert(index);
}

void MetadataExchange::setMetadata(const std::vector<uint8_t>& metadata) {
    if (metadata.empty() || metadata.size() > MAX_METADATA_SIZE) throw std::invalid_argument("Invalid metadata size");
    std::lock_guard<std::mutex> lock(mutex_);
    full_metadata_ = metadata;
    metadata_size_ = metadata.size();
    num_pieces_ = (metadata.size() + METADATA_PIECE_SIZE - 1) / METADATA_PIECE_SIZE;
    have_pieces_.assign(num_pieces_, true);
    pieces_.clear();
    requested_pieces_.clear();
    complete_ = true;
    failed_ = false;
}

std::vector<uint8_t> MetadataExchange::getMetadata() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (complete_) return full_metadata_;
    std::vector<uint8_t> data;
    for (const auto& piece : pieces_) data.insert(data.end(), piece.begin(), piece.end());
    return data;
}

void MetadataExchange::handleRequest(const BencodeValue::Dictionary&) {
    // Sending metadata is a separate seeding path; this class collects responses.
}

void MetadataExchange::handleData(const BencodeValue::Dictionary& dict, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> completed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (complete_ || failed_) return;
        const auto piece = dict.find("piece"), size = dict.find("total_size");
        if (piece == dict.end() || !piece->second.isInteger() || piece->second.getInteger() < 0 ||
            size == dict.end() || !size->second.isInteger() || size->second.getInteger() != metadata_size_) {
            failed_ = true;
            return;
        }
        const auto index = static_cast<size_t>(piece->second.getInteger());
        if (index >= num_pieces_ || data.size() != std::min(METADATA_PIECE_SIZE,
            static_cast<size_t>(metadata_size_) - index * METADATA_PIECE_SIZE)) {
            failed_ = true;
            return;
        }
        if (have_pieces_[index]) return;
        pieces_[index] = data;
        have_pieces_[index] = true;
        requested_pieces_.erase(index);
        if (!std::all_of(have_pieces_.begin(), have_pieces_.end(), [](bool have) { return have; })) return;
        full_metadata_.reserve(metadata_size_);
        for (const auto& bytes : pieces_) full_metadata_.insert(full_metadata_.end(), bytes.begin(), bytes.end());
        complete_ = true;
        completed = full_metadata_;
    }
    // Consumers may query the exchange or verify the hash in their callback.
    if (on_complete_) on_complete_(completed);
}

void MetadataExchange::handleReject(const BencodeValue::Dictionary& dict) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto piece = dict.find("piece");
    if (piece != dict.end() && piece->second.isInteger() && piece->second.getInteger() >= 0 &&
        static_cast<uint64_t>(piece->second.getInteger()) < num_pieces_) failed_ = true;
}

} // namespace torrent
