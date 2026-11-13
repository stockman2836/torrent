#pragma once

#include "download_task.h"
#include "network_settings.h"
#include <deque>
#include <optional>
#include <set>

namespace torrent::app {

using DownloadId = uint64_t;

struct QueueSnapshot {
    DownloadId id = 0;
    std::string source;
    Config config;
    DownloadSnapshot download;
    bool busy = false;
    bool queued = false;
    bool removing = false;
};

// All methods run on the owning UI thread; each task owns its engine worker.
class DownloadQueue {
public:
    ~DownloadQueue();
    DownloadId add(std::string source, Config config);
    bool configure(DownloadId id, const std::string& directory,
                   const std::map<size_t, FilePriority>& priorities);
    bool enqueue(DownloadId id);
    bool setSpeedLimits(DownloadId id, int64_t download_bytes_per_sec, int64_t upload_bytes_per_sec);
    bool setGlobalSpeedLimits(int64_t download_bytes_per_sec, int64_t upload_bytes_per_sec);
    bool setSeedingOptions(DownloadId id, const SeedingOptions& options);
    bool setNetworkSettings(const NetworkSettings& settings);
    bool requestTrackerUpdate(DownloadId id);
    bool setFilePriorities(DownloadId id, const std::map<size_t, FilePriority>& priorities);
    void stop(DownloadId id);
    void remove(DownloadId id); // Removes the list entry, never downloaded data.
    void setLimit(size_t limit);
    size_t limit() const { return limit_; }
    std::vector<QueueSnapshot> snapshots() const;
    void tick();
    void close();
    bool closed() const;
    void save(const std::string& path) const;
    void restore(const std::string& path, const Config& defaults);

private:
    struct Entry {
        DownloadId id;
        std::string source;
        Config config;
        DownloadTask task;
        bool transferring = false;
        bool downloading = false;
        bool queued = false;
        bool removing = false;
        bool unloading = false;
        uint16_t retiring_port = 0; // Metadata DHT may still own the previous pair during startup.
        std::set<std::string> owned_paths; // Retained until all session writers have stopped.
    };
    Entry* find(DownloadId id);
    void dequeue(DownloadId id);
    uint16_t availablePort(uint16_t first, const Entry* except = nullptr) const;
    bool prepareNetwork(Entry& entry);
    bool conflicts(const Entry& candidate, const Entry& active) const;
    std::vector<std::unique_ptr<Entry>> entries_;
    std::deque<DownloadId> pending_;
    DownloadId next_id_ = 1;
    size_t limit_ = 1;
    bool closing_ = false;
    std::optional<NetworkSettings> network_settings_;
    std::shared_ptr<utils::BandwidthLimits> global_limits_ = std::make_shared<utils::BandwidthLimits>();
};

} // namespace torrent::app
