#include "file_manager.h"
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace torrent {

FileManager::FileManager(const TorrentFile& torrent, const std::string& download_dir)
    : torrent_(torrent)
    , download_dir_(download_dir) {
}

FileManager::~FileManager() {
    closeFiles();
}

bool FileManager::initialize() {
    try {
        // Create download directory if it doesn't exist
        fs::create_directories(download_dir_);

        // Create file handles
        int64_t current_offset = 0;
        for (const auto& file_info : torrent_.files()) {
            FileHandle handle;
            handle.path = download_dir_ + "/" + file_info.path;
            handle.offset = current_offset;
            handle.length = file_info.length;

            // Create parent directories
            fs::path file_path(handle.path);
            if (file_path.has_parent_path()) {
                fs::create_directories(file_path.parent_path());
            }

            file_handles_.push_back(std::move(handle));
            current_offset += file_info.length;
        }

        openFiles();
        return true;

    } catch (const std::exception& e) {
        std::cerr << "Failed to initialize files: " << e.what() << "\n";
        return false;
    }
}

void FileManager::openFiles() {
    for (auto& handle : file_handles_) {
        handle.stream.open(handle.path,
                          std::ios::in | std::ios::out | std::ios::binary);

        if (!handle.stream.is_open()) {
            // Try creating the file
            handle.stream.open(handle.path,
                             std::ios::out | std::ios::binary);
            handle.stream.close();

            // Reopen in read/write mode
            handle.stream.open(handle.path,
                             std::ios::in | std::ios::out | std::ios::binary);
        }

        if (!handle.stream.is_open()) {
            throw std::runtime_error("Failed to open file: " + handle.path);
        }
    }
}

void FileManager::closeFiles() {
    for (auto& handle : file_handles_) {
        if (handle.stream.is_open()) {
            handle.stream.close();
        }
    }
}

bool FileManager::writePiece(uint32_t piece_index, const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(mutex_);

    try {
        int64_t piece_offset = static_cast<int64_t>(piece_index) * torrent_.pieceLength();
        int64_t remaining = data.size();
        int64_t data_offset = 0;

        // Write to appropriate file(s)
        for (auto& handle : file_handles_) {
            if (piece_offset >= handle.offset + handle.length) {
                continue;  // This piece starts after this file
            }

            if (piece_offset + remaining <= handle.offset) {
                break;  // This piece ends before this file
            }

            // Calculate write position in this file
            int64_t file_write_offset = std::max(0LL, piece_offset - handle.offset);
            int64_t write_start = std::max(piece_offset, handle.offset);
            int64_t write_end = std::min(piece_offset + static_cast<int64_t>(data.size()),
                                        handle.offset + handle.length);
            int64_t write_size = write_end - write_start;

            if (write_size > 0) {
                handle.stream.seekp(file_write_offset);
                handle.stream.write(reinterpret_cast<const char*>(data.data() + data_offset),
                                  write_size);
                data_offset += write_size;
                remaining -= write_size;
            }
        }

        return true;

    } catch (const std::exception& e) {
        std::cerr << "Failed to write piece " << piece_index << ": " << e.what() << "\n";
        return false;
    }
}

std::vector<uint8_t> FileManager::readPiece(uint32_t piece_index) {
    std::lock_guard<std::mutex> lock(mutex_);

    try {
        int64_t piece_offset = static_cast<int64_t>(piece_index) * torrent_.pieceLength();

        // Determine piece size
        size_t piece_size = torrent_.pieceLength();
        if (piece_index == torrent_.numPieces() - 1) {
            size_t last_piece_size = torrent_.totalLength() % torrent_.pieceLength();
            if (last_piece_size != 0) {
                piece_size = last_piece_size;
            }
        }

        std::vector<uint8_t> data(piece_size);
        int64_t remaining = piece_size;
        int64_t data_offset = 0;

        // Read from appropriate file(s)
        for (auto& handle : file_handles_) {
            if (piece_offset >= handle.offset + handle.length) {
                continue;
            }

            if (piece_offset + piece_size <= handle.offset) {
                break;
            }

            int64_t file_read_offset = std::max(0LL, piece_offset - handle.offset);
            int64_t read_start = std::max(piece_offset, handle.offset);
            int64_t read_end = std::min(piece_offset + static_cast<int64_t>(piece_size),
                                       handle.offset + handle.length);
            int64_t read_size = read_end - read_start;

            if (read_size > 0) {
                handle.stream.seekg(file_read_offset);
                handle.stream.read(reinterpret_cast<char*>(data.data() + data_offset),
                                 read_size);
                data_offset += read_size;
                remaining -= read_size;
            }
        }

        return data;

    } catch (const std::exception& e) {
        std::cerr << "Failed to read piece " << piece_index << ": " << e.what() << "\n";
        return {};
    }
}

bool FileManager::verifyExistingFiles() {
    // TODO: Implement verification of existing files
    return false;
}

} // namespace torrent
