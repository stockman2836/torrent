#include "download_task.h"
#include "torrent/session/download_manager.h"
#include "torrent/core/utils.h"
#include <chrono>
#include <optional>

namespace torrent::app {

namespace {
NetworkStatus initialNetworkStatus(const TorrentFile& torrent, const Config& config) {
    NetworkStatus result;
    result.dht_enabled = config.enable_dht;
    result.listener.port = config.listen_port;
    result.max_peers = config.max_peers;
    result.download_limit = config.max_download_speed;
    result.upload_limit = config.max_upload_speed;
    result.pex_enabled = config.enable_pex;
    result.lsd_enabled = config.enable_lsd;
    result.web_seeds = config.enable_webseeds ? torrent.webSeeds().size() : 0;
    if (!torrent.trackerTiers().empty()) result.tracker.url = torrent.trackerTiers().front().front();
    for (const auto& tier : torrent.trackerTiers()) result.tracker.total_trackers += tier.size();
    result.tracker.state = result.tracker.url.empty() ? TrackerState::None : TrackerState::Waiting;
    return result;
}
}

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
    snapshot_.network.peers.clear();
    snapshot_.network.queued_peers = 0;
    snapshot_.priorities_pending = false;
}

bool DownloadTask::load(const std::string& source, Config config) {
    if (!begin(DownloadState::Loading)) return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.files.reset();
        snapshot_.name.clear();
        snapshot_.info_hash.clear();
        snapshot_.progress = 0;
        snapshot_.can_seed = false;
        snapshot_.uploaded_bytes = snapshot_.seed_seconds = 0;
        snapshot_.seed_ratio = 0;
        snapshot_.network = {};
        snapshot_.listen_port = 0;
        snapshot_.priorities_pending = false;
        file_priorities_.clear(); priority_revision_ = 0;
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
                snapshot_.network = initialNetworkStatus(input->torrent, config);
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

bool DownloadTask::start(Config config, std::shared_ptr<utils::BandwidthLimits> global_limits) {
    const auto seeding = SeedingOptions::fromConfig(config);
    seeding.validate();
    if (config.max_download_speed < 0 || config.max_upload_speed < 0)
        throw std::invalid_argument("Speed limits must be non-negative");
    if (busy_ || !input_ || !begin(DownloadState::Downloading)) return false;
    setSpeedLimits(config.max_download_speed, config.max_upload_speed);
    setSeedingOptions(seeding);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.uploaded_bytes = snapshot_.seed_seconds = 0;
        snapshot_.seed_ratio = 0;
        snapshot_.listen_port = 0;
        snapshot_.network = initialNetworkStatus(input_->torrent, config);
        file_priorities_ = config.file_priorities; priority_revision_ = 0;
        snapshot_.priorities_pending = false;
    }
    worker_ = std::thread([this, config, global_limits = std::move(global_limits)] {
        try {
            // A magnet can have loaded metadata with the previous network settings.
            // Release its old socket on this worker before binding the new session.
            if (input_->dht && (!config.enable_dht ||
                (config.dht_port != 0 && input_->dht->port() != config.dht_port))) input_->dht.reset();
            DownloadManager manager(input_->torrent, config, std::move(input_->dht), global_limits);
            auto download_limit = config.max_download_speed, upload_limit = config.max_upload_speed;
            std::optional<std::chrono::steady_clock::time_point> seed_started;
            uint64_t applied_revision = 0;
            const auto apply_priorities = [&] {
                std::map<size_t, FilePriority> priorities;
                uint64_t revision;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (priority_revision_ == applied_revision) return;
                    priorities = file_priorities_; revision = priority_revision_;
                }
                manager.setFilePriorities(priorities);
                applied_revision = revision;
                std::lock_guard<std::mutex> lock(mutex_);
                snapshot_.priorities_pending = priority_revision_ != applied_revision;
            };
            try { if (!cancelled_) manager.start(); }
            catch (...) {
                auto network = manager.networkStatus();
                std::lock_guard<std::mutex> lock(mutex_);
                snapshot_.network = std::move(network);
                throw;
            }
            while (!cancelled_ && manager.isRunning()) {
                apply_priorities();
                const double progress = manager.progress();
                auto network = manager.networkStatus();
                const bool can_seed = progress >= 100 && manager.canSeed();
                const auto now = std::chrono::steady_clock::now();
                if (can_seed && !seed_started) {
                    seed_started = now;
                    manager.beginSeeding();
                }
                const auto elapsed = seed_started ? std::chrono::duration_cast<std::chrono::seconds>(now - *seed_started)
                                                 : std::chrono::seconds(0);
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    if (download_limit != download_limit_ || upload_limit != upload_limit_) {
                        download_limit = download_limit_; upload_limit = upload_limit_;
                        manager.setSpeedLimits(download_limit, upload_limit);
                    }
                    snapshot_.progress = progress;
                    snapshot_.network = std::move(network);
                    snapshot_.download_speed = can_seed ? 0 : manager.downloadSpeed();
                    snapshot_.upload_speed = manager.uploadSpeed();
                    snapshot_.uploaded_bytes = manager.uploadedBytes();
                    snapshot_.listen_port = manager.listenPort();
                    snapshot_.can_seed = can_seed;
                    snapshot_.seed_seconds = elapsed.count();
                    snapshot_.seed_ratio = input_->torrent.totalLength() > 0
                        ? static_cast<double>(snapshot_.uploaded_bytes) / input_->torrent.totalLength() : 0;
                    if (progress >= 100 && (!can_seed || seeding_.finished(snapshot_.uploaded_bytes, input_->torrent.totalLength(), elapsed))) {
                        if (priority_revision_ != applied_revision) continue;
                        snapshot_.state = DownloadState::Stopping;
                        break;
                    }
                    if (!cancelled_) snapshot_.state = can_seed ? DownloadState::Seeding : DownloadState::Downloading;
                    wake_.wait_for(lock, std::chrono::milliseconds(150));
                }
            }
            if (manager.isRunning()) apply_priorities();
            const double progress = manager.progress();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                snapshot_.state = DownloadState::Stopping;
                snapshot_.progress = progress;
            }
            manager.stop();
            auto network = manager.networkStatus();
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.network = std::move(network);
            snapshot_.priorities_pending = false;
            snapshot_.state = progress >= 100 ? DownloadState::Complete : DownloadState::Stopped;
            snapshot_.uploaded_bytes = manager.uploadedBytes();
            snapshot_.seed_ratio = input_->torrent.totalLength() > 0
                ? static_cast<double>(snapshot_.uploaded_bytes) / input_->torrent.totalLength() : 0;
            snapshot_.download_speed = snapshot_.upload_speed = 0;
        } catch (const std::exception& error) { fail(error); }
        busy_ = false;
    });
    return true;
}

void DownloadTask::setSeedingOptions(const SeedingOptions& options) {
    options.validate();
    { std::lock_guard<std::mutex> lock(mutex_); seeding_ = options; }
    wake_.notify_all();
}

bool DownloadTask::setFilePriorities(const std::map<size_t, FilePriority>& priorities) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!busy_ || cancelled_ || !snapshot_.files ||
        (snapshot_.state != DownloadState::Downloading && snapshot_.state != DownloadState::Seeding)) return false;
    for (const auto& [index, priority] : priorities) {
        if (index >= snapshot_.files->size() || priority > FilePriority::High)
            throw std::invalid_argument("Invalid live file priority");
    }
    file_priorities_ = priorities;
    ++priority_revision_;
    snapshot_.priorities_pending = true;
    wake_.notify_all();
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
