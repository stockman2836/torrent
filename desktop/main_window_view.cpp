#include "main_window.h"
#include "win32_helpers.h"
#include "speed_limits.h"
#include "network_dialog.h"
#include <commctrl.h>
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace torrent::desktop {
using namespace app;
namespace fs = std::filesystem;

void MainWindow::populate(const DownloadSnapshot& snapshot, const Config& config) {
    filling_ = true;
    ListView_DeleteAllItems(files_);
    displayed_ = snapshot.files;
    priorities_.assign(displayed_ ? displayed_->size() : 0, FilePriority::Normal);
    if (displayed_) for (size_t i = 0; i < displayed_->size(); ++i) {
        auto priority = config.file_priorities.find(i);
        if (priority != config.file_priorities.end()) priorities_[i] = priority->second;
        if (!config.file_indices.empty() && std::find(config.file_indices.begin(), config.file_indices.end(), i) == config.file_indices.end())
            priorities_[i] = FilePriority::Skip;
        const auto& file = (*displayed_)[i];
        auto path = wide(file.path), size = bytes(file.length);
        LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = static_cast<int>(i); item.pszText = path.data();
        ListView_InsertItem(files_, &item);
        ListView_SetItemText(files_, i, 1, size.data());
        ListView_SetItemText(files_, i, 2, const_cast<PWSTR>(priorityName(priorities_[i])));
        ListView_SetCheckState(files_, i, priorities_[i] != FilePriority::Skip);
    }
    filling_ = false;
}

void MainWindow::refresh() {
    if (dirty_) { dirty_ = false; saveEdits(); persist(); }
    queue_.tick();
    entries_ = queue_.snapshots();
    if (!current()) selected_ = entries_.empty() ? 0 : entries_.front().id;
    refreshJobs();
    const auto entry = current();
    const auto state = entry ? entry->download : DownloadSnapshot{};
    const bool busy = entry && entry->busy;
    if (selected_ != rendered_) {
        rendered_ = selected_; previous_ = DownloadState::Idle;
        SetWindowTextW(folder_, wide(entry ? entry->config.download_dir : config_.download_dir).c_str());
        SetWindowTextW(downloadLimit_, wide(formatSpeedLimit(entry ? entry->config.max_download_speed : 0)).c_str());
        SetWindowTextW(uploadLimit_, wide(formatSpeedLimit(entry ? entry->config.max_upload_speed : 0)).c_str());
        populate(state, entry ? entry->config : config_);
    } else if (state.files != displayed_) populate(state, entry ? entry->config : config_);
    for (HWND c : {source_, browse_, read_, limit_, networkSettings_}) EnableWindow(c, !closing_);
    for (HWND c : {folder_, choose_}) EnableWindow(c, editable());
    for (HWND c : {downloadLimit_, uploadLimit_, applyLimits_})
        EnableWindow(c, entry && !closing_ && !entry->removing && state.state != DownloadState::Stopping);
    for (HWND c : {all_, none_, priority_, apply_}) EnableWindow(c, prioritiesEditable());
    EnableWindow(start_, editable());
    EnableWindow(stop_, entry && (busy || entry->queued) && !closing_ && state.state != DownloadState::Stopping);
    EnableWindow(remove_, entry && !entry->removing && !closing_);
    EnableWindow(seedSettings_, entry && !entry->removing && !closing_ && state.state != DownloadState::Stopping);
    EnableWindow(open_, entry && !windowText(folder_).empty());
    EnableWindow(networkDetails_, entry && !closing_);
    SetWindowTextW(networkStatus_, entry ? networkHint(state).c_str() : L"");
    SetWindowTextW(start_, state.state == DownloadState::Stopped ? L"Продолжить" : L"Скачать");
    if (entry && state.can_seed && entry->config.seed_after_download) SetWindowTextW(start_, L"Раздавать");
    const wchar_t* label = L"Готов к работе";
    switch (state.state) {
    case DownloadState::Loading: label = L"Чтение метаданных…"; break;
    case DownloadState::Ready: label = L"Выберите файлы и нажмите «Скачать»"; break;
    case DownloadState::Downloading: label = L"Загрузка…"; break;
    case DownloadState::Seeding: label = L"Раздача"; break;
    case DownloadState::Stopping: label = L"Остановка и сохранение прогресса…"; break;
    case DownloadState::Stopped: label = L"Остановлено — можно продолжить загрузку"; break;
    case DownloadState::Complete: label = L"Загрузка завершена"; break;
    case DownloadState::Failed: label = L"Ошибка — подробности в сообщении"; break;
    default: break;
    }
    if (entry && entry->queued && !busy) label = L"В очереди — ожидание свободного места";
    if (entry && entry->removing) label = L"Удаление из списка…";
    if (state.priorities_pending) label = L"Применение приоритетов файлов…";
    std::wostringstream status;
    status << label;
    if (state.state == DownloadState::Seeding)
        status << L" · " << std::fixed << std::setprecision(2) << state.seed_ratio << L" · " << state.seed_seconds / 60 << L" мин";
    if (state.state == DownloadState::Downloading || state.state == DownloadState::Complete)
        status << L"  " << std::fixed << std::setprecision(1) << state.progress << L"%";
    SetWindowTextW(status_, status.str().c_str());
    SendMessageW(progress_, PBM_SETPOS, static_cast<WPARAM>(std::clamp(state.progress, 0.0, 100.0)*10), 0);
    SetWindowTextW(speeds_, (L"↓ " + bytes(state.download_speed) + L"/с    ↑ " + bytes(state.upload_speed) + L"/с").c_str());
    if (displayed_) {
        size_t count = 0; int64_t size = 0;
        for (size_t i = 0; i < displayed_->size(); ++i) if (ListView_GetCheckState(files_, i)) { ++count; size += (*displayed_)[i].length; }
        SetWindowTextW(summary_, (wide(state.name) + L"  ·  Выбрано " + std::to_wstring(count) + L" из " +
            std::to_wstring(displayed_->size()) + L"  ·  " + bytes(size) +
            (busy && state.listen_port ? L"  ·  Порт " + std::to_wstring(state.listen_port) : L"")).c_str());
    } else SetWindowTextW(summary_, L"Файлы появятся после чтения метаданных");
    if (state.state == DownloadState::Failed && previous_ != state.state && !smoke_.enabled)
        MessageBoxW(handle_, wide(state.error).c_str(), L"Ошибка загрузки", MB_OK | MB_ICONERROR);
    previous_ = state.state;
    if (closing_ && queue_.closed()) {
        DestroyWindow(handle_);
    } else if (!closing_ && !dialogOpen_) {
        runSmoke(state, busy);
    }
}

const wchar_t* MainWindow::jobState(const QueueSnapshot& entry) const {
    if (entry.removing) return L"Удаление";
    if (entry.queued && !entry.busy) return L"В очереди";
    switch (entry.download.state) {
    case DownloadState::Loading: return L"Метаданные";
    case DownloadState::Ready: return L"Готов";
    case DownloadState::Downloading: return L"Загрузка";
    case DownloadState::Seeding: return L"Раздача";
    case DownloadState::Stopping: return L"Остановка";
    case DownloadState::Stopped: return L"Остановлен";
    case DownloadState::Complete: return L"Завершён";
    case DownloadState::Failed: return L"Ошибка";
    default: return L"Готов";
    }
}

void MainWindow::refreshJobs() {
    fillingJobs_ = true;
    bool rebuild = ListView_GetItemCount(jobs_) != static_cast<int>(entries_.size());
    if (!rebuild) for (size_t i = 0; i < entries_.size(); ++i) {
        LVITEMW item{}; item.mask = LVIF_PARAM; item.iItem = static_cast<int>(i);
        ListView_GetItem(jobs_, &item);
        if (static_cast<DownloadId>(item.lParam) != entries_[i].id) rebuild = true;
    }
    if (rebuild) ListView_DeleteAllItems(jobs_);
    for (size_t i = 0; i < entries_.size(); ++i) {
        const auto& entry = entries_[i];
        auto name = wide(entry.download.name.empty() ? entry.source : entry.download.name);
        if (rebuild) {
            LVITEMW item{}; item.mask = LVIF_TEXT | LVIF_PARAM; item.iItem = static_cast<int>(i);
            item.pszText = name.data(); item.lParam = static_cast<LPARAM>(entry.id);
            ListView_InsertItem(jobs_, &item);
        } else ListView_SetItemText(jobs_, i, 0, name.data());
        ListView_SetItemText(jobs_, i, 1, const_cast<PWSTR>(jobState(entry)));
        auto progress = std::to_wstring(static_cast<int>(entry.download.progress));
        ListView_SetItemText(jobs_, i, 2, progress.data());
        ListView_SetItemState(jobs_, i, entry.id == selected_ ? LVIS_SELECTED : 0, LVIS_SELECTED);
    }
    fillingJobs_ = false;
}

} // namespace torrent::desktop
