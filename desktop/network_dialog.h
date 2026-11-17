#pragma once

#include "win32.h"
#include "download_task.h"
#include <functional>
#include <string>

std::wstring networkHint(const torrent::app::DownloadSnapshot& snapshot);
struct NetworkDialogTest {
    bool automatic = false;
    bool update_trackers = false;
    bool peer_statistics = false;
};
void showNetworkDetails(HWND owner,
    const std::function<torrent::app::DownloadSnapshot()>& snapshot, const std::function<bool()>& update_trackers,
    NetworkDialogTest test = {});
