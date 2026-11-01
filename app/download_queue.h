#pragma once

#include "download_task.h"
#include <deque>

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
        bool queued = false;
        bool removing = false;
        bool unloading = false;
    };
    Entry* find(DownloadId id);
    void dequeue(DownloadId id);
    bool conflicts(const Entry& candidate, const Entry& active) const;
    std::vector<std::unique_ptr<Entry>> entries_;
    std::deque<DownloadId> pending_;
    DownloadId next_id_ = 1;
    size_t limit_ = 1;
    bool closing_ = false;
};

} // namespace torrent::app
