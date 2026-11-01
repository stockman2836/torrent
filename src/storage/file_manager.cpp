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
    : torrent_(torrent), download_dir_(directory), selected_files_(FileSelection(torrent, selected).files()) {}

FileManager::~FileManager() { closeFiles(); }

bool FileManager::initialize() {
    try {
        closeFiles();
        file_handles_.clear();
        fs::create_directories(download_dir_);
        int64_t offset = 0;
        for (size_t index = 0; index < torrent_.files().size(); ++index) {
            const auto& file = torrent_.files()[index];
            FileHandle handle;
            handle.path = (fs::path(download_dir_) / file.path).string();
            handle.offset = offset;
            handle.length = file.length;
            handle.selected = selected_files_[index];
            if (handle.selected) fs::create_directories(fs::path(handle.path).parent_path());
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
        handle.stream.open(handle.path, std::ios::in | std::ios::out | std::ios::binary);
        if (!handle.stream.is_open()) {
            handle.stream.clear();
            { std::ofstream create(handle.path, std::ios::binary); }
            handle.stream.open(handle.path, std::ios::in | std::ios::out | std::ios::binary);
        }
        if (!handle.stream) throw std::runtime_error("Failed to open file: " + handle.path);
        if (fs::file_size(handle.path) < static_cast<uintmax_t>(handle.length)) fs::resize_file(handle.path, handle.length);
    }
}

void FileManager::closeFiles() {
    for (auto& handle : file_handles_) if (handle.stream.is_open()) handle.stream.close();
}

std::string FileManager::cachePath(uint32_t index) const {
    return (fs::path(download_dir_) / (".parts_" + utils::toHex(torrent_.infoHash())) /
            (std::to_string(index) + ".bin")).string();
}

bool FileManager::needsCache(uint32_t index) const {
    const int64_t start = static_cast<int64_t>(index) * torrent_.pieceLength();
    const int64_t end = std::min(start + torrent_.pieceLength(), torrent_.totalLength());
    return std::any_of(file_handles_.begin(), file_handles_.end(), [&](const FileHandle& file) {
        return !file.selected && file.offset < end && file.offset + file.length > start;
    });
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
        if (needsCache(index)) {
            const auto path = cachePath(index);
            fs::create_directories(fs::path(path).parent_path());
            std::ofstream cache(path, std::ios::binary | std::ios::trunc);
            cache.write(reinterpret_cast<const char*>(data.data()), data.size());
            cache.flush();
            if (!cache) return false;
        }
        return true;
    } catch (const std::exception& error) {
        LOG_ERROR("Failed to write piece {}: {}", index, error.what());
        return false;
    }
}

std::vector<uint8_t> FileManager::readPiece(uint32_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= torrent_.numPieces()) return {};
    const int64_t start = static_cast<int64_t>(index) * torrent_.pieceLength();
    const int64_t size = std::min(torrent_.pieceLength(), torrent_.totalLength() - start);
    std::vector<uint8_t> data(size);
    bool cached = false;
    {
        std::ifstream cache(cachePath(index), std::ios::binary);
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
                std::ifstream existing(file.path, std::ios::binary);
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
