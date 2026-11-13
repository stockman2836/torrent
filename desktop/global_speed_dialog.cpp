#include "global_speed_dialog.h"
#include "resource.h"
#include "speed_limits.h"
#include "win32_helpers.h"
#include <stdexcept>
#include <string>

namespace {
struct DialogState {
    int64_t download;
    int64_t upload;
    bool automatic;
};

INT_PTR CALLBACK procedure(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<DialogState*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, lparam);
        const auto download = torrent::app::formatSpeedLimit(state->download);
        const auto upload = torrent::app::formatSpeedLimit(state->upload);
        SetDlgItemTextA(dialog, IDC_GLOBAL_DOWNLOAD, download.c_str());
        SetDlgItemTextA(dialog, IDC_GLOBAL_UPLOAD, upload.c_str());
        SendDlgItemMessageW(dialog, IDC_GLOBAL_DOWNLOAD, EM_SETLIMITTEXT, 32, 0);
        SendDlgItemMessageW(dialog, IDC_GLOBAL_UPLOAD, EM_SETLIMITTEXT, 32, 0);
        if (state->automatic) {
            SetDlgItemTextW(dialog, IDC_GLOBAL_DOWNLOAD, L"64");
            SetDlgItemTextW(dialog, IDC_GLOBAL_UPLOAD, L"32");
            PostMessageW(dialog, WM_COMMAND, IDOK, 0);
        }
        return TRUE;
    }
    if (!state) return FALSE;
    if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wparam) == IDCANCEL)) {
        EndDialog(dialog, IDCANCEL); return TRUE;
    }
    if (message == WM_COMMAND && LOWORD(wparam) == IDOK) {
        try {
            const auto download = torrent::app::parseSpeedLimit(torrent::desktop::asciiField(dialog, IDC_GLOBAL_DOWNLOAD));
            const auto upload = torrent::app::parseSpeedLimit(torrent::desktop::asciiField(dialog, IDC_GLOBAL_UPLOAD));
            state->download = download;
            state->upload = upload;
            EndDialog(dialog, IDOK);
        } catch (const std::exception&) {
            if (state->automatic) EndDialog(dialog, IDCANCEL);
            else MessageBoxW(dialog, L"Введите неотрицательную скорость в КиБ/с. Можно использовать дробные значения.\n0 — без ограничения.",
                L"Проверьте лимиты скорости", MB_OK | MB_ICONWARNING);
        }
        return TRUE;
    }
    return FALSE;
}
}

bool editGlobalSpeedLimits(HWND owner, int64_t& download, int64_t& upload, bool automatic) {
    DialogState state{download, upload, automatic};
    const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_GLOBAL_SPEED), owner,
        procedure, reinterpret_cast<LPARAM>(&state));
    if (result == -1) throw std::runtime_error("Cannot open global speed settings");
    if (result != IDOK) return false;
    download = state.download;
    upload = state.upload;
    return true;
}
