#include "torrent/network/peer/metadata_responder.h"
#include "torrent/network/peer/metadata_exchange.h"
#include <algorithm>

namespace torrent {

MetadataResponder::MetadataResponder(const std::vector<uint8_t>& metadata)
    : metadata_(metadata), remaining_(4 * ((metadataSize() + METADATA_PIECE_SIZE - 1) / METADATA_PIECE_SIZE)) {}

size_t MetadataResponder::metadataSize() const {
    return metadata_.size() <= MAX_METADATA_SIZE ? metadata_.size() : 0;
}

std::vector<uint8_t> MetadataResponder::reply(const std::vector<uint8_t>& request) {
    // Requests have only a small bencoded header. Bound parser work and nesting;
    // unsolicited data and future message types need no response.
    if (request.size() > 256) return {};
    try {
        const auto value = BencodeParser::parse(request);
        if (!value.isDictionary()) return {};
        const auto& dict = value.getDictionary();
        const auto type = dict.find("msg_type"), piece = dict.find("piece");
        if (type == dict.end() || !type->second.isInteger() || type->second.getInteger() != 0 ||
            piece == dict.end() || !piece->second.isInteger()) return {};
        const auto index = piece->second.getInteger();
        const auto count = (metadataSize() + METADATA_PIECE_SIZE - 1) / METADATA_PIECE_SIZE;
        const bool available = index >= 0 && uint64_t(index) < count && remaining_ > 0;
        BencodeValue::Dictionary header{{"msg_type", int64_t(available ? 1 : 2)}, {"piece", index}};
        if (available) header["total_size"] = int64_t(metadataSize());
        const auto encoded = BencodeParser::encode(header);
        std::vector<uint8_t> result(encoded.begin(), encoded.end());
        if (available) {
            const auto offset = static_cast<size_t>(index) * METADATA_PIECE_SIZE;
            const auto length = std::min(METADATA_PIECE_SIZE, metadata_.size() - offset);
            result.insert(result.end(), metadata_.begin() + offset, metadata_.begin() + offset + length);
            --remaining_;
        }
        return result;
    } catch (const BencodeError&) { return {}; }
}

} // namespace torrent
