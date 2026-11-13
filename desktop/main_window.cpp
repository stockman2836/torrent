#include "main_window.h"
#include "win32_helpers.h"
#include "torrent/core/logger.h"
#include <commctrl.h>
#include <shellapi.h>
#include <stdexcept>

namespace torrent::desktop {
using namespace app;
namespace fs = std::filesystem;

MainWindow::~MainWindow() { if (font_) DeleteObject(font_); if (heading_) DeleteObject(heading_); }

LRESULT CALLBACK MainWindow::procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        self->handle_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
    try { return self->dispatch(message, wparam, lparam); }
    catch (const std::exception& error) {
        if (!self->smoke_.enabled) MessageBoxW(hwnd, wide(error.what()).c_str(), L"TorrentClient", MB_OK | MB_ICONERROR);
        else { self->exitCode_ = 1; self->close(); }
        if (message == WM_CREATE) return -1;
        return 0;
    }
}

void MainWindow::create() {
    createControls();
    initializeDownloads();
    startSmoke();
    refresh();
}

void MainWindow::initializeDownloads() {
    auto home = knownFolder(FOLDERID_LocalAppData) / L"TorrentClient";
    fs::create_directories(home);
    settings_ = home / L"config.json";
    listPath_ = home / L"downloads.json";
    config_.download_dir = (knownFolder(FOLDERID_Downloads) / L"TorrentClient").u8string();
    config_.resume_dir = (home / L"resume").u8string();
    if (fs::exists(settings_)) config_.loadFromFile(settings_.u8string());
    config_.seed_after_download = false; // New jobs opt in; older desktop versions ignored this JSON field.
    configureSmoke();
    // File logging is optional; a logging failure must not prevent opening the app.
    try { Logger::init((home / L"desktop.log").u8string(), Logger::Level::OFF); } catch (...) {}
    SetWindowTextW(folder_, wide(config_.download_dir).c_str());
    queue_.setNetworkSettings(NetworkSettings::fromConfig(config_));
    queue_.setGlobalSpeedLimits(config_.global_max_download_speed, config_.global_max_upload_speed);
    if (!listPath_.empty() && fs::exists(listPath_)) {
        try { queue_.restore(listPath_.u8string(), config_); }
        catch (const std::exception& error) {
            // Preserve the original file if it could not be fully restored.
            listPath_.clear();
            MessageBoxW(handle_, wide(error.what()).c_str(), L"Не удалось восстановить список загрузок", MB_OK | MB_ICONERROR);
        }
    }
    SendMessageW(limit_, CB_SETCURSEL, queue_.limit()-1, 0);
    SetTimer(handle_, 1, 150, nullptr);
}

const QueueSnapshot* MainWindow::current() const {
    for (const auto& entry : entries_) if (entry.id == selected_) return &entry;
    return nullptr;
}

bool MainWindow::editable() const {
    const auto entry = current();
    return entry && !entry->busy && !entry->queued && !entry->removing && !closing_;
}

bool MainWindow::prioritiesEditable() const {
    const auto entry = current();
    return entry && displayed_ && !closing_ && !entry->removing &&
        entry->download.state != DownloadState::Stopping &&
        (!entry->busy || entry->download.state == DownloadState::Downloading || entry->download.state == DownloadState::Seeding);
}

void MainWindow::persist() {
    dirty_ = false;
    if (!listPath_.empty()) queue_.save(listPath_.u8string());
}

void MainWindow::close() {
    if (closing_) return;
    // Persist before stopping so current edits are included in the saved list.
    try { saveEdits(); persist(); } catch (const std::exception& error) {
        if (!smoke_.enabled) MessageBoxW(handle_, wide(error.what()).c_str(), L"Не удалось сохранить список", MB_OK | MB_ICONERROR);
        else exitCode_ = 1;
    }
    closing_ = true; queue_.close();
}

