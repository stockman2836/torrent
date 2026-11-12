#pragma once

#include "download_task.h"
#include <string>

namespace torrent::app {

struct NetworkDiagnostics {
    std::string summary;
    std::string port;
    std::string incoming;
    std::string advice;
    std::string limits;
};

NetworkDiagnostics diagnoseNetwork(const DownloadSnapshot& snapshot);
std::string networkReport(const DownloadSnapshot& snapshot);

} // namespace torrent::app
