#pragma once

#include "torrent_file.h"
#include <string>
#include <vector>
#include <fstream>
#include <mutex>

namespace torrent {

class FileManager {
public:
    FileManager(const TorrentFile& torrent, const std::string& download_dir);
    ~FileManager();

    // Initialize files (create/allocate space)
    bool initialize();

    // Write piece data to file(s)
    bool writePiece(uint32_t piece_index, const std::vector<uint8_t>& data);

    // Read piece data from file(s)
    std::vector<uint8_t> readPiece(uint32_t piece_index);

    // Verification
    bool verifyExistingFiles();

private:
    struct FileHandle {
        std::string path;
        std::fstream stream;
        int64_t offset;  // Offset in the combined file space
        int64_t length;
    };

    void openFiles();
    void closeFiles();

    const TorrentFile& torrent_;
    std::string download_dir_;
    std::vector<FileHandle> file_handles_;

    mutable std::mutex mutex_;
};

} // namespace torrent
