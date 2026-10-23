#pragma once

#include "torrent/network/peer/extension_protocol.h"
#include "torrent/metainfo/bencode.h"
#include <vector>
#include <set>
#include <mutex>
#include <functional>
#include <cstdint>

namespace torrent {

// BEP 9: Extension for Peers to Send Metadata Files
// Allows downloading .torrent metadata via ut_metadata extension

constexpr size_t METADATA_PIECE_SIZE = 16384; // 16 KB
constexpr int64_t MAX_METADATA_SIZE = 4 * 1024 * 1024;

enum class MetadataMessageType : int {
    REQUEST = 0,
    DATA = 1,
    REJECT = 2
};

class MetadataExchange {
public:
    using MetadataCompleteCallback = std::function<void(const std::vector<uint8_t>&)>;

    MetadataExchange(int64_t metadata_size, MetadataCompleteCallback callback);

    // Get total number of pieces
    size_t totalPieces() const;

    // Check if metadata is complete
    bool isComplete() const;
    bool hasFailed() const;

    // Get completion percentage
    double progress() const;

    // Build request message for a piece
    BencodeValue buildRequestMessage(size_t piece_index);

    // Build data message for a piece (when we have metadata to share)
    BencodeValue buildDataMessage(size_t piece_index,
                                          const std::vector<uint8_t>& metadata);

    // Build reject message
    BencodeValue buildRejectMessage(size_t piece_index);

    // Handle incoming ut_metadata message
    void handleMessage(const std::vector<uint8_t>& payload);

    // Get next piece to request
    // Returns -1 if all pieces requested or complete
    int getNextPieceToRequest();

    // Mark piece as requested
    void markPieceRequested(size_t piece_index);

    // Set full metadata (for sharing)
    void setMetadata(const std::vector<uint8_t>& metadata);

    // Get current metadata (may be incomplete)
    std::vector<uint8_t> getMetadata() const;

private:
    // Handle request message
    void handleRequest(const BencodeValue::Dictionary& dict);

    // Handle data message
    void handleData(const BencodeValue::Dictionary& dict,
                   const std::vector<uint8_t>& piece_data);

    // Handle reject message
    void handleReject(const BencodeValue::Dictionary& dict);

    int64_t metadata_size_;
    size_t num_pieces_;

    std::vector<std::vector<uint8_t>> pieces_;
    std::vector<bool> have_pieces_;
    std::set<size_t> requested_pieces_;

    std::vector<uint8_t> full_metadata_; // For sharing

    MetadataCompleteCallback on_complete_;
    mutable std::mutex mutex_;
    bool complete_;
    bool failed_ = false;
};

} // namespace torrent
