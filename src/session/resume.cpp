#include "torrent/session/download_manager.h"
#include "torrent/core/logger.h"
#include "torrent/core/utils.h"
#include "torrent/network/discovery/pex_manager.h"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>


namespace torrent {

std::string DownloadManager::getResumeFilePath() const {
    // Generate resume file path: <download_dir>/.resume_<info_hash>.dat
    std::string info_hash_hex = utils::toHex(torrent_.infoHash());
    // Take first 16 chars of hash for filename
    std::string short_hash = info_hash_hex.substr(0, 16);
    return download_dir_ + "/.resume_" + short_hash + ".dat";
}

void DownloadManager::loadResumeState() {
    std::string resume_file = getResumeFilePath();

    std::cout << "Checking for resume data: " << resume_file << "\n";

    if (piece_manager_->loadStateFromDisk(resume_file)) {
        std::cout << "Resume data loaded successfully\n";

        // Verify existing pieces on disk
        verifyExistingPieces();
    } else {
        std::cout << "No resume data found, starting fresh download\n";
    }
}

void DownloadManager::verifyExistingPieces() {
    std::cout << "Verifying existing pieces on disk...\n";

    std::vector<bool> bitfield = piece_manager_->getBitfield();
    int verified = 0;
    int corrupted = 0;

    for (size_t i = 0; i < bitfield.size(); ++i) {
        if (bitfield[i]) {
            // Get expected hash for this piece
            size_t hash_offset = i * 20;  // SHA1 = 20 bytes
            std::vector<uint8_t> expected_hash(
                torrent_.pieces().begin() + hash_offset,
                torrent_.pieces().begin() + hash_offset + 20
            );

            // Verify piece on disk
            if (file_manager_->verifyPiece(i, expected_hash)) {
                verified++;
            } else {
                std::cerr << "Piece " << i << " failed verification, will re-download\n";
                bitfield[i] = false;
                corrupted++;
            }
        }
    }

    // Update bitfield with verified pieces only
    if (corrupted > 0) {
        piece_manager_->setBitfield(bitfield);
        std::cout << "Verification complete: " << verified << " OK, "
                  << corrupted << " corrupted (marked for re-download)\n";
    } else if (verified > 0) {
        std::cout << "Verification complete: all " << verified << " pieces OK\n";
    }
}

void DownloadManager::saveResumeState() {
    std::string resume_file = getResumeFilePath();

    if (piece_manager_->saveStateToDisk(resume_file)) {
        // Success - don't spam logs
    } else {
        std::cerr << "Failed to save resume state\n";
    }
}

void DownloadManager::resumeLoop() {
    std::cout << "Resume loop started (saving state every 30 seconds)\n";

    while (running_) {
        waitForStop(std::chrono::seconds(30));

        if (!running_) {
            break;
        }

        // Save current state
        saveResumeState();
    }

    std::cout << "Resume loop ended\n";
}

} // namespace torrent
