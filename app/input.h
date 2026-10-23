#pragma once

#include "torrent/core/config.h"
#include "torrent/metainfo/torrent_file.h"
#include "torrent/network/dht/dht_manager.h"
#include <memory>
#include <string>

namespace torrent::app {

struct ResolvedInput {
    TorrentFile torrent;
    std::unique_ptr<dht::DHTManager> dht;
};

ResolvedInput resolveInput(const std::string& input, const Config& config);

} // namespace torrent::app
