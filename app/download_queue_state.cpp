#include "download_queue.h"
#include "paths.h"
#include "torrent/metainfo/magnet_uri.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace torrent::app {
namespace fs = std::filesystem;
using json = nlohmann::json;
namespace {
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

int64_t savedSpeed(const json& item, const char* key, int64_t fallback) {
    if (!item.contains(key)) return fallback;
    if (!item.at(key).is_number_integer()) throw std::invalid_argument("Invalid saved speed limit");
    const auto value = item.at(key).get<int64_t>();
    if (value < 0) throw std::invalid_argument("Invalid saved speed limit");
    return value;
}
} // namespace

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
        if (!MagnetURI::isMagnetURI(source)) source = absolutePath(source);
        config.download_dir = absolutePath(config.download_dir);
        restored.emplace_back(std::move(source), std::move(config));
    }
    network_settings_.value_or(NetworkSettings::fromConfig(defaults)).validate(restored.size());
    setLimit(limit);
    for (auto& item : restored) add(std::move(item.first), std::move(item.second));
}

} // namespace torrent::app
