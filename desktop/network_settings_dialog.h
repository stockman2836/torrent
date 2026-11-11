#pragma once

#include <windows.h>
#include "network_settings.h"

bool editNetworkSettings(HWND owner, torrent::app::NetworkSettings& settings, size_t downloads, bool automatic = false);
