#include "peer_dialog.h"
#include "peer_diagnostics.h"
#include "diagnostic_format.h"
#include "win32_helpers.h"
#include "resource.h"
#include <commctrl.h>
#include <algorithm>
#include <stdexcept>

using namespace torrent;
using namespace torrent::app;
using namespace torrent::desktop;

namespace {
struct DialogState {
    const std::function<DownloadSnapshot()>& snapshot;
    PeerDialogTest test;
    uint64_t selected = 0;
    bool filling = false;
    std::string error;
};

std::wstring clientName(const PeerStatus& peer) { return peer.client.empty() ? L"Не указан" : wide(peer.client); }
std::wstring duration(const PeerStatus& peer) { return std::to_wstring(peer.duration_ms / 1000) + L" с"; }

void refresh(HWND dialog, DialogState& state) {
    const auto snapshot = state.snapshot();
    const auto& network = snapshot.network;
    state.filling = true;
    const auto active = GetDlgItem(dialog, IDC_PEER_ACTIVE);
    if (ListView_GetItemCount(active) != static_cast<int>(network.peers.size())) ListView_DeleteAllItems(active);
    for (size_t i = 0; i < network.peers.size(); ++i) {
        const auto& peer = network.peers[i];
        setListRow(active, static_cast<int>(i), {wide(peerAddress(peer)), clientName(peer),
            peer.incoming ? L"Входящее" : L"Исходящее", wide(peerStateText(peer, snapshot.state == DownloadState::Seeding)),
            bytes(peer.download_speed) + L"/с", bytes(peer.upload_speed) + L"/с", bytes(peer.downloaded_bytes),
            bytes(peer.uploaded_bytes), duration(peer), std::to_wstring(peer.pending_requests)});
    }
    const auto history = GetDlgItem(dialog, IDC_PEER_HISTORY);
    if (ListView_GetItemCount(history) != static_cast<int>(network.peer_history.size())) ListView_DeleteAllItems(history);
    const PeerStatus* selected = nullptr;
    for (const auto& peer : network.peer_history) if (peer.id == state.selected) selected = &peer;
    if (!selected && !network.peer_history.empty()) selected = &network.peer_history.back();
    state.selected = selected ? selected->id : 0;
    for (size_t i = 0; i < network.peer_history.size(); ++i) {
        const auto& peer = network.peer_history[network.peer_history.size() - i - 1];
        setListRow(history, static_cast<int>(i), {wide(formatDiagnosticTime(peer.finished_at_ms)), wide(peerAddress(peer)),
            clientName(peer), peer.incoming ? L"Входящее" : L"Исходящее", wide(peerEndText(peer.end_reason)),
            bytes(peer.downloaded_bytes), bytes(peer.uploaded_bytes), duration(peer)});
        LVITEMW item{}; item.mask = LVIF_PARAM; item.iItem = static_cast<int>(i); item.lParam = static_cast<LPARAM>(peer.id);
        ListView_SetItem(history, &item);
        ListView_SetItemState(history, i, peer.id == state.selected ? LVIS_SELECTED : 0, LVIS_SELECTED);
    }
    SetDlgItemTextW(dialog, IDC_PEER_REASON, selected ? wide(peerResultText(*selected)).c_str() : L"");
    SetDlgItemTextW(dialog, IDC_PEER_SUMMARY, (L"Текущих подключений: " + std::to_wstring(network.peers.size()) +
        L" · завершённых в истории: " + std::to_wstring(network.peer_history.size())).c_str());
    state.filling = false;
    if (state.test != PeerDialogTest::None) {
        if (ListView_GetItemCount(active) != static_cast<int>(network.peers.size()) ||
            ListView_GetItemCount(history) != static_cast<int>(network.peer_history.size()))
            throw std::runtime_error("Peer statistics rows were not displayed");
        if (state.test == PeerDialogTest::Statistics) {
            if (snapshot.state == DownloadState::Downloading) {
                if (std::none_of(network.peers.begin(), network.peers.end(), [](const auto& peer) {
                    return !peer.client.empty() && peer.download_speed > 0 && peer.downloaded_bytes > 0;
                })) throw std::runtime_error("Live peer client/rate was not displayed");
            } else if (network.peer_history.empty() || std::any_of(network.peer_history.begin(), network.peer_history.end(), [](const auto& peer) {
                return peer.end_reason == PeerEndReason::None || peer.download_speed || peer.upload_speed;
            })) throw std::runtime_error("Final peer history was not displayed");
        }
    }
}

INT_PTR CALLBACK procedure(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
    try {
        if (message == WM_INITDIALOG) {
            state = reinterpret_cast<DialogState*>(lparam); SetWindowLongPtrW(dialog, DWLP_USER, lparam);
            configureList(GetDlgItem(dialog, IDC_PEER_ACTIVE), {{L"Адрес", 165}, {L"Клиент", 180}, {L"Направление", 100},
                {L"Состояние", 170}, {L"↓ Скорость", 95}, {L"↑ Скорость", 95}, {L"Получено", 95}, {L"Отдано", 95}, {L"Время", 70}, {L"Запросов", 75}});
            configureList(GetDlgItem(dialog, IDC_PEER_HISTORY), {{L"Завершено", 160}, {L"Адрес", 165}, {L"Клиент", 180},
                {L"Направление", 100}, {L"Причина", 250}, {L"Получено", 95}, {L"Отдано", 95}, {L"Длительность", 95}});
            refresh(dialog, *state);
            if (state->test != PeerDialogTest::None) PostMessageW(dialog, WM_COMMAND, IDOK, 0);
            else SetTimer(dialog, 1, 500, nullptr);
            return TRUE;
        }
        if (!state) return FALSE;
        if (message == WM_TIMER) { refresh(dialog, *state); return TRUE; }
        if (message == WM_NOTIFY && !state->filling) {
            const auto header = reinterpret_cast<NMHDR*>(lparam);
            if (header->idFrom == IDC_PEER_HISTORY && header->code == LVN_ITEMCHANGED) {
                const auto changed = reinterpret_cast<NMLISTVIEW*>(lparam);
                if ((changed->uNewState & LVIS_SELECTED) && !(changed->uOldState & LVIS_SELECTED)) {
                    LVITEMW item{}; item.mask = LVIF_PARAM; item.iItem = changed->iItem;
                    ListView_GetItem(GetDlgItem(dialog, IDC_PEER_HISTORY), &item);
                    state->selected = static_cast<uint64_t>(item.lParam); refresh(dialog, *state);
                }
            }
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

void showPeerDetails(HWND owner, const std::function<DownloadSnapshot()>& snapshot, PeerDialogTest test) {
    DialogState state{snapshot, test};
    const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_PEERS), owner,
        procedure, reinterpret_cast<LPARAM>(&state));
    if (result == -1) throw std::runtime_error("Cannot open peer details");
    if (!state.error.empty()) throw std::runtime_error(state.error);
}
