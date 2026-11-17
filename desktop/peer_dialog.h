#pragma once

#include "network_dialog.h"

enum class PeerDialogTest { None, Render, Statistics };

void showPeerDetails(HWND owner, const std::function<torrent::app::DownloadSnapshot()>& snapshot,
                     PeerDialogTest test = PeerDialogTest::None);
