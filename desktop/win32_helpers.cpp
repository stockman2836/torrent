#include "win32_helpers.h"
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace torrent::desktop {
namespace fs = std::filesystem;

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("Invalid Unicode text");
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::wstring windowText(HWND window) {
    std::wstring value(GetWindowTextLengthW(window) + 1, L'\0');
    const int size = GetWindowTextW(window, value.data(), static_cast<int>(value.size()));
    value.resize(size);
    return value;
}

fs::path knownFolder(REFKNOWNFOLDERID id) {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &value))) throw std::runtime_error("Cannot locate user folder");
    fs::path result(value);
    CoTaskMemFree(value);
    return result;
}

std::wstring bytes(int64_t value) {
    const wchar_t* units[] = {L"Б", L"КиБ", L"МиБ", L"ГиБ", L"ТиБ"};
    double amount = static_cast<double>(value);
    int unit = 0;
    while (amount >= 1024 && unit < 4) { amount /= 1024; ++unit; }
    std::wostringstream out;
    out << std::fixed << std::setprecision(unit ? 1 : 0) << amount << L" " << units[unit];
    return out.str();
}

const wchar_t* priorityName(FilePriority value) {
    switch (value) {
    case FilePriority::High: return L"Высокий";
    case FilePriority::Low: return L"Низкий";
    case FilePriority::Skip: return L"Пропустить";
    default: return L"Обычный";
    }
}

std::string asciiField(HWND dialog, int id) {
    const auto value = windowText(GetDlgItem(dialog, id));
    if (std::any_of(value.begin(), value.end(), [](wchar_t c) { return c > 127; }))
        throw std::invalid_argument("Use decimal digits");
    return {value.begin(), value.end()};
}

} // namespace torrent::desktop