LRESULT MainWindow::dispatch(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: create(); return 0;
    case WM_SIZE: layout(); return 0;
    case WM_GETMINMAXINFO: {
        auto info = reinterpret_cast<MINMAXINFO*>(lparam); info->ptMinTrackSize = {1050, 730}; return 0;
    }
    case WM_TIMER: refresh(); return 0;
    case WM_CLOSE: close(); return 0;
    case WM_DESTROY: KillTimer(handle_, 1); PostQuitMessage(exitCode_); return 0;
    case WM_NOTIFY: {
        auto header = reinterpret_cast<NMHDR*>(lparam);
        if (header->idFrom == Jobs && header->code == LVN_ITEMCHANGED && !fillingJobs_ && !closing_) {
            auto change = reinterpret_cast<NMLISTVIEW*>(lparam);
            if ((change->uNewState & LVIS_SELECTED) && !(change->uOldState & LVIS_SELECTED)) {
                LVITEMW item{}; item.mask = LVIF_PARAM; item.iItem = change->iItem;
                ListView_GetItem(jobs_, &item);
                saveEdits(); selected_ = static_cast<DownloadId>(item.lParam);
                persist(); refresh();
            }
        }
        if (header->idFrom == Files && header->code == LVN_ITEMCHANGING && !filling_) {
            auto change = reinterpret_cast<NMLISTVIEW*>(lparam);
            if (((change->uOldState ^ change->uNewState) & LVIS_STATEIMAGEMASK) && !prioritiesEditable()) return TRUE;
        }
        if (header->idFrom == Files && header->code == LVN_ITEMCHANGED && !filling_) {
            auto change = reinterpret_cast<NMLISTVIEW*>(lparam);
            if ((change->uOldState ^ change->uNewState) & LVIS_STATEIMAGEMASK) {
                dirty_ = true;
                if (change->iItem >= 0 && static_cast<size_t>(change->iItem) < priorities_.size()) {
                    const bool checked = ListView_GetCheckState(files_, change->iItem);
                    if (checked && priorities_[change->iItem] == FilePriority::Skip) priorities_[change->iItem] = FilePriority::Normal;
                    ListView_SetItemText(files_, change->iItem, 2,
                        const_cast<PWSTR>(priorityName(checked ? priorities_[change->iItem] : FilePriority::Skip)));
                }
            }
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case Browse: browse(); break;
        case Read: load(); break;
        case ChooseFolder: chooseFolder(); break;
        case Start: start(); break;
        case SeedSettings: seedingSettings(); break;
        case NetworkDetails: networkDetails(); break;
        case EditNetworkSettings: networkSettings(); break;
        case GlobalSpeedLimits: globalSpeedLimits(); break;
        case ApplyLimits: applySpeedLimits(); refresh(); break;
        case Stop: queue_.stop(selected_); refresh(); break;
        case Remove: queue_.remove(selected_); persist(); refresh(); break;
        case Limit:
            if (HIWORD(wparam) == CBN_SELCHANGE) {
                queue_.setLimit(static_cast<size_t>(SendMessageW(limit_, CB_GETCURSEL, 0, 0)) + 1);
                persist(); refresh();
            }
            break;
        case OpenFolder: {
            const auto directory = fs::absolute(fs::u8path(utf8(windowText(folder_))));
            fs::create_directories(directory);
            ShellExecuteW(handle_, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        }
        case SelectAll: case SelectNone:
            if (prioritiesEditable()) for (size_t i = 0; i < priorities_.size(); ++i) ListView_SetCheckState(files_, i, LOWORD(wparam) == SelectAll);
            break;
        case Apply:
            if (prioritiesEditable()) {
                const FilePriority values[] = {FilePriority::Normal, FilePriority::High, FilePriority::Low, FilePriority::Skip};
                const auto selection = SendMessageW(priority_, CB_GETCURSEL, 0, 0);
                if (selection < 0 || selection > 3) break;
                for (int i = -1; (i = ListView_GetNextItem(files_, i, LVNI_SELECTED)) != -1;) {
                    priorities_[i] = values[selection];
                    ListView_SetCheckState(files_, i, values[selection] != FilePriority::Skip);
                    ListView_SetItemText(files_, i, 2, const_cast<PWSTR>(priorityName(values[selection])));
                }
                dirty_ = true;
                saveEdits(); persist(); refresh();
            }
            break;
        }
        return 0;
    }
    return DefWindowProcW(handle_, message, wparam, lparam);
}

} // namespace torrent::desktop
