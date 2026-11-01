#pragma once

#include "input.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace torrent::app {

enum class DownloadState { Idle, Loading, Ready, Downloading, Stopping, Stopped, Complete, Failed };

struct DownloadSnapshot {
    DownloadState state = DownloadState::Idle;
    std::string name;
    std::string error;
    std::shared_ptr<const std::vector<FileInfo>> files;
    double progress = 0;
    int64_t download_speed = 0;
    int64_t upload_speed = 0;
};

// Commands are issued by one UI thread. The engine belongs exclusively to the worker.
class DownloadTask {
public:
    ~DownloadTask();
    bool load(const std::string& source, Config config);
    bool start(Config config);
    bool unload();
    void stop();
    void wait();
    bool busy() const { return busy_.load(); }
    DownloadSnapshot snapshot() const;

private:
    bool begin(DownloadState state);
    void fail(const std::exception& error);
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    DownloadSnapshot snapshot_;
    std::unique_ptr<ResolvedInput> input_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> cancelled_{false};
    std::thread worker_;
};

} // namespace torrent::app
