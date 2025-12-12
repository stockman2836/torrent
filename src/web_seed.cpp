#include "web_seed.h"
#include "torrent_file.h"
#include "logger.h"
#include <curl/curl.h>
#include <sstream>
#include <thread>
#include <chrono>
#include <algorithm>

namespace torrent {

// ============================================================================
// CURL callback for writing response data
// ============================================================================

static size_t curlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    std::vector<uint8_t>* buffer = static_cast<std::vector<uint8_t>*>(userp);

    const uint8_t* data = static_cast<const uint8_t*>(contents);
    buffer->insert(buffer->end(), data, data + total_size);

    return total_size;
}

// ============================================================================
// WebSeed Implementation
// ============================================================================

WebSeed::WebSeed(const std::string& url,
                 const TorrentFile& torrent,
                 size_t piece_length,
                 size_t total_length)
    : url_(url)
    , torrent_(torrent)
    , piece_length_(piece_length)
    , total_length_(total_length)
    , running_(false)
    , failed_(false)
    , bytes_downloaded_(0)
    , active_downloads_(0)
    , successful_downloads_(0)
    , failed_downloads_(0)
    , consecutive_errors_(0) {

    // Initialize CURL globally
    static bool curl_initialized = false;
    if (!curl_initialized) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl_initialized = true;
    }
}

WebSeed::~WebSeed() {
    stop();
}

void WebSeed::start() {
    if (running_) {
        return;
    }

    running_ = true;
    LOG_INFO("WebSeed started: {}", url_);
}

void WebSeed::stop() {
    if (!running_) {
        return;
    }

    running_ = false;
    LOG_INFO("WebSeed stopped: {}", url_);
}

bool WebSeed::downloadPiece(uint32_t piece_index, uint32_t offset, uint32_t length) {
    if (!running_ || failed_) {
        return false;
    }

    // Launch async download
    std::thread(&WebSeed::downloadPieceAsync, this, piece_index, offset, length).detach();

    return true;
}

bool WebSeed::downloadFullPiece(uint32_t piece_index) {
    // Calculate piece size (last piece might be smaller)
    size_t piece_size = piece_length_;
    if (piece_index == (total_length_ / piece_length_)) {
        // Last piece
        size_t remainder = total_length_ % piece_length_;
        if (remainder > 0) {
            piece_size = remainder;
        }
    }

    return downloadPiece(piece_index, 0, static_cast<uint32_t>(piece_size));
}

void WebSeed::setCallback(WebSeedCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callback_ = callback;
}

int64_t WebSeed::getDownloadSpeed() const {
    // TODO: Implement speed tracking
    return 0;
}

// ============================================================================
// Private Methods
// ============================================================================

void WebSeed::downloadPieceAsync(uint32_t piece_index, uint32_t offset, uint32_t length) {
    active_downloads_++;

    WebSeedDownload download(piece_index, offset, length);

    try {
        // Calculate byte range
        int64_t start_byte = static_cast<int64_t>(piece_index) * piece_length_ + offset;
        int64_t end_byte = start_byte + length - 1;

        // Build URL for this piece
        std::string url;
        if (torrent_.isSingleFile()) {
            url = buildSingleFileUrl();
        } else {
            url = buildMultiFileUrl(piece_index, offset);
        }

        LOG_DEBUG("WebSeed: Downloading piece {} offset {} length {} from {}",
                  piece_index, offset, length, url);

        // Perform HTTP range request
        std::string error_msg;
        download.data = performHttpRangeRequest(url, start_byte, end_byte, error_msg);

        if (download.data.empty() || download.data.size() != length) {
            download.success = false;
            download.error_message = error_msg.empty() ? "Download failed or incomplete" : error_msg;

            LOG_WARN("WebSeed: Failed to download piece {} from {}: {}",
                     piece_index, url_, download.error_message);

            failed_downloads_++;
            consecutive_errors_++;

            // Mark as permanently failed if too many consecutive errors
            if (consecutive_errors_ >= MAX_CONSECUTIVE_ERRORS) {
                LOG_ERROR("WebSeed: Too many consecutive errors, marking {} as failed", url_);
                failed_ = true;
            }
        } else {
            download.success = true;
            bytes_downloaded_ += length;
            successful_downloads_++;
            consecutive_errors_ = 0;  // Reset error counter on success

            LOG_DEBUG("WebSeed: Successfully downloaded piece {} ({} bytes) from {}",
                      piece_index, length, url_);
        }

    } catch (const std::exception& e) {
        download.success = false;
        download.error_message = e.what();
        failed_downloads_++;
        consecutive_errors_++;

        LOG_ERROR("WebSeed: Exception during download: {}", e.what());
    }

    // Call callback
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (callback_) {
            callback_(download);
        }
    }

    active_downloads_--;
}

std::string WebSeed::buildSingleFileUrl() {
    std::string url = url_;

    // If URL ends with '/', append torrent name
    if (url.back() == '/') {
        url += torrent_.name();
    }

    return url;
}

