#include "torrent/metainfo/file_selection.h"
#include <stdexcept>
#include <algorithm>

namespace torrent {

FileSelection::FileSelection(const TorrentFile& torrent, const std::vector<size_t>& indices,
                             const std::map<size_t, FilePriority>& overrides)
    : files_(torrent.files().size(), indices.empty()), pieces_(torrent.numPieces(), false),
      file_priorities_(torrent.files().size(), FilePriority::Skip),
      piece_priorities_(torrent.numPieces(), FilePriority::Skip) {
    if (torrent.pieceLength() <= 0) throw std::invalid_argument("Invalid torrent piece length");
    if (torrent.totalLength() < 0) throw std::invalid_argument("Invalid torrent total size");
    const auto expected_pieces = torrent.totalLength() / torrent.pieceLength() +
        (torrent.totalLength() % torrent.pieceLength() != 0);
    if (static_cast<uint64_t>(expected_pieces) != torrent.numPieces()) {
        throw std::invalid_argument("Torrent piece table does not match its size");
    }
    for (const auto index : indices) {
        if (index >= files_.size()) throw std::invalid_argument("Selected file number is outside the torrent");
        files_[index] = true;
    }
    for (size_t i = 0; i < files_.size(); ++i) {
        if (files_[i]) file_priorities_[i] = FilePriority::Normal;
    }
    for (const auto& entry : overrides) {
        const auto index = entry.first;
        if (index >= files_.size()) throw std::invalid_argument("Priority file number is outside the torrent");
        if (entry.second > FilePriority::High) throw std::invalid_argument("Invalid file priority");
        if (!files_[index] && entry.second != FilePriority::Skip) {
            throw std::invalid_argument("Priority refers to a file excluded by --files");
        }
        file_priorities_[index] = entry.second;
        files_[index] = entry.second != FilePriority::Skip;
    }
    int64_t offset = 0;
    for (size_t index = 0; index < files_.size(); ++index) {
        const auto length = torrent.files()[index].length;
        if (length < 0 || length > torrent.totalLength() - offset) throw std::invalid_argument("Invalid torrent file length");
        if (files_[index]) {
            selected_bytes_ += length;
            if (length > 0) {
                const auto first = offset / torrent.pieceLength();
                const auto last = (offset + length - 1) / torrent.pieceLength();
                for (auto piece = first; piece <= last; ++piece) {
                    pieces_[piece] = true;
                    piece_priorities_[piece] = std::max(piece_priorities_[piece], file_priorities_[index]);
                }
            }
        }
        offset += length;
    }
    if (offset != torrent.totalLength()) throw std::invalid_argument("Torrent file lengths do not match total size");
}

} // namespace torrent
