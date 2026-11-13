#include "main_window.h"
#include "win32_helpers.h"
#include "speed_limits.h"
#include "seeding_dialog.h"
#include "network_dialog.h"
#include "network_settings_dialog.h"
#include "global_speed_dialog.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <stdexcept>

namespace torrent::desktop {
using namespace app;
namespace fs = std::filesystem;

void MainWindow::browse() {
    wchar_t path[32768]{};
    OPENFILENAMEW dialog{}; dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = handle_;
    dialog.lpstrFilter = L"Торренты (*.torrent)\0*.torrent\0Все файлы\0*.*\0";
    dialog.lpstrFile = path; dialog.nMaxFile = 32768; dialog.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&dialog)) { SetWindowTextW(source_, path); load(); }
}

void MainWindow::chooseFolder() {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        throw std::runtime_error("Cannot open folder picker");
    DWORD flags = 0; dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (SUCCEEDED(dialog->Show(handle_))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                SetWindowTextW(folder_, path); CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
}

void MainWindow::load() {
    if (closing_) return;
    const auto source = windowText(source_);
    if (source.empty()) { SetFocus(source_); return; }
    saveEdits();
    selected_ = queue_.add(utf8(source), config_);
    SetWindowTextW(source_, L"");
    persist();
    refresh();
}

void MainWindow::saveEdits() {
    if (!displayed_ || selected_ != rendered_ || !prioritiesEditable()) return;
    const auto entry = current();
    std::map<size_t, FilePriority> priorities;
    for (size_t i = 0; i < priorities_.size(); ++i)
        priorities[i] = ListView_GetCheckState(files_, i) ? priorities_[i] : FilePriority::Skip;
    if (!entry->config.file_indices.empty() || priorities != entry->config.file_priorities) {
        try {
            if (!queue_.setFilePriorities(selected_, priorities))
                throw std::runtime_error(u8"Приоритеты не изменены: дождитесь остановки раздачи");
        } catch (...) { populate(entry->download, entry->config); throw; }
    }
    if (editable() && !windowText(folder_).empty()) queue_.configure(selected_, utf8(windowText(folder_)), priorities);
}

void MainWindow::applySpeedLimits() {
    const auto entry = current();
    if (!entry || closing_ || entry->removing) return;
    int64_t download, upload;
    try {
        download = parseSpeedLimit(utf8(windowText(downloadLimit_)));
        upload = parseSpeedLimit(utf8(windowText(uploadLimit_)));
    } catch (const std::invalid_argument&) {
        throw std::invalid_argument(u8"Введите неотрицательные лимиты в КиБ/с. Допустимы дробные значения. 0 — без ограничений.");
    }
    if (queue_.setSpeedLimits(selected_, download, upload)) {
        saveEdits();
        SetWindowTextW(downloadLimit_, wide(formatSpeedLimit(download)).c_str());
        SetWindowTextW(uploadLimit_, wide(formatSpeedLimit(upload)).c_str());
        persist();
    }
}

void MainWindow::start() {
    if (!editable()) return;
    if (windowText(folder_).empty()) throw std::runtime_error("Select a download folder");
    applySpeedLimits();
    if (!displayed_) { queue_.enqueue(selected_); refresh(); return; }
    bool selected = false;
    for (size_t i = 0; i < priorities_.size(); ++i) selected |= ListView_GetCheckState(files_, i) != 0;
    if (!selected) throw std::runtime_error("Select at least one file");
    saveEdits();
    config_.download_dir = utf8(windowText(folder_));
    if (!settings_.empty() && !config_.saveToFile(settings_.u8string()))
        throw std::runtime_error("Cannot save application settings");
    queue_.enqueue(selected_);
    persist();
    refresh();
}

void MainWindow::seedingSettings() {
    const auto entry = current();
    if (!entry || closing_ || entry->removing) return;
    auto options = SeedingOptions::fromConfig(entry->config);
    if (editSeedingOptions(handle_, options, smoke_.enabled) && queue_.setSeedingOptions(selected_, options)) {
        saveEdits(); persist(); refresh();
    }
}

void MainWindow::networkSettings() {
    if (closing_ || dialogOpen_) return;
    auto settings = NetworkSettings::fromConfig(config_);
    dialogOpen_ = true;
    bool accepted;
    try { accepted = editNetworkSettings(handle_, settings, queue_.snapshots().size(), smoke_.enabled, smoke_.mapping); }
    catch (...) { dialogOpen_ = false; throw; }
    dialogOpen_ = false;
    if (smoke_.enabled && !accepted) throw std::runtime_error("Network settings dialog did not save");
    if (!accepted || closing_) return;
    settings.validate(queue_.snapshots().size());
    auto updated = config_;
    settings.applyTo(updated);
    if (!settings_.empty() && !updated.saveToFile(settings_.u8string()))
        throw std::runtime_error(u8"Не удалось сохранить настройки сети");
    queue_.setNetworkSettings(settings);
    config_ = std::move(updated);
    refresh();
}

void MainWindow::globalSpeedLimits() {
    if (closing_ || dialogOpen_) return;
    auto updated = config_;
    dialogOpen_ = true;
    bool accepted;
    try { accepted = editGlobalSpeedLimits(handle_, updated.global_max_download_speed, updated.global_max_upload_speed, smoke_.enabled); }
    catch (...) { dialogOpen_ = false; throw; }
    dialogOpen_ = false;
    if (smoke_.enabled && !accepted) throw std::runtime_error("Global speed settings dialog did not apply");
    if (!accepted || closing_) return;
    if (!settings_.empty() && !updated.saveToFile(settings_.u8string()))
        throw std::runtime_error(u8"Не удалось сохранить общие лимиты скорости");
    queue_.setGlobalSpeedLimits(updated.global_max_download_speed, updated.global_max_upload_speed);
    config_ = std::move(updated);
    refresh();
}

void MainWindow::networkDetails() {
    if (!current() || closing_ || dialogOpen_) return;
    const auto id = selected_;
    dialogOpen_ = true;
    try {
        showNetworkDetails(handle_, [this, id] {
            for (const auto& entry : entries_) if (entry.id == id) return entry.download;
            return DownloadSnapshot{};
        }, [this, id] { return queue_.requestTrackerUpdate(id); }, smoke_.enabled,
            smoke_.trackers && current() && current()->download.state == DownloadState::Downloading);
    } catch (...) { dialogOpen_ = false; throw; }
    dialogOpen_ = false;
}

} // namespace torrent::desktop