std::string WebSeed::buildMultiFileUrl(uint32_t piece_index, uint32_t offset) {
    // For multi-file torrents, we need to find which file contains this byte offset
    int64_t byte_offset = static_cast<int64_t>(piece_index) * piece_length_ + offset;

    const auto& files = torrent_.files();
    int64_t current_offset = 0;

    for (const auto& file : files) {
        if (byte_offset >= current_offset && byte_offset < current_offset + file.length) {
            // This file contains the byte we need
            std::string url = url_;

            // Ensure URL ends with '/'
            if (url.back() != '/') {
                url += '/';
            }

            // Add torrent name
            url += torrent_.name();
            url += '/';

            // Add file path
            url += file.path;

            return url;
        }
        current_offset += file.length;
    }

    // Fallback: just use base URL + name
    return buildSingleFileUrl();
}

std::vector<uint8_t> WebSeed::performHttpRangeRequest(const std::string& url,
                                                       int64_t start_byte,
                                                       int64_t end_byte,
                                                       std::string& error_msg) {
    std::vector<uint8_t> response_data;

    CURL* curl = curl_easy_init();
    if (!curl) {
        error_msg = "Failed to initialize CURL";
        return response_data;
    }

    // Build Range header
    std::ostringstream range_header;
    range_header << "bytes=" << start_byte << "-" << end_byte;

    // Set CURL options
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_RANGE, range_header.str().c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);  // Follow redirects
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);        // 30 second timeout
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L); // 10 second connect timeout
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);     // Fail on HTTP errors

    // User agent
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "BitTorrent-WebSeed/1.0");

    // Perform request
    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        error_msg = curl_easy_strerror(res);
        response_data.clear();
    } else {
        // Check HTTP response code
        long response_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

        // HTTP 206 Partial Content is expected for range requests
        // HTTP 200 OK is also acceptable (server ignored Range header)
        if (response_code != 200 && response_code != 206) {
            std::ostringstream oss;
            oss << "HTTP error " << response_code;
            error_msg = oss.str();
            response_data.clear();
        }
    }

    curl_easy_cleanup(curl);

    return response_data;
}

// ============================================================================
// WebSeedManager Implementation
// ============================================================================

WebSeedManager::WebSeedManager(const TorrentFile& torrent,
                               size_t piece_length,
                               size_t total_length)
    : torrent_(torrent)
    , piece_length_(piece_length)
    , total_length_(total_length) {
}

WebSeedManager::~WebSeedManager() {
    stop();
}

void WebSeedManager::addWebSeed(const std::string& url) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto web_seed = std::make_unique<WebSeed>(url, torrent_, piece_length_, total_length_);
    web_seeds_.push_back(std::move(web_seed));

    LOG_INFO("WebSeedManager: Added web seed: {}", url);
}

void WebSeedManager::start() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& web_seed : web_seeds_) {
        web_seed->start();
    }

    LOG_INFO("WebSeedManager: Started {} web seeds", web_seeds_.size());
}

void WebSeedManager::stop() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& web_seed : web_seeds_) {
        web_seed->stop();
    }

    LOG_INFO("WebSeedManager: Stopped all web seeds");
}

bool WebSeedManager::downloadPiece(uint32_t piece_index, uint32_t offset, uint32_t length) {
    WebSeed* web_seed = selectBestWebSeed();

    if (!web_seed) {
        return false;
    }

    return web_seed->downloadPiece(piece_index, offset, length);
}

bool WebSeedManager::downloadFullPiece(uint32_t piece_index) {
    WebSeed* web_seed = selectBestWebSeed();

    if (!web_seed) {
        return false;
    }

    return web_seed->downloadFullPiece(piece_index);
}

void WebSeedManager::setCallback(WebSeedCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& web_seed : web_seeds_) {
        web_seed->setCallback(callback);
    }
}

size_t WebSeedManager::getActiveWebSeeds() const {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t active = 0;
    for (const auto& web_seed : web_seeds_) {
        if (web_seed->isRunning() && !web_seed->isFailed()) {
            active++;
        }
    }

    return active;
}

int64_t WebSeedManager::getTotalBytesDownloaded() const {
    std::lock_guard<std::mutex> lock(mutex_);

    int64_t total = 0;
    for (const auto& web_seed : web_seeds_) {
        total += web_seed->getBytesDownloaded();
    }

    return total;
}

int64_t WebSeedManager::getTotalDownloadSpeed() const {
    std::lock_guard<std::mutex> lock(mutex_);

    int64_t total = 0;
    for (const auto& web_seed : web_seeds_) {
        total += web_seed->getDownloadSpeed();
    }

    return total;
}

WebSeed* WebSeedManager::selectBestWebSeed() {
    std::lock_guard<std::mutex> lock(mutex_);

    WebSeed* best = nullptr;
    size_t min_active_downloads = SIZE_MAX;

    for (auto& web_seed : web_seeds_) {
        if (!web_seed->isRunning() || web_seed->isFailed()) {
            continue;
        }

        size_t active = web_seed->getActiveDownloads();
        if (active < min_active_downloads) {
            min_active_downloads = active;
            best = web_seed.get();
        }
    }

    return best;
}

} // namespace torrent
