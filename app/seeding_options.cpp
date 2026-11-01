#include "seeding_options.h"
#include <cmath>
#include <stdexcept>

namespace torrent::app {

SeedingOptions SeedingOptions::fromConfig(const Config& config) {
    return {config.seed_after_download, config.seed_ratio_limit, config.seed_time_limit_hours};
}

void SeedingOptions::validate() const {
    if (!std::isfinite(ratio_limit) || ratio_limit < 0 || time_limit_hours < 0)
        throw std::invalid_argument("Seeding limits must be finite non-negative numbers");
}

void SeedingOptions::applyTo(Config& config) const {
    validate();
    config.seed_after_download = enabled;
    config.seed_ratio_limit = ratio_limit;
    config.seed_time_limit_hours = time_limit_hours;
}

bool SeedingOptions::finished(int64_t uploaded, int64_t torrent_size, std::chrono::seconds elapsed) const {
    if (!enabled || torrent_size <= 0) return true;
    if (ratio_limit > 0 && static_cast<double>(uploaded) / torrent_size >= ratio_limit) return true;
    return time_limit_hours > 0 && elapsed.count() >= int64_t(time_limit_hours) * 3600;
}

} // namespace torrent::app
