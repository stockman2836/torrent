#include "speed_limits.h"
#include <limits>
#include <stdexcept>

namespace torrent::app {

int64_t parseSpeedLimit(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) throw std::invalid_argument("Enter a speed limit; 0 means unlimited");
    const auto last = value.find_last_not_of(" \t\r\n");
    const auto input = value.substr(first, last - first + 1);
    constexpr auto maximum = std::numeric_limits<int64_t>::max();
    int64_t whole = 0, fraction = 0, denominator = 1;
    bool decimal = false, digit = false, fraction_digit = false;
    for (char ch : input) {
        if ((ch == '.' || ch == ',') && !decimal && digit) { decimal = true; continue; }
        if (ch < '0' || ch > '9') throw std::invalid_argument("Speed must be a non-negative number in KiB/s");
        if (!decimal) {
            if (whole > (maximum / 1024 - (ch - '0')) / 10)
                throw std::invalid_argument("Speed limit is too large");
            whole = whole * 10 + (ch - '0');
            if (whole > maximum / 1024) throw std::invalid_argument("Speed limit is too large");
            digit = true;
        } else {
            if (denominator == 10000000000LL) throw std::invalid_argument("Use at most 10 decimal places");
            fraction = fraction * 10 + ch - '0'; denominator *= 10; fraction_digit = true;
        }
    }
    if (!digit || (decimal && !fraction_digit)) throw std::invalid_argument("Enter a complete speed limit");
    const auto extra = (fraction * 1024 + denominator - 1) / denominator;
    const auto base = whole * 1024;
    if (extra > maximum - base) throw std::invalid_argument("Speed limit is too large");
    return base + extra;
}

std::string formatSpeedLimit(int64_t bytes_per_second) {
    if (bytes_per_second < 0) throw std::invalid_argument("Speed limit must be non-negative");
    auto result = std::to_string(bytes_per_second / 1024);
    const auto remainder = bytes_per_second % 1024;
    if (remainder) {
        auto fraction = std::to_string(remainder * 9765625LL); // Exact base-10 representation of /1024.
        fraction.insert(0, 10 - fraction.size(), '0');
        while (fraction.back() == '0') fraction.pop_back();
        result += '.' + fraction;
    }
    return result;
}

} // namespace torrent::app
