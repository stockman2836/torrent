#include "network_dialog.h"
#include "resource.h"
#include "network_diagnostics.h"
#include <commctrl.h>
#include <uxtheme.h>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <cstring>

using namespace torrent;
using namespace torrent::app;

namespace {
std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    const auto size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}
std::wstring bytes(int64_t count) {
    const wchar_t* units[] = {L"Б", L"КиБ", L"МиБ", L"ГиБ"};
    double value = static_cast<double>(count); int unit = 0;
    while (value >= 1024 && unit < 3) { value /= 1024; ++unit; }
    std::wostringstream out; out << std::fixed << std::setprecision(unit ? 1 : 0) << value << L' ' << units[unit];
    return out.str();
}
const wchar_t* trackerState(TrackerState state) {
    switch (state) {
    case TrackerState::Waiting: return L"Ещё не запрашивался";
    case TrackerState::Announcing: return L"Ожидание ответа";
    case TrackerState::Working: return L"Ответ получен";
    case TrackerState::Error: return L"Ошибка";
    default: return L"Трекер не указан";
    }
}
struct DialogState {
    const std::function<DownloadSnapshot()>& snapshot;
    bool automatic;
    std::string error;
};

void copyReport(HWND dialog, const DownloadSnapshot& snapshot) {
    std::wstring value;
    for (const auto character : wide(networkReport(snapshot))) {
        if (character == L'\n') value += L'\r';
        value += character;
    }
    const auto size = (value.size() + 1) * sizeof(wchar_t);
    const auto memory = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!memory) throw std::runtime_error(u8"Не удалось подготовить отчёт");
    auto data = GlobalLock(memory);
    if (!data) { GlobalFree(memory); throw std::runtime_error(u8"Не удалось подготовить отчёт"); }
    std::memcpy(data, value.c_str(), size);
    GlobalUnlock(memory);
    if (!OpenClipboard(dialog)) {
        GlobalFree(memory); throw std::runtime_error(u8"Буфер обмена занят. Попробуйте ещё раз.");
    }
    const bool copied = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
    if (!copied) GlobalFree(memory);
    CloseClipboard();
    if (!copied) throw std::runtime_error(u8"Не удалось скопировать отчёт");
}

void refresh(HWND dialog, const DialogState& state) {
    const auto snapshot = state.snapshot();
    const auto& network = snapshot.network;
    const auto diagnosis = diagnoseNetwork(snapshot);
    SetWindowTextW(dialog, (L"Пиры и трекер · " + wide(snapshot.name)).c_str());
    SetDlgItemTextW(dialog, IDC_NET_HINT, networkHint(snapshot).c_str());
    SetDlgItemTextW(dialog, IDC_NET_PORT, wide(diagnosis.port).c_str());
    SetDlgItemTextW(dialog, IDC_NET_INCOMING, wide(diagnosis.incoming).c_str());
    auto advice = diagnosis.mapping + "\r\n" + diagnosis.advice;
    if (network.download_limit || network.upload_limit || network.global_download_limit || network.global_upload_limit)
        advice += "\r\n" + diagnosis.limits;
    SetDlgItemTextW(dialog, IDC_NET_ADVICE, wide(advice).c_str());
    const auto connected = std::count_if(network.peers.begin(), network.peers.end(),
        [](const auto& peer) { return peer.state == PeerState::Connected; });
    const auto summary = L"Подключено: " + std::to_wstring(connected) + L" · Подключаются: " +
        std::to_wstring(network.peers.size() - connected) + L" · Адресов в очереди: " + std::to_wstring(network.queued_peers) +
        L" · DHT: " + (network.dht_enabled ? std::to_wstring(network.dht_nodes) + L" узлов" : L"выключена");
    SetDlgItemTextW(dialog, IDC_NET_SUMMARY, summary.c_str());
    SetDlgItemTextW(dialog, IDC_NET_TRACKER_URL, wide(network.tracker.url).c_str());
    auto tracker = std::wstring(trackerState(network.tracker.state));
    if (network.tracker.state == TrackerState::Announcing && network.tracker.total_trackers > 1)
        tracker += L" · попытка " + std::to_wstring(network.tracker.attempt) + L" из " + std::to_wstring(network.tracker.total_trackers);
    if (network.tracker.state == TrackerState::Working)
        tracker += L" · адресов в последнем ответе: " + std::to_wstring(network.tracker.peers_returned);
    if (network.tracker.consecutive_failures)
        tracker += L" · ошибок подряд: " + std::to_wstring(network.tracker.consecutive_failures);
    SetDlgItemTextW(dialog, IDC_NET_TRACKER_STATE, tracker.c_str());
    SetDlgItemTextW(dialog, IDC_NET_TRACKER_ERROR, wide(network.tracker.error).c_str());
    SetDlgItemTextW(dialog, IDC_NET_PEER_ERROR,
        (L"Неудачных подключений за сеанс: " + std::to_wstring(network.failed_connections) +
         (network.last_peer_error.empty() ? L"" : L" · " + wide(network.last_peer_error))).c_str());

    const auto list = GetDlgItem(dialog, IDC_NET_PEERS);
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    if (ListView_GetItemCount(list) != static_cast<int>(network.peers.size())) ListView_DeleteAllItems(list);
    for (size_t i = 0; i < network.peers.size(); ++i) {
        const auto& peer = network.peers[i];
        const auto address = (peer.ip.find(':') == std::string::npos ? peer.ip : "[" + peer.ip + "]") + ":" + std::to_string(peer.port);
        std::wstring status = peer.state == PeerState::Connecting ? L"TCP-подключение" :
            peer.state == PeerState::Handshaking ? L"Обмен приветствиями" : snapshot.state == DownloadState::Seeding ? L"Подключён" :
            peer.choked ? L"Ожидание разрешения скачивать" : L"Скачивание разрешено";
        std::wstring columns[] = {wide(address), peer.incoming ? L"Входящее" : L"Исходящее", status,
            bytes(peer.downloaded_bytes), bytes(peer.uploaded_bytes), std::to_wstring(peer.pending_requests)};
        if (static_cast<int>(i) >= ListView_GetItemCount(list)) {
            LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = static_cast<int>(i); item.pszText = columns[0].data();
            ListView_InsertItem(list, &item);
        }
        for (int column = 0; column < 6; ++column) ListView_SetItemText(list, i, column, columns[column].data());
    }
    SendMessageW(list, WM_SETREDRAW, TRUE, 0); InvalidateRect(list, nullptr, TRUE);
}

