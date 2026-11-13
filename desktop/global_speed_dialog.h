#pragma once

#include "win32.h"
#include <cstdint>

bool editGlobalSpeedLimits(HWND owner, int64_t& download, int64_t& upload, bool automatic = false);
