#include "download_queue.h"
#include "torrent/metainfo/magnet_uri.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <limits>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace torrent::app {
namespace {
std::string absolute(const std::string& path) {
    return fs::weakly_canonical(fs::absolute(fs::u8path(path))).u8string();
}
std::string pathKey(const std::string& path) {
    auto key = absolute(path);
#ifdef _WIN32
    const auto native = fs::u8path(key).wstring();
    const int size = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, native.data(),
        static_cast<int>(native.size()), nullptr, 0, nullptr, nullptr, 0);
    if (!size) throw std::runtime_error("Cannot normalize output path");
    std::wstring normalized(size, L'\0');
    LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, native.data(), static_cast<int>(native.size()),
        normalized.data(), size, nullptr, nullptr, 0);
    key = fs::path(normalized).u8string();
#endif
    return key;
}
FilePriority parsePriority(const std::string& value) {
    if (value == "skip") return FilePriority::Skip;
    if (value == "low") return FilePriority::Low;
    if (value == "normal") return FilePriority::Normal;
    if (value == "high") return FilePriority::High;
    throw std::invalid_argument("Invalid saved file priority");
}
size_t savedIndex(const json& value) {
    if (!value.is_number_unsigned() && (!value.is_number_integer() || value.get<int64_t>() < 0))
        throw std::invalid_argument("Invalid saved file index");
    return value.get<size_t>();
}
std::set<std::string> selectedPaths(const Config& config, const DownloadSnapshot& state) {
    std::set<std::string> result;
    if (!state.files) return result;
    for (size_t i = 0; i < state.files->size(); ++i) {
        if (!config.file_indices.empty() && std::find(config.file_indices.begin(), config.file_indices.end(), i) == config.file_indices.end()) continue;
        const auto priority = config.file_priorities.find(i);
        if (priority != config.file_priorities.end() && priority->second == FilePriority::Skip) continue;
        result.insert(pathKey((fs::u8path(config.download_dir) / fs::u8path((*state.files)[i].path)).u8string()));
    }
    return result;
}
int64_t savedSpeed(const json& item, const char* key, int64_t fallback) {
    if (!item.contains(key)) return fallback;
    if (!item.at(key).is_number_integer()) throw std::invalid_argument("Invalid saved speed limit");
    const auto value = item.at(key).get<int64_t>();
    if (value < 0) throw std::invalid_argument("Invalid saved speed limit");
    return value;
}
}

DownloadQueue::~DownloadQueue() {
    close();
    for (auto& entry : entries_) entry->task.wait();
}

DownloadQueue::Entry* DownloadQueue::find(DownloadId id) {
    for (auto& entry : entries_) if (entry->id == id) return entry.get();
    return nullptr;
}

DownloadId DownloadQueue::add(std::string source, Config config) {
    SeedingOptions::fromConfig(config).validate();
    if (closing_) throw std::logic_error("The download list is closing");
    if (source.empty()) throw std::invalid_argument("Empty torrent source");
    if (!MagnetURI::isMagnetURI(source)) source = absolute(source);
    for (const auto& entry : entries_) if (!entry->removing && entry->source == source) return entry->id;
    config.download_dir = absolute(config.download_dir);
    if (network_settings_) network_settings_->applyTo(config);
    // Each live task may own a DHT socket even while waiting for user input.
    const auto port = availablePort(config.listen_port);
    if (!port) throw std::invalid_argument("No free port pair for another download");
    config.listen_port = port;
    config.dht_port = static_cast<uint16_t>(port + 1);
    auto entry = std::make_unique<Entry>();
    entry->id = next_id_++;
    entry->source = std::move(source);
    entry->config = std::move(config);
    entry->task.load(entry->source, entry->config);
    const auto id = entry->id;
    entries_.push_back(std::move(entry));
    return id;
}

uint16_t DownloadQueue::availablePort(uint16_t first, const Entry* except) const {
    for (uint32_t port = first; port >= 1024 && port < 65535; port += 2) {
        const auto overlaps = [port](uint16_t used) { return used && port <= uint32_t(used) + 1 && port + 1 >= used; };
        const bool used = std::any_of(entries_.begin(), entries_.end(), [&](const auto& entry) {
            return entry.get() != except && (overlaps(entry->config.listen_port) || overlaps(entry->retiring_port));
        });
        if (!used) return static_cast<uint16_t>(port);
    }
    return 0;
}