INT_PTR CALLBACK procedure(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
    try {
        if (message == WM_INITDIALOG) {
            state = reinterpret_cast<DialogState*>(lparam);
            SetWindowLongPtrW(dialog, DWLP_USER, lparam);
            const auto list = GetDlgItem(dialog, IDC_NET_PEERS);
            ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            SetWindowTheme(list, L"Explorer", nullptr);
            const wchar_t* columns[] = {L"Адрес", L"Направление", L"Состояние", L"Получено", L"Отдано", L"Запросов"};
            const int widths[] = {190, 110, 250, 100, 100, 85};
            for (int i = 0; i < 6; ++i) {
                LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH;
                column.pszText = const_cast<PWSTR>(columns[i]); column.cx = widths[i];
                ListView_InsertColumn(list, i, &column);
            }
            refresh(dialog, *state);
            if (state->automatic && (GetWindowTextLengthW(GetDlgItem(dialog, IDC_NET_PORT)) == 0 ||
                GetWindowTextLengthW(GetDlgItem(dialog, IDC_NET_ADVICE)) == 0 || networkReport(state->snapshot()).empty()))
                throw std::runtime_error("Network diagnosis was not displayed");
            if (state->automatic) PostMessageW(dialog, WM_COMMAND, IDOK, 0);
            else SetTimer(dialog, 1, 500, nullptr);
            return TRUE;
        }
        if (!state) return FALSE;
        if (message == WM_TIMER) { refresh(dialog, *state); return TRUE; }
        if (message == WM_COMMAND && LOWORD(wparam) == IDC_NET_COPY) {
            try {
                copyReport(dialog, state->snapshot());
                SetDlgItemTextW(dialog, IDC_NET_COPY, L"Отчёт скопирован");
            } catch (const std::exception& error) {
                MessageBoxW(dialog, wide(error.what()).c_str(), L"Копирование отчёта", MB_OK | MB_ICONWARNING);
            }
            return TRUE;
        }
        if (message == WM_CLOSE || (message == WM_COMMAND && (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL))) {
            KillTimer(dialog, 1); EndDialog(dialog, IDOK); return TRUE;
        }
    } catch (const std::exception& error) {
        if (state) state->error = error.what();
        KillTimer(dialog, 1); EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}
}

std::wstring networkHint(const DownloadSnapshot& snapshot) {
    return wide(diagnoseNetwork(snapshot).summary);
}

void showNetworkDetails(HWND owner, const std::function<DownloadSnapshot()>& snapshot, bool automatic) {
    DialogState state{snapshot, automatic, {}};
    const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_NETWORK), owner,
        procedure, reinterpret_cast<LPARAM>(&state));
    if (result == -1) throw std::runtime_error("Cannot open network details");
    if (!state.error.empty()) throw std::runtime_error(state.error);
}
