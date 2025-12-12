#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <mutex>
#include <atomic>
#include <functional>
#include <memory>

namespace torrent {

// Forward declarations
class TorrentFile;
class PieceManager;

// BEP 19: HTTP/FTP Seeding (GetRight-style)
// Allows downloading pieces from HTTP/FTP servers in addition to peers

// Web seed download result
struct WebSeedDownload {
    uint32_t piece_index;
    uint32_t offset;
    uint32_t length;
    std::vector<uint8_t> data;
    bool success;
    std::string error_message;

    WebSeedDownload(uint32_t pi, uint32_t off, uint32_t len)
        : piece_index(pi), offset(off), length(len), success(false) {}
};

// Callback for when a piece is downloaded from web seed
using WebSeedCallback = std::function<void(const WebSeedDownload&)>;

class WebSeed {
public:
    WebSeed(const std::string& url,
            const TorrentFile& torrent,
            size_t piece_length,
            size_t total_length);
    ~WebSeed();

    // Start/Stop
    void start();
    void stop();
    bool isRunning() const { return running_; }

    // Download a specific piece range
    // Returns true if download initiated successfully
    bool downloadPiece(uint32_t piece_index, uint32_t offset, uint32_t length);

    // Download a full piece
    bool downloadFullPiece(uint32_t piece_index);

    // Set callback for completed downloads
    void setCallback(WebSeedCallback callback);

    // Get URL
    const std::string& getUrl() const { return url_; }

    // Mark URL as failed (too many errors)
    void markAsFailed() { failed_ = true; }
    bool isFailed() const { return failed_; }

    // Statistics
    int64_t getBytesDownloaded() const { return bytes_downloaded_; }
    int64_t getDownloadSpeed() const;
    size_t getActiveDownloads() const { return active_downloads_; }
    size_t getSuccessfulDownloads() const { return successful_downloads_; }
    size_t getFailedDownloads() const { return failed_downloads_; }

private:
    // Build full URL for piece download
    std::string buildPieceUrl(uint32_t piece_index, uint32_t offset, uint32_t length);

    // Build URL for single-file torrent
    std::string buildSingleFileUrl();

    // Build URL for multi-file torrent (returns file path for specific byte offset)
    std::string buildMultiFileUrl(uint32_t piece_index, uint32_t offset);

    // Perform HTTP range request
    // Returns downloaded data or empty vector on failure
    std::vector<uint8_t> performHttpRangeRequest(const std::string& url,
                                                  int64_t start_byte,
                                                  int64_t end_byte,
                                                  std::string& error_msg);

    // Download piece in background thread
    void downloadPieceAsync(uint32_t piece_index, uint32_t offset, uint32_t length);

    std::string url_;                      // Base URL
    const TorrentFile& torrent_;           // Torrent metadata
    size_t piece_length_;
    size_t total_length_;

    std::atomic<bool> running_;
    std::atomic<bool> failed_;             // URL permanently failed

    // Statistics
    std::atomic<int64_t> bytes_downloaded_;
    std::atomic<size_t> active_downloads_;
    std::atomic<size_t> successful_downloads_;
    std::atomic<size_t> failed_downloads_;

    // Callback
    WebSeedCallback callback_;
    std::mutex callback_mutex_;

    // Error tracking
    std::atomic<size_t> consecutive_errors_;
    static constexpr size_t MAX_CONSECUTIVE_ERRORS = 5;
};

// Web Seed Manager
// Manages multiple web seeds for a torrent
class WebSeedManager {
public:
    WebSeedManager(const TorrentFile& torrent,
                   size_t piece_length,
                   size_t total_length);
    ~WebSeedManager();

    // Add web seed URL
    void addWebSeed(const std::string& url);

    // Start/Stop all web seeds
    void start();
    void stop();

    // Download piece from best available web seed
    // Returns true if download initiated
    bool downloadPiece(uint32_t piece_index, uint32_t offset, uint32_t length);
    bool downloadFullPiece(uint32_t piece_index);

    // Set callback for all web seeds
    void setCallback(WebSeedCallback callback);

    // Get statistics
    size_t getActiveWebSeeds() const;
    size_t getTotalWebSeeds() const { return web_seeds_.size(); }
    int64_t getTotalBytesDownloaded() const;
    int64_t getTotalDownloadSpeed() const;

    // Check if any web seeds are available
    bool hasWebSeeds() const { return !web_seeds_.empty(); }

private:
    // Select best web seed for download (least busy, not failed)
    WebSeed* selectBestWebSeed();

    const TorrentFile& torrent_;
    size_t piece_length_;
    size_t total_length_;

    std::vector<std::unique_ptr<WebSeed>> web_seeds_;
    std::mutex mutex_;
};

} // namespace torrent
