#pragma once

#include "input.h"
#include "seeding_options.h"
#include "torrent/session/network_status.h"
#include "torrent/core/utils.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace torrent::app {

enum class DownloadState { Idle, Loading, Ready, Downloading, Seeding, Stopping, Stopped, Complete, Failed };

struct DownloadSnapshot {
    DownloadState state = DownloadState::Idle;
    std::string name;
    std::string info_hash;
    std::string error;
    std::shared_ptr<const std::vector<FileInfo>> files;
    double progress = 0;
    int64_t download_speed = 0;
    int64_t upload_speed = 0;
    int64_t uploaded_bytes = 0;
    int64_t seed_seconds = 0;
    double seed_ratio = 0;
    bool can_seed = false;
    bool priorities_pending = false;
    uint16_t listen_port = 0;
    NetworkStatus network;
};

// Commands are issued by one UI thread. The engine belongs exclusively to the worker.
class DownloadTask {
public:
    ~DownloadTask();
    bool load(const std::string& source, Config config);
    bool start(Config config, std::shared_ptr<utils::BandwidthLimits> global_limits = nullptr);
    bool unload();
    void stop();
    void setSpeedLimits(int64_t download_bytes_per_sec, int64_t upload_bytes_per_sec);
    void setSeedingOptions(const SeedingOptions& options);
    void setPortMappingEnabled(bool enabled);
    bool setFilePriorities(const std::map<size_t, FilePriority>& priorities);
    void wait();
    bool busy() const { return busy_.load(); }
    DownloadSnapshot snapshot() const;

private:
    bool begin(DownloadState state);
    void fail(const std::exception& error);
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    DownloadSnapshot snapshot_;
    int64_t download_limit_ = 0;
    int64_t upload_limit_ = 0;
    SeedingOptions seeding_;
    bool port_mapping_enabled_ = false;
    std::map<size_t, FilePriority> file_priorities_;
    uint64_t priority_revision_ = 0;
    std::unique_ptr<ResolvedInput> input_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> cancelled_{false};
    std::thread worker_;
};

} // namespace torrent::app
