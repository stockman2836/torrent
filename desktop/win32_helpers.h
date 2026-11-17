#pragma once

#include "win32.h"
#include "torrent/core/file_priority.h"
#include <shlobj.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <utility>

namespace torrent::desktop {

std::string utf8(const std::wstring& value);
std::wstring wide(const std::string& value);
std::wstring windowText(HWND window);
std::filesystem::path knownFolder(REFKNOWNFOLDERID id);
std::wstring bytes(int64_t value);
const wchar_t* priorityName(FilePriority value);
std::string asciiField(HWND dialog, int id);
void configureList(HWND list, const std::vector<std::pair<const wchar_t*, int>>& columns);
void setListRow(HWND list, int index, const std::vector<std::wstring>& values);

} // namespace torrent::desktop
