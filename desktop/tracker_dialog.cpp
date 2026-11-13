#include "tracker_dialog.h"
#include "tracker_diagnostics.h"
#include "win32_helpers.h"
#include "resource.h"
#include <commctrl.h>
#include <uxtheme.h>
#include <algorithm>
#include <stdexcept>

using namespace torrent;
using namespace torrent::app;
using torrent::desktop::wide;

namespace {
struct DialogState {
    const std::function<DownloadSnapshot()>& snapshot;
    const std::function<bool()>& update;
    bool automatic;
    bool automatic_update;
    bool updating = false;
    bool sent = false;
    size_t requests_before = 0;
    int selected = 0;
    ULONGLONG deadline = 0;
    std::string error;
};

void columns(HWND list, const std::vector<std::pair<const wchar_t*, int>>& definitions) {
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SetWindowTheme(list, L"Explorer", nullptr);
    for (size_t i = 0; i < definitions.size(); ++i) {
        LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<PWSTR>(definitions[i].first); column.cx = definitions[i].second;
        ListView_InsertColumn(list, i, &column);
    }
}

void row(HWND list, int index, const std::vector<std::wstring>& values) {
    if (index >= ListView_GetItemCount(list)) {
        LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = index; item.pszText = const_cast<PWSTR>(values.front().c_str());
        ListView_InsertItem(list, &item);
    }
    for (size_t column = 0; column < values.size(); ++column)
        ListView_SetItemText(list, index, column, const_cast<PWSTR>(values[column].c_str()));
}

void refresh(HWND dialog, DialogState& state) {
    const auto snapshot = state.snapshot();
    const auto& network = snapshot.network;
    state.updating = true;
    const auto list = GetDlgItem(dialog, IDC_TRACKERS);
    if (ListView_GetItemCount(list) != static_cast<int>(network.trackers.size())) ListView_DeleteAllItems(list);
    for (size_t i = 0; i < network.trackers.size(); ++i) {
        const auto& tracker = network.trackers[i];
        row(list, static_cast<int>(i), {std::to_wstring(tracker.tier), wide(tracker.url), wide(trackerStateText(tracker.state)),
            std::to_wstring(tracker.peers_returned), wide(trackerTime(tracker.last_response_ms)),
            std::to_wstring(tracker.requests), std::to_wstring(tracker.consecutive_failures)});
    }
    if (state.selected >= static_cast<int>(network.trackers.size())) state.selected = 0;
    const auto history = GetDlgItem(dialog, IDC_TRACKER_HISTORY);
    const TrackerDetails* selected = network.trackers.empty() ? nullptr : &network.trackers[state.selected];
    const auto count = selected ? selected->history.size() : 0;
    if (ListView_GetItemCount(history) != static_cast<int>(count)) ListView_DeleteAllItems(history);
    if (selected) {
        ListView_SetItemState(list, state.selected, LVIS_SELECTED, LVIS_SELECTED);
        for (size_t i = 0; i < count; ++i) {
            const auto& event = selected->history[count - i - 1];
            row(history, static_cast<int>(i), {wide(trackerTime(event.time_ms)), wide(trackerEventText(event)),
                std::to_wstring(event.peers_returned), std::to_wstring(event.duration_ms), wide(event.error)});
        }
    }
    SetDlgItemTextW(dialog, IDC_TRACKER_ERROR, selected ? wide(selected->error).c_str() : L"");
    std::wstring status;
    const bool active = snapshot.state == DownloadState::Downloading || snapshot.state == DownloadState::Seeding;
    if (network.trackers.empty()) status = L"В торренте не указаны трекеры.";
    else if (!active) status = L"Для обновления трекеров запустите загрузку или раздачу.";
    else if (network.tracker.retry_pending) status = L"Обновление запланировано…";
    else if (network.tracker.state == TrackerState::Announcing) status = L"Ожидание ответа трекера…";
    else if (network.tracker.state == TrackerState::Waiting) status = L"Ожидание первого запроса…";
    else status = L"Следующий автоматический поиск через " + std::to_wstring(trackerWaitSeconds(network.tracker.next_announce)) + L" с.";
    const auto remaining = trackerWaitSeconds(network.tracker.retry_after);
    if (active && remaining) status += L" Ручное обновление через " + std::to_wstring(remaining) + L" с.";
    SetDlgItemTextW(dialog, IDC_TRACKER_SUMMARY, status.c_str());
    EnableWindow(GetDlgItem(dialog, IDC_TRACKER_UPDATE), canUpdateTrackers(snapshot));
    state.updating = false;

    if (state.automatic) {
        if (ListView_GetItemCount(list) != static_cast<int>(network.trackers.size()) ||
            ListView_GetItemCount(history) != static_cast<int>(count)) throw std::runtime_error("Tracker rows were not displayed");
        if (state.automatic_update) {
            if (GetTickCount64() >= state.deadline) throw std::runtime_error("Tracker UI update timed out");
            if (!state.sent && canUpdateTrackers(snapshot)) PostMessageW(dialog, WM_COMMAND, IDC_TRACKER_UPDATE, 0);
            size_t requests = 0; for (const auto& tracker : network.trackers) requests += tracker.requests;
            if (state.sent && requests > state.requests_before && !network.tracker.retry_pending &&
                network.tracker.state == TrackerState::Working) PostMessageW(dialog, WM_COMMAND, IDOK, 0);
        }
    }
}

INT_PTR CALLBACK procedure(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
    try {
        if (message == WM_INITDIALOG) {
            state = reinterpret_cast<DialogState*>(lparam); SetWindowLongPtrW(dialog, DWLP_USER, lparam);
            columns(GetDlgItem(dialog, IDC_TRACKERS), {{L"Группа", 55}, {L"Адрес", 320}, {L"Состояние", 165},
                {L"Пиров", 60}, {L"Последний ответ", 160}, {L"Запросов", 70}, {L"Ошибок подряд", 110}});
            columns(GetDlgItem(dialog, IDC_TRACKER_HISTORY), {{L"Время", 160}, {L"Событие", 220},
                {L"Пиров", 60}, {L"мс", 60}, {L"Ошибка", 440}});
            for (const auto& tracker : state->snapshot().network.trackers) state->requests_before += tracker.requests;
            state->deadline = GetTickCount64() + 12000;
            refresh(dialog, *state);
            if (state->automatic && !state->automatic_update) PostMessageW(dialog, WM_COMMAND, IDOK, 0);
            else SetTimer(dialog, 1, 200, nullptr);
            return TRUE;
        }
        if (!state) return FALSE;
        if (message == WM_TIMER) { refresh(dialog, *state); return TRUE; }
        if (message == WM_NOTIFY && !state->updating) {
            const auto header = reinterpret_cast<NMHDR*>(lparam);
            if (header->idFrom == IDC_TRACKERS && header->code == LVN_ITEMCHANGED) {
                const auto changed = reinterpret_cast<NMLISTVIEW*>(lparam);
                if ((changed->uNewState & LVIS_SELECTED) && !(changed->uOldState & LVIS_SELECTED)) {
                    state->selected = std::max(changed->iItem, 0); refresh(dialog, *state);
                }
            }
            return FALSE;
        }
        if (message == WM_COMMAND && LOWORD(wparam) == IDC_TRACKER_UPDATE) {
            if (canUpdateTrackers(state->snapshot()) && state->update()) state->sent = true;
            refresh(dialog, *state); return TRUE;
        }
        if (message == WM_CLOSE || (message == WM_COMMAND && (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL))) {
            KillTimer(dialog, 1); EndDialog(dialog, IDOK); return TRUE;
        }
    } catch (const std::exception& error) {
        if (state) state->error = error.what();
        KillTimer(dialog, 1); EndDialog(dialog, IDCANCEL); return TRUE;
    }
    return FALSE;
}
}

void showTrackerDetails(HWND owner, const std::function<DownloadSnapshot()>& snapshot,
                        const std::function<bool()>& update, bool automatic, bool automatic_update) {
    DialogState state{snapshot, update, automatic, automatic_update};
    const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_TRACKERS), owner,
        procedure, reinterpret_cast<LPARAM>(&state));
    if (result == -1) throw std::runtime_error("Cannot open tracker details");
    if (!state.error.empty()) throw std::runtime_error(state.error);
}
