#pragma once

#include "win32.h"
#include "download_task.h"
#include <functional>
#include <string>

std::wstring networkHint(const torrent::app::DownloadSnapshot& snapshot);
void showNetworkDetails(HWND owner,
    const std::function<torrent::app::DownloadSnapshot()>& snapshot, const std::function<bool()>& update_trackers,
    bool automatic = false, bool automatic_update = false);
