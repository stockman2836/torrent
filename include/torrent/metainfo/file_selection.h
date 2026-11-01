#pragma once

#include "torrent/metainfo/torrent_file.h"
#include <cstddef>
#include <vector>
#include <map>
#include "torrent/core/file_priority.h"

namespace torrent {

// Zero-based metadata indices internally; an empty list selects every file.
class FileSelection {
public:
    FileSelection(const TorrentFile& torrent, const std::vector<size_t>& indices = {},
                  const std::map<size_t, FilePriority>& overrides = {});
    const std::vector<bool>& files() const { return files_; }
    const std::vector<bool>& pieces() const { return pieces_; }
    int64_t selectedBytes() const { return selected_bytes_; }
    const std::vector<FilePriority>& filePriorities() const { return file_priorities_; }
    const std::vector<FilePriority>& piecePriorities() const { return piece_priorities_; }

private:
    std::vector<bool> files_;
    std::vector<bool> pieces_;
    int64_t selected_bytes_ = 0;
    std::vector<FilePriority> file_priorities_;
    std::vector<FilePriority> piece_priorities_;
};

} // namespace torrent
