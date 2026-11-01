#include "download_task.h"
#include "torrent/session/download_manager.h"
#include "torrent/core/utils.h"
#include <chrono>

namespace torrent::app {

DownloadTask::~DownloadTask() { stop(); wait(); }

void DownloadTask::wait() {
    if (worker_.joinable()) worker_.join();
}

DownloadSnapshot DownloadTask::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

bool DownloadTask::begin(DownloadState state) {
    if (busy_.exchange(true)) return false;
    wait();
    cancelled_ = false;
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.state = state;
    snapshot_.error.clear();
    snapshot_.download_speed = snapshot_.upload_speed = 0;
    return true;
}

void DownloadTask::fail(const std::exception& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.state = cancelled_ ? DownloadState::Stopped : DownloadState::Failed;
    if (!cancelled_) snapshot_.error = error.what();
    snapshot_.download_speed = snapshot_.upload_speed = 0;
}

bool DownloadTask::load(const std::string& source, Config config) {
    if (!begin(DownloadState::Loading)) return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.files.reset();
        snapshot_.name.clear();
        snapshot_.info_hash.clear();
        snapshot_.progress = 0;
    }
    worker_ = std::thread([this, source, config] {
        try {
            input_.reset();
            auto input = std::make_unique<ResolvedInput>(resolveInput(source, config, [this] { return cancelled_.load(); }));
            if (!cancelled_) {
                auto files = std::make_shared<const std::vector<FileInfo>>(input->torrent.files());
                std::lock_guard<std::mutex> lock(mutex_);
                snapshot_.name = input->torrent.name();
                snapshot_.info_hash = utils::toHex(input->torrent.infoHash());
                snapshot_.files = std::move(files);
                input_ = std::move(input);
                snapshot_.state = cancelled_ ? DownloadState::Stopped : DownloadState::Ready;
            } else {
                std::lock_guard<std::mutex> lock(mutex_);
                snapshot_.state = DownloadState::Stopped;
            }
        } catch (const std::exception& error) { fail(error); }
        busy_ = false;
    });
    return true;
}

bool DownloadTask::start(Config config) {
    if (config.max_download_speed < 0 || config.max_upload_speed < 0)
        throw std::invalid_argument("Speed limits must be non-negative");
    if (busy_ || !input_ || !begin(DownloadState::Downloading)) return false;
    setSpeedLimits(config.max_download_speed, config.max_upload_speed);
    worker_ = std::thread([this, config] {
        try {
            DownloadManager manager(input_->torrent, config, std::move(input_->dht));
            auto download_limit = config.max_download_speed, upload_limit = config.max_upload_speed;
            if (!cancelled_) manager.start();
            while (!cancelled_ && manager.isRunning()) {
                const double progress = manager.progress();
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    if (download_limit != download_limit_ || upload_limit != upload_limit_) {
                        download_limit = download_limit_; upload_limit = upload_limit_;
                        manager.setSpeedLimits(download_limit, upload_limit);
                    }
                    snapshot_.progress = progress;
                    snapshot_.download_speed = manager.downloadSpeed();
                    snapshot_.upload_speed = manager.uploadSpeed();
                    if (progress >= 100) break;
                    wake_.wait_for(lock, std::chrono::milliseconds(150), [this, download_limit, upload_limit] {
                        return cancelled_.load() || download_limit_ != download_limit || upload_limit_ != upload_limit;
                    });
                }
            }
            const double progress = manager.progress();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                snapshot_.state = DownloadState::Stopping;
                snapshot_.progress = progress;
            }
            manager.stop();
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.state = progress >= 100 ? DownloadState::Complete : DownloadState::Stopped;
            snapshot_.download_speed = snapshot_.upload_speed = 0;
        } catch (const std::exception& error) { fail(error); }
        busy_ = false;
    });
    return true;
}

void DownloadTask::setSpeedLimits(int64_t download_bytes_per_sec, int64_t upload_bytes_per_sec) {
    if (download_bytes_per_sec < 0 || upload_bytes_per_sec < 0)
        throw std::invalid_argument("Speed limits must be non-negative");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        download_limit_ = download_bytes_per_sec;
        upload_limit_ = upload_bytes_per_sec;
    }
    wake_.notify_all();
}

void DownloadTask::stop() {
    if (!busy_) return;
    cancelled_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.state = DownloadState::Stopping;
    }
    wake_.notify_all();
}

bool DownloadTask::unload() {
    if (!begin(DownloadState::Stopping)) return false;
    worker_ = std::thread([this] {
        input_.reset();
        { std::lock_guard<std::mutex> lock(mutex_); snapshot_ = {}; }
        busy_ = false;
    });
    return true;
}

} // namespace torrent::app