bool DownloadQueue::setNetworkSettings(const NetworkSettings& settings) {
    settings.validate(entries_.size());
    if (closing_) return false;
    network_settings_ = settings;
    for (auto& entry : entries_) {
        entry->config.enable_port_mapping = settings.enable_port_mapping;
        entry->task.setPortMappingEnabled(settings.enable_port_mapping);
    }
    return true;
}

bool DownloadQueue::prepareNetwork(Entry& entry) {
    if (!network_settings_) return true;
    const auto port = availablePort(static_cast<uint16_t>(network_settings_->first_port), &entry);
    if (!port) return false; // Another startup may still be releasing its old DHT socket.
    auto config = entry.config;
    network_settings_->applyTo(config);
    config.listen_port = port;
    config.dht_port = static_cast<uint16_t>(port + 1);
    if (port != entry.config.listen_port) entry.retiring_port = entry.config.listen_port;
    entry.config = std::move(config);
    return true;
}

bool DownloadQueue::configure(DownloadId id, const std::string& directory,
                              const std::map<size_t, FilePriority>& priorities) {
    auto entry = find(id);
    if (closing_ || !entry || entry->removing || entry->queued || entry->task.busy()) return false;
    if (directory.empty()) throw std::invalid_argument("Select a download folder");
    entry->config.download_dir = absolute(directory);
    entry->config.file_indices.clear();
    entry->config.file_priorities = priorities;
    return true;
}

bool DownloadQueue::enqueue(DownloadId id) {
    auto entry = find(id);
    if (entry && entry->transferring && !entry->task.busy()) entry->transferring = entry->downloading = false;
    if (closing_ || !entry || entry->removing || entry->queued || entry->transferring) return false;
    if (!entry->task.busy() && !entry->task.snapshot().files) {
        if (!prepareNetwork(*entry)) return false;
        entry->task.load(entry->source, entry->config);
    }
    entry->queued = true;
    pending_.push_back(id);
    return true;
}

bool DownloadQueue::setSpeedLimits(DownloadId id, int64_t download_bytes_per_sec, int64_t upload_bytes_per_sec) {
    if (download_bytes_per_sec < 0 || upload_bytes_per_sec < 0)
        throw std::invalid_argument("Speed limits must be non-negative");
    auto entry = find(id);
    if (closing_ || !entry || entry->removing) return false;
    entry->task.setSpeedLimits(download_bytes_per_sec, upload_bytes_per_sec);
    entry->config.max_download_speed = download_bytes_per_sec;
    entry->config.max_upload_speed = upload_bytes_per_sec;
    return true;
}

bool DownloadQueue::setSeedingOptions(DownloadId id, const SeedingOptions& options) {
    options.validate();
    auto entry = find(id);
    if (closing_ || !entry || entry->removing) return false;
    entry->task.setSeedingOptions(options);
    options.applyTo(entry->config);
    return true;
}

bool DownloadQueue::setGlobalSpeedLimits(int64_t download_bytes_per_sec, int64_t upload_bytes_per_sec) {
    if (download_bytes_per_sec < 0 || upload_bytes_per_sec < 0)
        throw std::invalid_argument("Speed limits must be non-negative");
    if (closing_) return false;
    global_limits_->download.setRate(download_bytes_per_sec);
    global_limits_->upload.setRate(upload_bytes_per_sec);
    return true;
}

