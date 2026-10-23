#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace torrent {

// Magnet URI parser (BEP 9)
// Format: magnet:?xt=urn:btih:<info-hash>&dn=<name>&tr=<tracker>...
class MagnetURI {
public:
    // Parse magnet URI string
    static MagnetURI parse(const std::string& uri);

    // Getters
    const std::vector<uint8_t>& infoHash() const { return info_hash_; }
    const std::string& displayName() const { return display_name_; }
    const std::vector<std::string>& trackers() const { return trackers_; }
    const std::vector<std::string>& webSeeds() const { return web_seeds_; }
    std::optional<int64_t> exactLength() const { return exact_length_; }

    // Check if magnet URI is valid
    bool isValid() const { return !info_hash_.empty(); }

    // Get info hash as hex string
    std::string infoHashHex() const;

    // Convert back to magnet URI string
    std::string toString() const;

    // Check if a string is a magnet URI
    static bool isMagnetURI(const std::string& uri);

private:
    MagnetURI() = default;

    // URL decode helper
    static std::string urlDecode(const std::string& str);

    // Parse hex info hash (40 chars) or base32 (32 chars)
    static std::vector<uint8_t> parseInfoHash(const std::string& hash_str);

    std::vector<uint8_t> info_hash_;
    std::string display_name_;
    std::vector<std::string> trackers_;
    std::vector<std::string> web_seeds_;
    std::optional<int64_t> exact_length_;
};

} // namespace torrent
