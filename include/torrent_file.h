#pragma once

#include "bencode.h"
#include <string>
#include <vector>
#include <cstdint>

namespace torrent {

struct FileInfo {
    std::string path;
    int64_t length;
};

class TorrentFile {
public:
    TorrentFile() = default;

    // Load from .torrent file
    static TorrentFile fromFile(const std::string& filepath);
    static TorrentFile fromData(const std::vector<uint8_t>& data);

    // Create from magnet link metadata (info dict only)
    // info_hash: expected SHA1 hash of the metadata
    // metadata: bencode-encoded info dictionary
    // trackers: optional list of tracker URLs from magnet link
    static TorrentFile fromMetadata(const std::vector<uint8_t>& info_hash,
                                   const std::vector<uint8_t>& metadata,
                                   const std::vector<std::string>& trackers = {});

    // Getters
    const std::string& announce() const { return announce_; }
    const std::vector<std::string>& announceList() const { return announce_list_; }
    const std::string& name() const { return name_; }
    int64_t pieceLength() const { return piece_length_; }
    const std::vector<uint8_t>& pieces() const { return pieces_; }
    int64_t totalLength() const { return total_length_; }
    const std::vector<FileInfo>& files() const { return files_; }
    const std::vector<uint8_t>& infoHash() const { return info_hash_; }
    const std::vector<std::string>& webSeeds() const { return web_seeds_; }

    // Info
    size_t numPieces() const { return pieces_.size() / 20; }
    bool isSingleFile() const { return files_.size() == 1; }
    bool hasWebSeeds() const { return !web_seeds_.empty(); }

    // Display info
    void printInfo() const;

private:
    void parse(const BencodeValue& root);
    void parseInfo(const BencodeValue& info);
    void calculateInfoHash(const std::string& rawInfoDict);

    std::string announce_;
    std::vector<std::string> announce_list_;
    std::string name_;
    int64_t piece_length_;
    std::vector<uint8_t> pieces_;  // SHA1 hashes concatenated
    int64_t total_length_;
    std::vector<FileInfo> files_;
    std::vector<uint8_t> info_hash_;  // SHA1 hash of info dictionary
    std::string comment_;
    std::string created_by_;
    int64_t creation_date_;
    std::vector<std::string> web_seeds_;  // BEP 19: HTTP/FTP web seed URLs
};

} // namespace torrent
