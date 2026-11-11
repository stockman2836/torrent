#include "network_settings_dialog.h"
#include "resource.h"
#include <algorithm>
#include <stdexcept>
#include <string>

namespace {
struct DialogState {
    torrent::app::NetworkSettings settings;
    size_t downloads;
    bool automatic;
};

std::string field(HWND dialog, int id) {
    wchar_t value[32]{};
    const auto size = GetDlgItemTextW(dialog, id, value, 32);
    if (std::any_of(value, value + size, [](wchar_t c) { return c > 127; }))
        throw std::invalid_argument("Use decimal digits");
    return {value, value + size};
}

INT_PTR CALLBACK procedure(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<DialogState*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, lparam);
        SetDlgItemTextW(dialog, IDC_NET_FIRST_PORT, std::to_wstring(state->settings.first_port).c_str());
        SetDlgItemTextW(dialog, IDC_NET_MAX_PEERS, std::to_wstring(state->automatic ? 8 : state->settings.max_peers).c_str());
        CheckDlgButton(dialog, IDC_NET_DHT, state->settings.enable_dht ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_NET_PEX, state->settings.enable_pex ? BST_CHECKED : BST_UNCHECKED);
        SendDlgItemMessageW(dialog, IDC_NET_FIRST_PORT, EM_SETLIMITTEXT, 6, 0);
        SendDlgItemMessageW(dialog, IDC_NET_MAX_PEERS, EM_SETLIMITTEXT, 5, 0);
        if (state->automatic) PostMessageW(dialog, WM_COMMAND, IDOK, 0);
        return TRUE;
    }
    if (!state) return FALSE;
    if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wparam) == IDCANCEL)) {
        EndDialog(dialog, IDCANCEL); return TRUE;
    }
    if (message == WM_COMMAND && LOWORD(wparam) == IDOK) {
        try {
            const auto settings = torrent::app::NetworkSettings::parse(field(dialog, IDC_NET_FIRST_PORT),
                field(dialog, IDC_NET_MAX_PEERS), IsDlgButtonChecked(dialog, IDC_NET_DHT) == BST_CHECKED,
                IsDlgButtonChecked(dialog, IDC_NET_PEX) == BST_CHECKED);
            settings.validate(state->downloads);
            state->settings = settings;
            EndDialog(dialog, IDOK);
        } catch (const std::exception&) {
            if (state->automatic) EndDialog(dialog, IDCANCEL);
            else MessageBoxW(dialog,
                L"Начальный порт: целое число от 1024 до 65534.\nЛимит пиров на раздачу: от 1 до 1000.\nДля списка из нескольких раздач должно хватать свободных портов выше начального.",
                L"Проверьте настройки сети", MB_OK | MB_ICONWARNING);
        }
        return TRUE;
    }
    return FALSE;
}
}

bool editNetworkSettings(HWND owner, torrent::app::NetworkSettings& settings, size_t downloads, bool automatic) {
    DialogState state{settings, downloads, automatic};
    const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_NETWORK_SETTINGS), owner,
        procedure, reinterpret_cast<LPARAM>(&state));
    if (result == -1) throw std::runtime_error("Cannot open network settings");
    if (result != IDOK) return false;
    settings = state.settings;
    return true;
}