bool DownloadQueue::setFilePriorities(DownloadId id, const std::map<size_t, FilePriority>& priorities) {
    auto entry = find(id);
    if (closing_ || !entry || entry->removing) return false;
    const auto state = entry->task.snapshot();
    if (!state.files) return false;
    for (const auto& [index, priority] : priorities) {
        if (index >= state.files->size() || priority > FilePriority::High)
            throw std::invalid_argument("Invalid file priority");
    }
    auto config = entry->config;
    config.file_indices.clear(); config.file_priorities = priorities;
    auto paths = selectedPaths(config, state);
    if (entry->task.busy()) {
        if (state.state != DownloadState::Downloading && state.state != DownloadState::Seeding) return false;
        for (const auto& other : entries_) if (other.get() != entry && other->transferring) {
            if (std::any_of(paths.begin(), paths.end(), [&](const auto& path) { return other->owned_paths.count(path); }))
                throw std::invalid_argument("A selected file is used by another active torrent");
        }
        if (!entry->task.setFilePriorities(priorities)) return false;
        entry->owned_paths.insert(paths.begin(), paths.end());
    }
    entry->config = std::move(config);
    return true;
}

void DownloadQueue::dequeue(DownloadId id) {
    pending_.erase(std::remove(pending_.begin(), pending_.end(), id), pending_.end());
    if (auto entry = find(id)) entry->queued = false;
}

void DownloadQueue::stop(DownloadId id) {
    dequeue(id);
    if (auto entry = find(id)) entry->task.stop();
}

void DownloadQueue::remove(DownloadId id) {
    if (auto entry = find(id)) { entry->removing = true; stop(id); }
}

void DownloadQueue::setLimit(size_t limit) {
    if (limit == 0 || limit > 8) throw std::invalid_argument("The active download limit must be between 1 and 8");
    limit_ = limit;
}

std::vector<QueueSnapshot> DownloadQueue::snapshots() const {
    std::vector<QueueSnapshot> result;
    for (const auto& entry : entries_)
        result.push_back({entry->id, entry->source, entry->config, entry->task.snapshot(),
                          entry->task.busy(), entry->queued, entry->removing});
    return result;
}

bool DownloadQueue::conflicts(const Entry& candidate, const Entry& active) const {
    const auto first = candidate.task.snapshot();
    const auto second = active.task.snapshot();
    if (!first.files || !second.files) return false;
    const auto paths = selectedPaths(candidate.config, first);
    for (const auto& path : paths) if (active.owned_paths.count(path)) return true;
    // Resume and boundary-piece cache files also belong to one writer at a time.
    return first.info_hash == second.info_hash && pathKey(candidate.config.download_dir) == pathKey(active.config.download_dir);
}

void DownloadQueue::tick() {
    for (auto& entry : entries_) {
        if (entry->transferring && !entry->task.busy()) {
            entry->transferring = entry->downloading = false;
            entry->owned_paths.clear();
        }
        if (entry->retiring_port && (!entry->task.busy() ||
            (entry->transferring && entry->task.snapshot().listen_port == entry->config.listen_port)))
            entry->retiring_port = 0;
        if (entry->task.snapshot().state == DownloadState::Seeding) entry->downloading = false;
        if ((closing_ || entry->removing) && !entry->unloading && !entry->task.busy()) {
            entry->task.unload(); entry->unloading = true;
        }
    }
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [](const auto& entry) {
        return entry->removing && entry->unloading && !entry->task.busy();
    }), entries_.end());
    if (closing_) return;
    auto active = static_cast<size_t>(std::count_if(entries_.begin(), entries_.end(), [](const auto& entry) {
        return entry->downloading;
    }));
    for (auto it = pending_.begin(); it != pending_.end();) {
        auto entry = find(*it);
        if (!entry || entry->removing) { it = pending_.erase(it); continue; }
        if (entry->task.busy()) { ++it; continue; }
        const auto state = entry->task.snapshot();
        if (!state.files) {
            entry->queued = false; it = pending_.erase(it); continue;
        }
        if (active >= limit_) break;
        const bool collision = std::any_of(entries_.begin(), entries_.end(), [&](const auto& other) {
            return other->transferring && conflicts(*entry, *other);
        });
        if (collision) { ++it; continue; }
        if (!prepareNetwork(*entry)) { ++it; continue; }
        entry->owned_paths = selectedPaths(entry->config, state);
        if (entry->task.start(entry->config, global_limits_)) {
            entry->transferring = entry->downloading = true; entry->queued = false; ++active;
            it = pending_.erase(it);
        } else ++it;
    }
}

