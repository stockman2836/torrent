#pragma once

#include "torrent/core/config.h"
#include <chrono>

namespace torrent::app {

struct SeedingOptions {
    bool enabled = false;
    double ratio_limit = 2.0; // Uploaded bytes / verified torrent size during this session.
    int time_limit_hours = 0;

    static SeedingOptions fromConfig(const Config& config);
    void validate() const;
    void applyTo(Config& config) const;
    bool finished(int64_t uploaded, int64_t torrent_size, std::chrono::seconds elapsed) const;
};

} // namespace torrent::app
