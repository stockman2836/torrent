#pragma once

#include <cstdint>

namespace torrent {

enum class FilePriority : uint8_t { Skip, Low, Normal, High };

inline const char* filePriorityName(FilePriority priority) {
    switch (priority) {
        case FilePriority::Skip: return "skip";
        case FilePriority::Low: return "low";
        case FilePriority::Normal: return "normal";
        case FilePriority::High: return "high";
    }
    return "unknown";
}

} // namespace torrent
