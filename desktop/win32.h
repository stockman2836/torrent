#pragma once

// Keep Winsock before windows.h and prevent ERROR from leaking into engine enums.
#include <winsock2.h>
#include <windows.h>
#undef ERROR
