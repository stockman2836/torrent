#pragma once

#include "torrent/metainfo/torrent_file.h"
#include <string>
#include <vector>
#include <fstream>
#include <mutex>

namespace torrent {

class FileSelection;

class FileManager {
public:
    FileManager(const TorrentFile& torrent, const std::string& download_dir,
                const std::vector<size_t>& selected_files = {});
    FileManager(const TorrentFile& torrent, const std::string& download_dir, const FileSelection& selection);
    ~FileManager();

    // Initialize files (create/allocate space)
    bool initialize();

    // Write piece data to file(s)
    bool writePiece(uint32_t piece_index, const std::vector<uint8_t>& data);

    // Read piece data from file(s)
    std::vector<uint8_t> readPiece(uint32_t piece_index);

    // Verification
    bool verifyExistingFiles();
    bool verifyPiece(uint32_t piece_index, const std::vector<uint8_t>& expected_hash);

private:
    struct FileHandle {
        std::string path;
        std::fstream stream;
        int64_t offset;  // Offset in the combined file space
        int64_t length;
        bool selected;
    };

    void openFiles();
    void closeFiles();
    bool needsCache(uint32_t piece_index) const;
    std::string cachePath(uint32_t piece_index) const;

    const TorrentFile& torrent_;
    std::string download_dir_;
    std::vector<FileHandle> file_handles_;
    std::vector<bool> selected_files_;

    mutable std::mutex mutex_;
};

} // namespace torrent
