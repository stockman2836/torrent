#pragma once

#include "win32.h"
#include "network_settings.h"

bool editNetworkSettings(HWND owner, torrent::app::NetworkSettings& settings, size_t downloads, bool automatic = false, bool automatic_mapping = false);
