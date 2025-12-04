#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <sstream>
#include <iomanip>

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

} // namespace utils
} // namespace torrent
