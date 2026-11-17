#pragma once

#include <cstdint>
#include <string>

namespace torrent::app {
std::string formatDiagnosticTime(int64_t unix_milliseconds);
}