void DownloadQueue::close() {
    if (closing_) return;
    closing_ = true; pending_.clear();
    for (auto& entry : entries_) { entry->queued = false; entry->task.stop(); }
}

bool DownloadQueue::closed() const {
    return closing_ && std::all_of(entries_.begin(), entries_.end(), [](const auto& entry) {
        return entry->unloading && !entry->task.busy();
    });
}

void DownloadQueue::save(const std::string& path) const {
    json document = {{"version", 1}, {"max_active", limit_}, {"downloads", json::array()}};
    for (const auto& entry : entries_) if (!entry->removing) {
        json priorities = json::array();
        for (auto [index, priority] : entry->config.file_priorities)
            priorities.push_back({{"index", index}, {"priority", filePriorityName(priority)}});
        document["downloads"].push_back({{"source", entry->source}, {"directory", entry->config.download_dir},
            {"files", entry->config.file_indices}, {"priorities", priorities},
            {"max_download_speed", entry->config.max_download_speed}, {"max_upload_speed", entry->config.max_upload_speed},
            {"seed_after_download", entry->config.seed_after_download}, {"seed_ratio_limit", entry->config.seed_ratio_limit},
            {"seed_time_limit_hours", entry->config.seed_time_limit_hours}});
    }
    const auto destination = fs::u8path(path);
    auto temporary = destination; temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << document.dump(2) << '\n'; output.close();
        if (!output) throw std::runtime_error("Cannot save the download list");
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot replace the saved download list");
#else
    fs::rename(temporary, destination);
#endif
}

void DownloadQueue::restore(const std::string& path, const Config& defaults) {
    if (!entries_.empty() || closing_) throw std::logic_error("Restore requires an empty download list");
    std::ifstream input(fs::u8path(path), std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read the saved download list");
    const auto document = json::parse(input);
    if (document.at("version").get<int>() != 1) throw std::invalid_argument("Unsupported download list version");
    const auto limit = document.at("max_active").get<size_t>();
    if (limit == 0 || limit > 8) throw std::invalid_argument("Invalid saved download limit");
    const auto& downloads = document.at("downloads");
    if (!downloads.is_array()) throw std::invalid_argument("Invalid saved download list");
    std::vector<std::pair<std::string, Config>> restored;
    for (const auto& item : downloads) {
        auto source = item.at("source").get<std::string>();
        Config config = defaults;
        config.download_dir = item.at("directory").get<std::string>();
        config.max_download_speed = savedSpeed(item, "max_download_speed", defaults.max_download_speed);
        config.max_upload_speed = savedSpeed(item, "max_upload_speed", defaults.max_upload_speed);
        const auto seed_hours = savedSpeed(item, "seed_time_limit_hours", 0);
        if (seed_hours > std::numeric_limits<int>::max()) throw std::invalid_argument("Invalid saved seeding time limit");
        SeedingOptions seeding{item.value("seed_after_download", false), item.value("seed_ratio_limit", 2.0),
                               static_cast<int>(seed_hours)};
        seeding.applyTo(config);
        if (config.max_download_speed < 0 || config.max_upload_speed < 0)
            throw std::invalid_argument("Invalid saved speed limits");
        config.file_indices.clear();
        if (!item.at("files").is_array() || !item.at("priorities").is_array())
            throw std::invalid_argument("Invalid saved file choices");
        for (const auto& index : item.at("files")) config.file_indices.push_back(savedIndex(index));
        config.file_priorities.clear();
        if (source.empty() || config.download_dir.empty()) throw std::invalid_argument("Empty saved path");
        for (const auto& priority : item.at("priorities"))
            config.file_priorities[savedIndex(priority.at("index"))] = parsePriority(priority.at("priority").get<std::string>());
        if (!MagnetURI::isMagnetURI(source)) source = absolute(source);
        config.download_dir = absolute(config.download_dir);
        restored.emplace_back(std::move(source), std::move(config));
    }
    network_settings_.value_or(NetworkSettings::fromConfig(defaults)).validate(restored.size());
    setLimit(limit);
    for (auto& item : restored) add(std::move(item.first), std::move(item.second));
}

} // namespace torrent::app
