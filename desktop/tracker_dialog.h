#pragma once

#include "network_dialog.h"

void showTrackerDetails(HWND owner, const std::function<torrent::app::DownloadSnapshot()>& snapshot,
    const std::function<bool()>& update, bool automatic = false, bool automatic_update = false);
