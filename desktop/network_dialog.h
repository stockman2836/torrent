#pragma once

#include <windows.h>
// Win32's ERROR macro conflicts with the DHT message enum in shared headers.
#undef ERROR
#include "download_task.h"
#include <functional>
#include <string>

std::wstring networkHint(const torrent::app::DownloadSnapshot& snapshot);
void showNetworkDetails(HWND owner,
    const std::function<torrent::app::DownloadSnapshot()>& snapshot, bool automatic = false);
