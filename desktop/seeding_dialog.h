#pragma once

#include "win32.h"
#include "seeding_options.h"

bool editSeedingOptions(HWND owner, torrent::app::SeedingOptions& options, bool automatic = false);
