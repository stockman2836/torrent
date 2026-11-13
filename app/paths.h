#pragma once

#include <string>

namespace torrent::app {

// Canonical UTF-8 paths. Ownership keys also ignore case on Windows.
std::string absolutePath(const std::string& path);
std::string pathKey(const std::string& path);

} // namespace torrent::app
