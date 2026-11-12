#include "network_dialog.h"
#include "resource.h"
#include <commctrl.h>
#include <uxtheme.h>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>

using namespace torrent;
using namespace torrent::app;

std::wstring networkHint(const DownloadSnapshot& snapshot) {
    switch (snapshot.state) {
    case DownloadState::Loading: return L"Получение метаданных…";
    case DownloadState::Stopping: return L"Закрытие сетевых подключений…";
    case DownloadState::Stopped: return L"Сеть остановлена · доступен последний ответ трекера";
    case DownloadState::Complete: return L"Загрузка завершена · сеть остановлена";
    case DownloadState::Failed: return L"Загрузка завершилась с ошибкой";
    case DownloadState::Downloading: case DownloadState::Seeding: break;
    default: return L"Сеть будет запущена после начала загрузки";
    }
    const auto& network = snapshot.network;
    const auto connected = std::count_if(network.peers.begin(), network.peers.end(),
        [](const auto& peer) { return peer.state == PeerState::Connected; });
    if (snapshot.state == DownloadState::Seeding)
        return L"Раздача · подключений: " + std::to_wstring(connected);
    if (snapshot.download_speed > 0 && connected) return L"Данные поступают · пиров: " + std::to_wstring(connected);
    if (connected) {
        if (std::any_of(network.peers.begin(), network.peers.end(), [](const auto& peer) { return peer.pending_requests > 0; }))
            return L"Ожидание запрошенных блоков · пиров: " + std::to_wstring(connected);
        if (std::all_of(network.peers.begin(), network.peers.end(), [](const auto& peer) {
            return peer.state != PeerState::Connected || peer.choked;
        })) return L"Пиры пока не разрешают скачивание";
        return L"Подключение установлено · ожидание данных";
    }
    if (!network.peers.empty()) return L"Установка подключения к пирам…";
    if (network.tracker.state == TrackerState::Announcing) return L"Запрос пиров у трекера…";
    if (network.tracker.state == TrackerState::Error)
        return network.dht_enabled ? L"Ошибка трекера · поиск пиров через DHT"
                                   : L"Ошибка трекера · подробности в окне «Пиры и трекер»";
    if (network.dht_enabled && network.dht_nodes == 0) return L"Поиск узлов DHT…";
    return L"Доступные пиры пока не найдены";
}

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

void refresh(HWND dialog, const DialogState& state) {
    const auto snapshot = state.snapshot();
    const auto& network = snapshot.network;
    SetWindowTextW(dialog, (L"Пиры и трекер · " + wide(snapshot.name)).c_str());
    SetDlgItemTextW(dialog, IDC_NET_HINT, networkHint(snapshot).c_str());
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
            if (state->automatic) PostMessageW(dialog, WM_COMMAND, IDOK, 0);
            else SetTimer(dialog, 1, 500, nullptr);
            return TRUE;
        }
        if (!state) return FALSE;
        if (message == WM_TIMER) { refresh(dialog, *state); return TRUE; }
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

void showNetworkDetails(HWND owner, const std::function<DownloadSnapshot()>& snapshot, bool automatic) {
    DialogState state{snapshot, automatic, {}};
    const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_NETWORK), owner,
        procedure, reinterpret_cast<LPARAM>(&state));
    if (result == -1) throw std::runtime_error("Cannot open network details");
    if (!state.error.empty()) throw std::runtime_error(state.error);
}
