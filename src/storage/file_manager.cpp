#include "torrent/storage/file_manager.h"
#include "torrent/metainfo/file_selection.h"
#include "torrent/core/utils.h"
#include "torrent/core/logger.h"
#include <algorithm>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace torrent {

FileManager::FileManager(const TorrentFile& torrent, const std::string& directory,
                         const std::vector<size_t>& selected)
    : FileManager(torrent, directory, FileSelection(torrent, selected)) {}

FileManager::FileManager(const TorrentFile& torrent, const std::string& directory, const FileSelection& selection)
    : torrent_(torrent), download_dir_(directory), selected_files_(selection.files()) {
    if (selected_files_.size() != torrent.files().size()) throw std::invalid_argument("Invalid file selection size");
}

FileManager::~FileManager() { closeFiles(); }

bool FileManager::initialize() {
    try {
        closeFiles();
        file_handles_.clear();
        fs::create_directories(fs::u8path(download_dir_));
        int64_t offset = 0;
        for (size_t index = 0; index < torrent_.files().size(); ++index) {
            const auto& file = torrent_.files()[index];
            FileHandle handle;
            handle.path = (fs::u8path(download_dir_) / fs::u8path(file.path)).u8string();
            handle.offset = offset;
            handle.length = file.length;
            handle.selected = selected_files_[index];
            if (handle.selected) fs::create_directories(fs::u8path(handle.path).parent_path());
            file_handles_.push_back(std::move(handle));
            offset += file.length;
        }
        openFiles();
        return true;
    } catch (const std::exception& error) {
        LOG_ERROR("Failed to initialize files: {}", error.what());
        return false;
    }
}

void FileManager::openFiles() {
    for (auto& handle : file_handles_) {
        if (!handle.selected) continue;
        openFile(handle);
    }
}

void FileManager::openFile(FileHandle& handle) {
    const auto path = fs::u8path(handle.path);
    fs::create_directories(path.parent_path());
    if (!fs::exists(path)) {
        std::ofstream create(path, std::ios::binary);
        if (!create) throw std::runtime_error("Failed to create file: " + handle.path);
    }
    handle.stream.open(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!handle.stream) throw std::runtime_error("Failed to open file: " + handle.path);
    if (fs::file_size(path) < static_cast<uintmax_t>(handle.length)) fs::resize_file(path, handle.length);
}

void FileManager::setSelection(const FileSelection& selection, const std::vector<bool>& verified) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto selected = selection.files();
    if (selected.size() != file_handles_.size() || verified.size() != torrent_.numPieces())
        throw std::invalid_argument("Invalid live file selection");
    if (selected == selected_files_) return;
    std::vector<std::fstream> opened(selected.size());
    std::vector<bool> created(selected.size(), false);
    for (size_t i = 0; i < selected.size(); ++i) if (selected[i] && !selected_files_[i]) {
        FileHandle handle;
        handle.path = file_handles_[i].path; handle.length = file_handles_[i].length;
        created[i] = !fs::exists(fs::u8path(handle.path));
        openFile(handle);
        opened[i] = std::move(handle.stream);
    }
    for (uint32_t index = 0; index < verified.size(); ++index) {
        if (!verified[index]) continue;
        const int64_t start = static_cast<int64_t>(index) * torrent_.pieceLength();
        const auto end = std::min(start + torrent_.pieceLength(), torrent_.totalLength());
        bool changed = false, wanted = false, skipped = false, missing = false;
        for (size_t i = 0; i < selected.size(); ++i) {
            const auto& file = file_handles_[i];
            if (file.offset >= end || file.offset + file.length <= start) continue;
            changed = changed || selected[i] != selected_files_[i];
            wanted = wanted || selected[i]; skipped = skipped || !selected[i]; missing = missing || created[i];
        }
        if (!changed) continue;
        const bool cached = fs::exists(fs::u8path(cachePath(index)));
        // Whole skipped pieces remain in their existing files. Only boundary
        // pieces and cached data need copying; skipping a large file is cheap.
        if (!(wanted && skipped) && !cached && !missing) continue;
        const auto data = readPieceLocked(index); // Read with the previous mask.
        const auto digest = utils::sha1(data);
        if (data.empty() || !std::equal(digest.begin(), digest.end(), torrent_.pieces().begin() + index * 20))
            throw std::runtime_error("Verified data changed on disk; restart the download to recheck it");
        for (size_t i = 0; i < selected.size(); ++i) if (selected[i] && !selected_files_[i]) {
            const auto& file = file_handles_[i];
            const auto begin = std::max(start, file.offset), finish = std::min(end, file.offset + file.length);
            if (begin >= finish) continue;
            opened[i].seekp(begin - file.offset);
            opened[i].write(reinterpret_cast<const char*>(data.data() + begin - start), finish - begin);
            opened[i].flush();
            if (!opened[i]) throw std::runtime_error("Failed to restore cached file data: " + file.path);
        }
        if (((wanted && skipped) || cached) && !writeCache(index, data))
            throw std::runtime_error("Failed to preserve boundary piece data");
    }
    for (size_t i = 0; i < selected.size(); ++i) {
        auto& file = file_handles_[i];
        if (file.selected && !selected[i]) file.stream.close();
        else if (!file.selected && selected[i]) file.stream = std::move(opened[i]);
        file.selected = selected[i];
    }
    selected_files_.swap(selected);
}

