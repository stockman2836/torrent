#include "seeding_dialog.h"
#include "resource.h"
#include <algorithm>
#include <cwctype>
#include <iomanip>
#include <limits>
#include <sstream>

namespace {
struct DialogState {
    torrent::app::SeedingOptions options;
    bool automatic;
};

std::wstring field(HWND dialog, int id) {
    wchar_t value[64]{};
    GetDlgItemTextW(dialog, id, value, 64);
    std::wstring result(value);
    const auto first = result.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    return result.substr(first, result.find_last_not_of(L" \t\r\n") - first + 1);
}

INT_PTR CALLBACK procedure(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<DialogState*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, lparam);
        if (state->automatic) state->options = {true, 0.25, 1};
        CheckDlgButton(dialog, IDC_SEED_ENABLED, state->options.enabled ? BST_CHECKED : BST_UNCHECKED);
        std::wostringstream ratio; ratio << std::setprecision(17) << state->options.ratio_limit;
        SetDlgItemTextW(dialog, IDC_SEED_RATIO, ratio.str().c_str());
        SetDlgItemTextW(dialog, IDC_SEED_HOURS, std::to_wstring(state->options.time_limit_hours).c_str());
        SendDlgItemMessageW(dialog, IDC_SEED_RATIO, EM_SETLIMITTEXT, 32, 0);
        SendDlgItemMessageW(dialog, IDC_SEED_HOURS, EM_SETLIMITTEXT, 10, 0);
        if (state->automatic) PostMessageW(dialog, WM_COMMAND, IDOK, 0);
        return TRUE;
    }
    if (!state) return FALSE;
    if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wparam) == IDCANCEL)) {
        EndDialog(dialog, IDCANCEL); return TRUE;
    }
    if (message == WM_COMMAND && LOWORD(wparam) == IDOK) {
        try {
            auto ratio = field(dialog, IDC_SEED_RATIO);
            std::replace(ratio.begin(), ratio.end(), L',', L'.');
            const auto hours = field(dialog, IDC_SEED_HOURS);
            size_t read = 0;
            const double limit = std::stod(ratio, &read);
            if (read != ratio.size() || ratio.empty() || ratio.front() == L'-' ||
                hours.empty() || !std::all_of(hours.begin(), hours.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
                throw std::invalid_argument("Invalid seeding limit");
            const auto duration = std::stoll(hours);
            if (duration > std::numeric_limits<int>::max()) throw std::invalid_argument("Seeding duration is too large");
            torrent::app::SeedingOptions options{IsDlgButtonChecked(dialog, IDC_SEED_ENABLED) == BST_CHECKED, limit, static_cast<int>(duration)};
            options.validate(); state->options = options;
            EndDialog(dialog, IDOK);
        } catch (const std::exception&) {
            if (state->automatic) EndDialog(dialog, IDCANCEL);
            else MessageBoxW(dialog, L"Укажите неотрицательный коэффициент и целое количество часов. 0 — без ограничения.", L"Проверьте лимиты", MB_OK | MB_ICONWARNING);
        }
        return TRUE;
    }
    return FALSE;
}
}

bool editSeedingOptions(HWND owner, torrent::app::SeedingOptions& options, bool automatic) {
    DialogState state{options, automatic};
    const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SEEDING), owner, procedure,
                                        reinterpret_cast<LPARAM>(&state));
    if (result == -1) throw std::runtime_error("Cannot open seeding settings");
    if (result != IDOK) return false;
    options = state.options;
    return true;
}
