#include "paths.h"
#include <filesystem>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace torrent::app {
namespace fs = std::filesystem;

std::string absolutePath(const std::string& path) {
    return fs::weakly_canonical(fs::absolute(fs::u8path(path))).u8string();
}

std::string pathKey(const std::string& path) {
    auto key = absolutePath(path);
#ifdef _WIN32
    const auto native = fs::u8path(key).wstring();
    const int size = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, native.data(),
        static_cast<int>(native.size()), nullptr, 0, nullptr, nullptr, 0);
    if (!size) throw std::runtime_error("Cannot normalize output path");
    std::wstring normalized(size, L'\0');
    LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, native.data(), static_cast<int>(native.size()),
        normalized.data(), size, nullptr, nullptr, 0);
    key = fs::path(normalized).u8string();
#endif
    return key;
}

} // namespace torrent::app