void FileManager::closeFiles() {
    for (auto& handle : file_handles_) if (handle.stream.is_open()) handle.stream.close();
}

std::string FileManager::cachePath(uint32_t index) const {
    return (fs::u8path(download_dir_) / (".parts_" + utils::toHex(torrent_.infoHash())) /
            (std::to_string(index) + ".bin")).u8string();
}

bool FileManager::needsCache(uint32_t index) const {
    const int64_t start = static_cast<int64_t>(index) * torrent_.pieceLength();
    const int64_t end = std::min(start + torrent_.pieceLength(), torrent_.totalLength());
    return std::any_of(file_handles_.begin(), file_handles_.end(), [&](const FileHandle& file) {
        return !file.selected && file.offset < end && file.offset + file.length > start;
    });
}

bool FileManager::writeCache(uint32_t index, const std::vector<uint8_t>& data) {
    const auto path = fs::u8path(cachePath(index));
    fs::create_directories(path.parent_path());
    std::ofstream cache(path, std::ios::binary | std::ios::trunc);
    cache.write(reinterpret_cast<const char*>(data.data()), data.size());
    cache.close();
    return static_cast<bool>(cache);
}

bool FileManager::writePiece(uint32_t index, const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= torrent_.numPieces()) return false;
    const int64_t start = static_cast<int64_t>(index) * torrent_.pieceLength();
    const int64_t size = std::min(torrent_.pieceLength(), torrent_.totalLength() - start);
    if (data.size() != static_cast<size_t>(size)) return false;
    try {
        int64_t written = 0;
        for (auto& file : file_handles_) {
            if (!file.selected) continue;
            const auto begin = std::max(start, file.offset);
            const auto end = std::min(start + size, file.offset + file.length);
            if (begin >= end) continue;
            file.stream.clear();
            file.stream.seekp(begin - file.offset);
            file.stream.write(reinterpret_cast<const char*>(data.data() + (begin - start)), end - begin);
            file.stream.flush();
            if (!file.stream) return false;
            written += end - begin;
        }
        if (written == 0) return false;
        if (needsCache(index) && !writeCache(index, data)) return false;
        return true;
    } catch (const std::exception& error) {
        LOG_ERROR("Failed to write piece {}: {}", index, error.what());
        return false;
    }
}

std::vector<uint8_t> FileManager::readPiece(uint32_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    return readPieceLocked(index);
}

std::vector<uint8_t> FileManager::readPieceLocked(uint32_t index) {
    if (index >= torrent_.numPieces()) return {};
    const int64_t start = static_cast<int64_t>(index) * torrent_.pieceLength();
    const int64_t size = std::min(torrent_.pieceLength(), torrent_.totalLength() - start);
    std::vector<uint8_t> data(size);
    bool cached = false;
    {
        std::ifstream cache(fs::u8path(cachePath(index)), std::ios::binary);
        if (cache) {
            cache.read(reinterpret_cast<char*>(data.data()), size);
            cached = cache.gcount() == size;
        }
    }
    try {
        int64_t covered = 0;
        for (auto& file : file_handles_) {
            const auto begin = std::max(start, file.offset);
            const auto end = std::min(start + size, file.offset + file.length);
            if (begin >= end) continue;
            std::vector<uint8_t> bytes(end - begin);
            bool read = false;
            if (file.selected) {
                file.stream.clear();
                file.stream.seekg(begin - file.offset);
                file.stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
                if (!file.stream) return {};
                read = true;
            } else if (!cached) {
                // Previously selected files may exist. Read them without opening for writing.
                std::ifstream existing(fs::u8path(file.path), std::ios::binary);
                if (existing) {
                    existing.seekg(begin - file.offset);
                    existing.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
                    read = static_cast<bool>(existing);
                }
            }
            if (read) std::copy(bytes.begin(), bytes.end(), data.begin() + (begin - start));
            else if (!cached) return {};
            covered += end - begin;
        }
        return covered == size ? data : std::vector<uint8_t>{};
    } catch (const std::exception& error) {
        LOG_ERROR("Failed to read piece {}: {}", index, error.what());
        return {};
    }
}

bool FileManager::verifyExistingFiles() {
    for (size_t index = 0; index < torrent_.numPieces(); ++index) {
        const auto offset = index * 20;
        if (!verifyPiece(index, {torrent_.pieces().begin() + offset, torrent_.pieces().begin() + offset + 20})) return false;
    }
    return true;
}

bool FileManager::verifyPiece(uint32_t index, const std::vector<uint8_t>& expected) {
    const auto data = readPiece(index);
    return !data.empty() && utils::sha1(data) == expected;
}

} // namespace torrent
