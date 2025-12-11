#include "torrent_file.h"
#include "utils.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace torrent {

TorrentFile TorrentFile::fromFile(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + filepath);
    }

    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    file.close();

    return fromData(data);
}

TorrentFile TorrentFile::fromData(const std::vector<uint8_t>& data) {
    TorrentFile torrent;

    // Parse bencode
    BencodeValue root = BencodeParser::parse(data);
    if (!root.isDictionary()) {
        throw std::runtime_error("Torrent file must be a dictionary");
    }

    torrent.parse(root);

    // Calculate info hash
    const auto& dict = root.getDictionary();
    auto it = dict.find("info");
    if (it != dict.end()) {
        std::string encoded_info = BencodeParser::encode(it->second);
        torrent.calculateInfoHash(encoded_info);
    }

    return torrent;
}

TorrentFile TorrentFile::fromMetadata(const std::vector<uint8_t>& info_hash,
                                     const std::vector<uint8_t>& metadata,
                                     const std::vector<std::string>& trackers) {
    TorrentFile torrent;

    // Verify info_hash size
    if (info_hash.size() != 20) {
        throw std::runtime_error("Invalid info_hash size (must be 20 bytes)");
    }

    // Parse metadata as bencode dictionary (it's the info dict)
    BencodeValue info_dict = BencodeParser::parse(metadata);
    if (!info_dict.isDictionary()) {
        throw std::runtime_error("Metadata must be a dictionary");
    }

    // Verify info hash matches
    std::string metadata_str(metadata.begin(), metadata.end());
    auto calculated_hash = utils::sha1(metadata_str);
    if (calculated_hash != info_hash) {
        throw std::runtime_error("Info hash mismatch: metadata verification failed");
    }

    // Parse the info dictionary
    torrent.parseInfo(info_dict);

    // Set info hash
    torrent.info_hash_ = info_hash;

    // Set trackers from magnet link
    if (!trackers.empty()) {
        torrent.announce_ = trackers[0];
        torrent.announce_list_ = trackers;
    }

    return torrent;
}

void TorrentFile::parse(const BencodeValue& root) {
    const auto& dict = root.getDictionary();

    // Announce URL (required)
    auto it = dict.find("announce");
    if (it != dict.end() && it->second.isString()) {
        announce_ = it->second.getString();
    }

    // Announce list (optional)
    it = dict.find("announce-list");
    if (it != dict.end() && it->second.isList()) {
        for (const auto& tier : it->second.getList()) {
            if (tier.isList()) {
                for (const auto& url : tier.getList()) {
                    if (url.isString()) {
                        announce_list_.push_back(url.getString());
                    }
                }
            }
        }
    }

    // Comment (optional)
    it = dict.find("comment");
    if (it != dict.end() && it->second.isString()) {
        comment_ = it->second.getString();
    }

    // Created by (optional)
    it = dict.find("created by");
    if (it != dict.end() && it->second.isString()) {
        created_by_ = it->second.getString();
    }

    // Creation date (optional)
    it = dict.find("creation date");
    if (it != dict.end() && it->second.isInteger()) {
        creation_date_ = it->second.getInteger();
    } else {
        creation_date_ = 0;
    }

    // Info dictionary (required)
    it = dict.find("info");
    if (it == dict.end() || !it->second.isDictionary()) {
        throw std::runtime_error("Missing or invalid 'info' dictionary");
    }

    parseInfo(it->second);
}

void TorrentFile::parseInfo(const BencodeValue& info) {
    const auto& dict = info.getDictionary();

    // Name (required)
    auto it = dict.find("name");
    if (it != dict.end() && it->second.isString()) {
        name_ = it->second.getString();
    } else {
        throw std::runtime_error("Missing 'name' field in info dictionary");
    }

    // Piece length (required)
    it = dict.find("piece length");
    if (it != dict.end() && it->second.isInteger()) {
        piece_length_ = it->second.getInteger();
    } else {
        throw std::runtime_error("Missing 'piece length' field");
    }

    // Pieces (required) - concatenated SHA1 hashes
    it = dict.find("pieces");
    if (it != dict.end() && it->second.isString()) {
        const std::string& pieces_str = it->second.getString();
        pieces_.assign(pieces_str.begin(), pieces_str.end());

        if (pieces_.size() % 20 != 0) {
            throw std::runtime_error("Invalid pieces field (must be multiple of 20)");
        }
    } else {
        throw std::runtime_error("Missing 'pieces' field");
    }

    // Check if single file or multi-file mode
    it = dict.find("length");
    if (it != dict.end() && it->second.isInteger()) {
        // Single file mode
        total_length_ = it->second.getInteger();
        files_.push_back({name_, total_length_});
    } else {
        // Multi-file mode
        it = dict.find("files");
        if (it == dict.end() || !it->second.isList()) {
            throw std::runtime_error("Missing 'files' field for multi-file torrent");
        }

        total_length_ = 0;
        for (const auto& file_entry : it->second.getList()) {
            if (!file_entry.isDictionary()) {
                continue;
            }

            const auto& file_dict = file_entry.getDictionary();

            // Length
            auto len_it = file_dict.find("length");
            if (len_it == file_dict.end() || !len_it->second.isInteger()) {
                continue;
            }
            int64_t length = len_it->second.getInteger();

            // Path
            auto path_it = file_dict.find("path");
            if (path_it == file_dict.end() || !path_it->second.isList()) {
                continue;
            }

            std::string path = name_;
            for (const auto& path_component : path_it->second.getList()) {
                if (path_component.isString()) {
                    path += "/" + path_component.getString();
                }
            }

            files_.push_back({path, length});
            total_length_ += length;
        }
    }
}

void TorrentFile::calculateInfoHash(const std::string& rawInfoDict) {
    info_hash_ = utils::sha1(rawInfoDict);
}

void TorrentFile::printInfo() const {
    std::cout << "=== Torrent Information ===\n";
    std::cout << "Name: " << name_ << "\n";
    std::cout << "Announce: " << announce_ << "\n";

    if (!announce_list_.empty()) {
        std::cout << "Announce list:\n";
        for (const auto& url : announce_list_) {
            std::cout << "  - " << url << "\n";
        }
    }

    std::cout << "Total size: " << utils::formatBytes(total_length_) << "\n";
    std::cout << "Piece length: " << utils::formatBytes(piece_length_) << "\n";
    std::cout << "Number of pieces: " << numPieces() << "\n";

    std::cout << "Info hash: " << utils::toHex(info_hash_) << "\n";

    if (!comment_.empty()) {
        std::cout << "Comment: " << comment_ << "\n";
    }
    if (!created_by_.empty()) {
        std::cout << "Created by: " << created_by_ << "\n";
    }
    if (creation_date_ > 0) {
        std::cout << "Creation date: " << creation_date_ << "\n";
    }

    if (files_.size() > 1) {
        std::cout << "\nFiles (" << files_.size() << "):\n";
        for (const auto& file : files_) {
            std::cout << "  " << file.path << " (" << utils::formatBytes(file.length) << ")\n";
        }
    }

    std::cout << "===========================\n";
}

} // namespace torrent
