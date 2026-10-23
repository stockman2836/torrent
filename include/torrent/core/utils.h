#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <mutex>
#include <deque>

namespace torrent {
namespace utils {

// SHA1 hash
std::vector<uint8_t> sha1(const std::string& data);
std::vector<uint8_t> sha1(const std::vector<uint8_t>& data);

// URL encoding
std::string urlEncode(const std::string& str);
std::string urlEncode(const std::vector<uint8_t>& data);

// Hex conversion
std::string toHex(const std::vector<uint8_t>& data);
std::vector<uint8_t> fromHex(const std::string& hex);

// Random generation
std::string generatePeerId();

// Network byte order conversion
uint32_t htonl(uint32_t hostlong);
uint16_t htons(uint16_t hostshort);
uint32_t ntohl(uint32_t netlong);
uint16_t ntohs(uint16_t netshort);

// Formatting
std::string formatBytes(int64_t bytes);
std::string formatSpeed(double bytesPerSec);

// IPv6 support (BEP 7)
enum class IPVersion {
    IPv4,
    IPv6,
    Unknown
};

IPVersion detectIPVersion(const std::string& ip);
bool isValidIPv4(const std::string& ip);
bool isValidIPv6(const std::string& ip);

// Convert IPv6 string to compact binary format (16 bytes)
std::vector<uint8_t> ipv6ToCompact(const std::string& ipv6);

// Convert compact binary format to IPv6 string
std::string compactToIPv6(const uint8_t* data);

// Normalize IPv6 address (expand :: notation)
std::string normalizeIPv6(const std::string& ipv6);

// Retry helpers
int calculateBackoffDelay(int attempt, int base_delay_ms = 1000, int max_delay_ms = 60000);

// Rate limiting with token bucket algorithm
class TokenBucket {
public:
    // rate_bytes_per_sec: maximum bytes per second (0 = unlimited)
    TokenBucket(int64_t rate_bytes_per_sec);

    // Try to consume tokens for sending/receiving bytes
    // Returns true if allowed, false if rate limit exceeded
    bool tryConsume(size_t bytes);

    // Wait until tokens are available (blocks)
    void waitAndConsume(size_t bytes);

    // Get current rate limit
    int64_t getRate() const { return rate_; }

    // Update rate limit (0 = unlimited)
    void setRate(int64_t rate_bytes_per_sec);

private:
    int64_t rate_;        // Bytes per second (0 = unlimited)
    double tokens_;       // Available tokens
    int64_t capacity_;    // Maximum tokens
    std::chrono::steady_clock::time_point last_update_;
    std::mutex mutex_;

    void refill();
};

// Speed tracking with sliding window
class SpeedTracker {
public:
    SpeedTracker(int window_seconds = 20);

    // Record bytes transferred
    void addBytes(int64_t bytes);

    // Get current speed (bytes per second)
    double getSpeed() const;

    // Reset tracking
    void reset();

private:
    struct Sample {
        std::chrono::steady_clock::time_point timestamp;
        int64_t bytes;
    };

    int window_seconds_;
    std::deque<Sample> samples_;
    mutable std::mutex mutex_;

    void removeOldSamples();
};

} // namespace utils
} // namespace torrent
