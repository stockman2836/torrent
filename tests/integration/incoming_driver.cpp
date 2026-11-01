#include "download_queue.h"
#include "torrent/storage/file_manager.h"
#include "torrent/storage/piece_manager.h"
#include "torrent/core/utils.h"
#include <chrono>
#include <functional>
#include <iostream>
#include <sstream>
#include <thread>

using namespace torrent;
using namespace torrent::app;

// Build honest resume data from fixture files, then let normal session startup
// independently verify those pieces again before accepting peers.
void prepareResume(const std::string& source, const Config& config) {
    const auto torrent = TorrentFile::fromFile(source);
    FileManager files(torrent, config.download_dir);
    if (!files.initialize()) throw std::runtime_error("Cannot open fixture files");
    PieceManager pieces(torrent.numPieces(), torrent.pieceLength(), torrent.totalLength(), torrent.pieces());
    std::vector<bool> verified(torrent.numPieces(), false);
    for (size_t i = 0; i < verified.size(); ++i) {
        const auto hash = torrent.pieces().begin() + i * 20;
        verified[i] = files.verifyPiece(i, {hash, hash + 20});
    }
    pieces.setBitfield(verified);
    if (!pieces.saveStateToDisk(config.download_dir + "/.resume_" + utils::toHex(torrent.infoHash()).substr(0, 16) + ".dat"))
        throw std::runtime_error("Cannot save fixture resume data");
}

void until(DownloadQueue& queue, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    do {
        queue.tick();
        for (const auto& entry : queue.snapshots()) if (entry.download.state == DownloadState::Failed)
            throw std::runtime_error(entry.download.error);
        if (done()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("Incoming fixture timed out");
}

int main(int argc, char* argv[]) {
    try {
        if (argc != 3 && argc != 4) return 2;
        Config config;
        if (!config.loadFromFile(argv[2]) || !config.validate()) return 2;
        DownloadQueue queue;
        std::vector<DownloadId> ids;
        for (const auto& source : argc == 4 ? std::vector<std::string>{argv[1], argv[3]} : std::vector<std::string>{argv[1]}) {
            prepareResume(source, config);
            auto id = queue.add(source, config); ids.push_back(id);
            queue.setSeedingOptions(id, {true, 0, 0}); queue.enqueue(id);
        }
        queue.setLimit(2);
        const auto ready = [&] {
            for (const auto& entry : queue.snapshots()) if (!entry.busy || !entry.download.listen_port) return false;
            return true;
        };
        until(queue, ready);
        std::cout << "INCOMING_READY\n" << std::flush;
        std::string command;
        while (std::getline(std::cin, command) && command != "stop") {
            if (command == "restart") {
                for (auto id : ids) queue.stop(id);
                until(queue, [&] { for (const auto& entry : queue.snapshots()) if (entry.busy) return false; return true; });
                for (auto id : ids) queue.enqueue(id);
                until(queue, ready);
                std::cout << "INCOMING_RESTARTED\n" << std::flush;
            } else if (command == "remove") {
                queue.remove(ids.front());
                until(queue, [&] { for (const auto& entry : queue.snapshots()) if (entry.id == ids.front()) return false; return true; });
                std::cout << "INCOMING_REMOVED\n" << std::flush;
            } else if (command == "complete") {
                until(queue, [&] { for (const auto& entry : queue.snapshots()) if (!entry.download.can_seed) return false; return true; });
                std::cout << "INCOMING_COMPLETE\n" << std::flush;
            } else {
                std::istringstream input(command);
                std::string key; int64_t down, up;
                if (!(input >> key >> down >> up) || key != "limits") throw std::runtime_error("Unexpected fixture command");
                queue.setSpeedLimits(ids.front(), down, up);
                std::cout << "INCOMING_LIMITS\n" << std::flush;
            }
        }
        queue.close(); until(queue, [&] { return queue.closed(); });
        std::cout << "INCOMING_STOPPED\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
