#pragma once

#include "torrent/core/config.h"
#include <string_view>

namespace torrent::app {

struct NetworkSettings {
    int first_port = 6881;
    int max_peers = 50;
    bool enable_dht = true;
    bool enable_pex = true;

    static NetworkSettings fromConfig(const Config& config);
    static NetworkSettings parse(std::string_view port, std::string_view peers, bool dht, bool pex);
    void validate(size_t downloads = 1) const;
    void applyTo(Config& config) const;
};

} // namespace torrent::app
