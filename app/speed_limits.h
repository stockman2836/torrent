#pragma once

#include <cstdint>
#include <string>

namespace torrent::app {

// UI values are in KiB/s; stored configuration and engine limits are in bytes/s.
// Accept a dot or comma decimal separator and round fractions of a byte upwards.
int64_t parseSpeedLimit(const std::string& value);
std::string formatSpeedLimit(int64_t bytes_per_second);

} // namespace torrent::app
