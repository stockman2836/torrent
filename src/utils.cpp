#include "utils.h"
#include <openssl/sha.h>
#include <random>
#include <chrono>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace torrent {
namespace utils {

// SHA1 hash
std::vector<uint8_t> sha1(const std::string& data) {
    return sha1(std::vector<uint8_t>(data.begin(), data.end()));
}

std::vector<uint8_t> sha1(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> hash(SHA_DIGEST_LENGTH);
    SHA1(data.data(), data.size(), hash.data());
    return hash;
}

// URL encoding
std::string urlEncode(const std::string& str) {
    return urlEncode(std::vector<uint8_t>(str.begin(), str.end()));
}

std::string urlEncode(const std::vector<uint8_t>& data) {
    std::ostringstream encoded;
    encoded.fill('0');
    encoded << std::hex;

    for (uint8_t byte : data) {
        // Keep alphanumeric and some safe characters
        if (std::isalnum(byte) || byte == '-' || byte == '_' || byte == '.' || byte == '~') {
            encoded << static_cast<char>(byte);
        } else {
            encoded << '%' << std::setw(2) << static_cast<int>(byte);
        }
    }

    return encoded.str();
}

// Hex conversion
std::string toHex(const std::vector<uint8_t>& data) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t byte : data) {
        oss << std::setw(2) << static_cast<int>(byte);
    }
    return oss.str();
}

std::vector<uint8_t> fromHex(const std::string& hex) {
    std::vector<uint8_t> data;
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byte_str = hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoi(byte_str, nullptr, 16));
        data.push_back(byte);
    }
    return data;
}

// Generate random peer ID
std::string generatePeerId() {
    static const char* chars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 61);

    std::string peer_id = "-TC0100-";  // Client ID: TC (TorrentClient) version 01.00
    for (int i = 0; i < 12; ++i) {
        peer_id += chars[dis(gen)];
    }
    return peer_id;
}

// Network byte order (these might be redundant on some platforms)
#ifdef _WIN32
uint32_t htonl(uint32_t hostlong) {
    return ::htonl(hostlong);
}

uint16_t htons(uint16_t hostshort) {
    return ::htons(hostshort);
}

uint32_t ntohl(uint32_t netlong) {
    return ::ntohl(netlong);
}

uint16_t ntohs(uint16_t netshort) {
    return ::ntohs(netshort);
}
#else
uint32_t htonl(uint32_t hostlong) {
    return ::htonl(hostlong);
}

uint16_t htons(uint16_t hostshort) {
    return ::htons(hostshort);
}

uint32_t ntohl(uint32_t netlong) {
    return ::ntohl(netlong);
}

uint16_t ntohs(uint16_t netshort) {
    return ::ntohs(netshort);
}
#endif

// Formatting helpers
std::string formatBytes(int64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_index = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024.0 && unit_index < 4) {
        size /= 1024.0;
        unit_index++;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << size << " " << units[unit_index];
    return oss.str();
}

std::string formatSpeed(double bytesPerSec) {
    return formatBytes(static_cast<int64_t>(bytesPerSec)) + "/s";
}

// Exponential backoff calculation
int calculateBackoffDelay(int attempt, int base_delay_ms, int max_delay_ms) {
    // Exponential backoff: base_delay * 2^attempt
    // Example: 1s, 2s, 4s, 8s, 16s, 32s, 60s (capped)
    if (attempt < 0) attempt = 0;

    int delay = base_delay_ms;
    for (int i = 0; i < attempt; ++i) {
        delay *= 2;
        if (delay >= max_delay_ms) {
            return max_delay_ms;
        }
    }

    return delay;
}

// TokenBucket implementation
TokenBucket::TokenBucket(int64_t rate_bytes_per_sec)
    : rate_(rate_bytes_per_sec)
    , tokens_(rate_bytes_per_sec)
    , capacity_(rate_bytes_per_sec * 2)  // Allow burst up to 2 seconds worth
    , last_update_(std::chrono::steady_clock::now()) {
    if (rate_ <= 0) {
        rate_ = 0;  // 0 means unlimited
        capacity_ = 0;
    }
}

void TokenBucket::refill() {
    if (rate_ <= 0) return;  // Unlimited

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_update_).count() / 1000.0;

    // Add tokens based on elapsed time
    tokens_ += elapsed * rate_;

    // Cap at capacity
    if (tokens_ > capacity_) {
        tokens_ = capacity_;
    }

    last_update_ = now;
}

bool TokenBucket::tryConsume(size_t bytes) {
    if (rate_ <= 0) return true;  // Unlimited

    std::lock_guard<std::mutex> lock(mutex_);
    refill();

    if (tokens_ >= static_cast<double>(bytes)) {
        tokens_ -= bytes;
        return true;
    }

    return false;
}

void TokenBucket::waitAndConsume(size_t bytes) {
    if (rate_ <= 0) return;  // Unlimited

    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            refill();

            if (tokens_ >= static_cast<double>(bytes)) {
                tokens_ -= bytes;
                return;
            }
        }

        // Sleep for a short time before retry
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void TokenBucket::setRate(int64_t rate_bytes_per_sec) {
    std::lock_guard<std::mutex> lock(mutex_);
    rate_ = rate_bytes_per_sec;

    if (rate_ <= 0) {
        rate_ = 0;  // 0 means unlimited
        capacity_ = 0;
        tokens_ = 0;
    } else {
        capacity_ = rate_ * 2;
        tokens_ = std::min(tokens_, static_cast<double>(capacity_));
    }

    last_update_ = std::chrono::steady_clock::now();
}

// SpeedTracker implementation
SpeedTracker::SpeedTracker(int window_seconds)
    : window_seconds_(window_seconds) {
}

void SpeedTracker::addBytes(int64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);

    Sample sample;
    sample.timestamp = std::chrono::steady_clock::now();
    sample.bytes = bytes;

    samples_.push_back(sample);
    removeOldSamples();
}

void SpeedTracker::removeOldSamples() {
    auto now = std::chrono::steady_clock::now();
    auto cutoff = now - std::chrono::seconds(window_seconds_);

    while (!samples_.empty() && samples_.front().timestamp < cutoff) {
        samples_.pop_front();
    }
}

double SpeedTracker::getSpeed() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (samples_.empty()) {
        return 0.0;
    }

    // Remove old samples
    auto now = std::chrono::steady_clock::now();
    auto cutoff = now - std::chrono::seconds(window_seconds_);

    int64_t total_bytes = 0;
    auto oldest_timestamp = now;

    for (const auto& sample : samples_) {
        if (sample.timestamp >= cutoff) {
            total_bytes += sample.bytes;
            if (sample.timestamp < oldest_timestamp) {
                oldest_timestamp = sample.timestamp;
            }
        }
    }

    if (total_bytes == 0) {
        return 0.0;
    }

    // Calculate elapsed time
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - oldest_timestamp).count() / 1000.0;

    if (elapsed <= 0.0) {
        return 0.0;
    }

    return total_bytes / elapsed;
}

void SpeedTracker::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    samples_.clear();
}

} // namespace utils
} // namespace torrent
