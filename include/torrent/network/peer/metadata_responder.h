#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace torrent {

// One peer's BEP 9 reply budget. The verified bytes must outlive this responder.
class MetadataResponder {
public:
    explicit MetadataResponder(const std::vector<uint8_t>& verified_metadata);
    size_t metadataSize() const;
    // Empty means ignored (malformed or not a request). Replies include header + data.
    std::vector<uint8_t> reply(const std::vector<uint8_t>& request);
private:
    const std::vector<uint8_t>& metadata_;
    size_t remaining_;
};

} // namespace torrent
