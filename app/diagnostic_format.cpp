#include "diagnostic_format.h"
#include <ctime>
#include <iomanip>
#include <sstream>

namespace torrent::app {

std::string formatDiagnosticTime(int64_t milliseconds) {
    if (milliseconds <= 0) return u8"—";
    const auto seconds = static_cast<std::time_t>(milliseconds / 1000);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &seconds)) return u8"—";
#else
    if (!localtime_r(&seconds, &local)) return u8"—";
#endif
    std::ostringstream output;
    output << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
    return output.str();
}

} // namespace torrent::app
