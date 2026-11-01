#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <uxtheme.h>
// Win32's ERROR macro conflicts with the protocol and logger enum members.
#undef ERROR
#include "download_queue.h"
#include "speed_limits.h"
#include "seeding_dialog.h"
#include "torrent/core/logger.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace torrent;
using namespace torrent::app;

namespace {
std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("Invalid Unicode text");
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}
std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}
std::wstring text(HWND window) {
    std::wstring value(GetWindowTextLengthW(window) + 1, L'\0');
    const int size = GetWindowTextW(window, value.data(), static_cast<int>(value.size()));
    value.resize(size);
    return value;
}
fs::path knownFolder(REFKNOWNFOLDERID id) {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &value))) throw std::runtime_error("Cannot locate user folder");
    fs::path result(value);
    CoTaskMemFree(value);
    return result;
}
std::wstring bytes(int64_t value) {
    const wchar_t* units[] = {L"Б", L"КиБ", L"МиБ", L"ГиБ", L"ТиБ"};
    double amount = static_cast<double>(value);
    int unit = 0;
    while (amount >= 1024 && unit < 4) { amount /= 1024; ++unit; }
    std::wostringstream out;
    out << std::fixed << std::setprecision(unit ? 1 : 0) << amount << L" " << units[unit];
    return out.str();
}
enum Control { Source = 100, Browse, Read, Folder, ChooseFolder, Files, SelectAll, SelectNone,
    Priority, Apply, Start, Stop, OpenFolder, Jobs, Remove, Limit, DownloadLimit, UploadLimit, ApplyLimits, SeedSettings };
const wchar_t* priorityName(FilePriority value) {
    switch (value) {
    case FilePriority::High: return L"Высокий";
    case FilePriority::Low: return L"Низкий";
    case FilePriority::Skip: return L"Пропустить";
    default: return L"Обычный";
    }
}

class Window {
public:
    HWND handle = nullptr;
    bool smoke = false;
    std::wstring smokeSource, smokeSecond, smokeFolder, smokeConfig;
    int exitCode = 0;

    ~Window() { if (font_) DeleteObject(font_); if (heading_) DeleteObject(heading_); }
    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        Window* self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            self->handle = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
        try { return self->dispatch(message, wparam, lparam); }
        catch (const std::exception& error) {
            if (!self->smoke) MessageBoxW(hwnd, wide(error.what()).c_str(), L"TorrentClient", MB_OK | MB_ICONERROR);
            else { self->exitCode = 1; self->close(); }
            if (message == WM_CREATE) return -1;
            return 0;
        }
    }

private:
    DownloadQueue queue_;
    Config config_;
    fs::path settings_, listPath_;
    HWND title_, subtitle_, source_, browse_, read_, folderLabel_, folder_, choose_, files_,
        all_, none_, priority_, apply_, summary_, progress_, status_, speeds_, start_, stop_, open_,
        jobs_, jobsLabel_, remove_, limit_, limitLabel_, rateLabel_, downloadLabel_, uploadLabel_,
        downloadLimit_, uploadLimit_, applyLimits_, seedSettings_;
    HFONT font_ = nullptr, heading_ = nullptr;
    std::shared_ptr<const std::vector<FileInfo>> displayed_;
    std::vector<FilePriority> priorities_;
    bool filling_ = false, fillingJobs_ = false, closing_ = false, smokeStarted_ = false, dirty_ = false;
    ULONGLONG smokeDeadline_ = 0;
    DownloadId selected_ = 0, rendered_ = 0;
    std::vector<QueueSnapshot> entries_;
    DownloadState previous_ = DownloadState::Idle;

    HWND control(const wchar_t* cls, const wchar_t* label, DWORD style, int id = 0) {
        HWND child = CreateWindowExW(cls == WC_EDITW ? WS_EX_CLIENTEDGE : 0, cls, label,
            WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, handle,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        if (!child) throw std::runtime_error("Cannot create window control");
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        return child;
    }
    void create() {
        HDC dc = GetDC(handle);
        const int dpi = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(handle, dc);
        font_ = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        heading_ = CreateFontW(-MulDiv(21, dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        title_ = control(L"STATIC", L"TorrentClient", SS_LEFT);
        SendMessageW(title_, WM_SETFONT, reinterpret_cast<WPARAM>(heading_), TRUE);
        subtitle_ = control(L"STATIC", L"Добавляйте торренты и управляйте загрузками в списке слева", SS_LEFT);
        source_ = control(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, Source);
        SendMessageW(source_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Путь к .torrent или magnet-ссылка"));
        browse_ = control(L"BUTTON", L"Открыть .torrent", WS_TABSTOP, Browse);
        read_ = control(L"BUTTON", L"Добавить", WS_TABSTOP, Read);
        jobsLabel_ = control(L"STATIC", L"Загрузки", SS_LEFT);
        jobs_ = control(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, Jobs);
        ListView_SetExtendedListViewStyle(jobs_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        SetWindowTheme(jobs_, L"Explorer", nullptr);
        const wchar_t* jobColumns[] = {L"Торрент", L"Состояние", L"%"};
        for (int i = 0; i < 3; ++i) {
            LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH;
            column.pszText = const_cast<PWSTR>(jobColumns[i]); column.cx = i == 0 ? 125 : (i == 1 ? 100 : 50);
            ListView_InsertColumn(jobs_, i, &column);
        }
        remove_ = control(L"BUTTON", L"Удалить из списка", WS_TABSTOP, Remove);
        limitLabel_ = control(L"STATIC", L"Одновременных загрузок", SS_LEFT);
        limit_ = control(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST, Limit);
        for (int i = 1; i <= 8; ++i)
            SendMessageW(limit_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(std::to_wstring(i).c_str()));
        SendMessageW(limit_, CB_SETCURSEL, 0, 0);
        folderLabel_ = control(L"STATIC", L"Папка загрузки", SS_LEFT);
        folder_ = control(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, Folder);
        choose_ = control(L"BUTTON", L"Выбрать папку", WS_TABSTOP, ChooseFolder);
        rateLabel_ = control(L"STATIC", L"Лимиты скорости (КиБ/с) · 0 — без ограничений", SS_LEFT);
        downloadLabel_ = control(L"STATIC", L"Скачивание", SS_LEFT);
        uploadLabel_ = control(L"STATIC", L"Отдача", SS_LEFT);
        downloadLimit_ = control(WC_EDITW, L"0", WS_TABSTOP | ES_AUTOHSCROLL, DownloadLimit);
        uploadLimit_ = control(WC_EDITW, L"0", WS_TABSTOP | ES_AUTOHSCROLL, UploadLimit);
        SendMessageW(downloadLimit_, EM_SETLIMITTEXT, 32, 0);
        SendMessageW(uploadLimit_, EM_SETLIMITTEXT, 32, 0);
        applyLimits_ = control(L"BUTTON", L"Применить лимиты", WS_TABSTOP, ApplyLimits);
        files_ = control(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS, Files);
        ListView_SetExtendedListViewStyle(files_, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        SetWindowTheme(files_, L"Explorer", nullptr);
        const wchar_t* columns[] = {L"Файл", L"Размер", L"Приоритет"};
        for (int i = 0; i < 3; ++i) {
            LVCOLUMNW col{}; col.mask = LVCF_TEXT | LVCF_WIDTH; col.pszText = const_cast<PWSTR>(columns[i]);
            col.cx = i == 0 ? 550 : 130;
            ListView_InsertColumn(files_, i, &col);
        }
        all_ = control(L"BUTTON", L"Выбрать все", WS_TABSTOP, SelectAll);
        none_ = control(L"BUTTON", L"Снять выбор", WS_TABSTOP, SelectNone);
        priority_ = control(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST, Priority);
        for (auto label : {L"Обычный", L"Высокий", L"Низкий", L"Пропустить"})
            SendMessageW(priority_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        SendMessageW(priority_, CB_SETCURSEL, 0, 0);
        apply_ = control(L"BUTTON", L"Применить к выделенным", WS_TABSTOP, Apply);
        summary_ = control(L"STATIC", L"Файлы появятся после чтения метаданных", SS_LEFT);
        progress_ = control(PROGRESS_CLASSW, L"", 0);
        SendMessageW(progress_, PBM_SETRANGE32, 0, 1000);
        status_ = control(L"STATIC", L"Готов к работе", SS_LEFT);
        speeds_ = control(L"STATIC", L"", SS_RIGHT);
        start_ = control(L"BUTTON", L"Скачать", WS_TABSTOP | BS_DEFPUSHBUTTON, Start);
        stop_ = control(L"BUTTON", L"Остановить", WS_TABSTOP, Stop);
        seedSettings_ = control(L"BUTTON", L"Раздача…", WS_TABSTOP, SeedSettings);
        open_ = control(L"BUTTON", L"Открыть папку", WS_TABSTOP, OpenFolder);
        auto home = knownFolder(FOLDERID_LocalAppData) / L"TorrentClient";
        fs::create_directories(home);
        settings_ = home / L"config.json";
        listPath_ = home / L"downloads.json";
        config_.download_dir = (knownFolder(FOLDERID_Downloads) / L"TorrentClient").u8string();
        config_.resume_dir = (home / L"resume").u8string();
        if (fs::exists(settings_)) config_.loadFromFile(settings_.u8string());
        config_.seed_after_download = false; // New jobs opt in; older desktop versions ignored this JSON field.
        if (smoke) {
            config_.loadFromFile(utf8(smokeConfig));
            settings_.clear();
            listPath_.clear();
            config_.download_dir = utf8(smokeFolder);
            SetWindowTextW(source_, smokeSource.c_str());
        }
        // File logging is optional; a logging failure must not prevent opening the app.
        try { Logger::init((home / L"desktop.log").u8string(), Logger::Level::OFF); } catch (...) {}
        SetWindowTextW(folder_, wide(config_.download_dir).c_str());
        if (!listPath_.empty() && fs::exists(listPath_)) {
            try { queue_.restore(listPath_.u8string(), config_); }
            catch (const std::exception& error) {
                // Preserve the original file if it could not be fully restored.
                listPath_.clear();
                MessageBoxW(handle, wide(error.what()).c_str(), L"Не удалось восстановить список загрузок", MB_OK | MB_ICONERROR);
            }
        }
        SendMessageW(limit_, CB_SETCURSEL, queue_.limit()-1, 0);
        SetTimer(handle, 1, 150, nullptr);
        if (smoke) {
            smokeDeadline_ = GetTickCount64() + 45000;
            if (smokeSecond.empty()) load();
            else {
                selected_ = queue_.add(utf8(smokeSource), config_);
                queue_.add(utf8(smokeSecond), config_);
                SetWindowTextW(source_, L"");
            }
        }
        refresh();
    }
    void layout() {
        if (!font_) return;
        RECT bounds{}; GetClientRect(handle, &bounds);
        const int w = bounds.right, h = bounds.bottom, m = 24, left = 328, bottom = h - 205;
        const auto move = [](HWND c, int x, int y, int width, int height) { MoveWindow(c, x, y, std::max(1, width), height, TRUE); };
        move(title_, m, 18, w-48, 42); move(subtitle_, m, 66, w-48, 25);
        move(source_, m, 107, w-320, 32); move(browse_, w-284, 106, 155, 34); move(read_, w-119, 106, 95, 34);
        move(jobsLabel_, m, 158, 280, 22); move(jobs_, m, 188, 280, h-370);
        move(limitLabel_, m, h-167, 280, 24); move(limit_, m, h-137, 90, 250);
        move(remove_, m, h-91, 280, 34);
        move(folderLabel_, left, 158, 170, 22); move(folder_, left, 188, w-left-196, 32); move(choose_, w-184, 187, 160, 34);
        move(rateLabel_, left, 231, w-left-24, 23);
        move(downloadLabel_, left, 268, 100, 24); move(downloadLimit_, left+103, 260, 95, 32);
        move(uploadLabel_, left+217, 268, 75, 24); move(uploadLimit_, left+292, 260, 95, 32);
        move(applyLimits_, left+407, 259, 180, 34);
        move(files_, left, 306, w-left-24, bottom-306);
        ListView_SetColumnWidth(files_, 0, std::max(250, w-left-310));
        move(all_, left, bottom+12, 110, 32); move(none_, left+120, bottom+12, 110, 32);
        move(priority_, w-403, bottom+12, 125, 240); move(apply_, w-268, bottom+11, 244, 34);
        move(summary_, left, bottom+56, w-left-24, 23); move(progress_, left, bottom+85, w-left-24, 17);
        move(status_, left, bottom+112, w-left-275, 25); move(speeds_, w-270, bottom+112, 246, 25);
        move(start_, left, h-53, 150, 34); move(stop_, left+162, h-53, 140, 34); move(open_, w-184, h-53, 160, 34);
        move(seedSettings_, left+314, h-53, 140, 34);
    }
    void browse() {
        wchar_t path[32768]{};
        OPENFILENAMEW dialog{}; dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = handle;
        dialog.lpstrFilter = L"Торренты (*.torrent)\0*.torrent\0Все файлы\0*.*\0";
        dialog.lpstrFile = path; dialog.nMaxFile = 32768; dialog.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&dialog)) { SetWindowTextW(source_, path); load(); }
    }
    void chooseFolder() {
        IFileOpenDialog* dialog = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
            throw std::runtime_error("Cannot open folder picker");
        DWORD flags = 0; dialog->GetOptions(&flags);
        dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        if (SUCCEEDED(dialog->Show(handle))) {
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
    void load() {
        if (closing_) return;
        const auto source = text(source_);
        if (source.empty()) { SetFocus(source_); return; }
        saveEdits();
        selected_ = queue_.add(utf8(source), config_);
        SetWindowTextW(source_, L"");
        persist();
        refresh();
    }
    const QueueSnapshot* current() const {
        for (const auto& entry : entries_) if (entry.id == selected_) return &entry;
        return nullptr;
    }
    bool editable() const {
        const auto entry = current();
        return entry && !entry->busy && !entry->queued && !entry->removing && !closing_;
    }
    void saveEdits() {
        if (!displayed_ || selected_ != rendered_ || !editable()) return;
        if (text(folder_).empty()) return;
        std::map<size_t, FilePriority> priorities;
        for (size_t i = 0; i < priorities_.size(); ++i)
            priorities[i] = ListView_GetCheckState(files_, i) ? priorities_[i] : FilePriority::Skip;
        queue_.configure(selected_, utf8(text(folder_)), priorities);
    }
    void persist() {
        dirty_ = false;
        if (!listPath_.empty()) queue_.save(listPath_.u8string());
    }
    void applySpeedLimits() {
        const auto entry = current();
        if (!entry || closing_ || entry->removing) return;
        int64_t download, upload;
        try {
            download = parseSpeedLimit(utf8(text(downloadLimit_)));
            upload = parseSpeedLimit(utf8(text(uploadLimit_)));
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
    void start() {
        if (!editable()) return;
        if (text(folder_).empty()) throw std::runtime_error("Select a download folder");
        applySpeedLimits();
        if (!displayed_) { queue_.enqueue(selected_); refresh(); return; }
        bool selected = false;
        for (size_t i = 0; i < priorities_.size(); ++i) selected |= ListView_GetCheckState(files_, i) != 0;
        if (!selected) throw std::runtime_error("Select at least one file");
        saveEdits();
        config_.download_dir = utf8(text(folder_));
        if (!settings_.empty() && !config_.saveToFile(settings_.u8string()))
            throw std::runtime_error("Cannot save application settings");
        queue_.enqueue(selected_);
        persist();
        refresh();
    }
    void seedingSettings() {
        const auto entry = current();
        if (!entry || closing_ || entry->removing) return;
        auto options = SeedingOptions::fromConfig(entry->config);
        if (editSeedingOptions(handle, options, smoke) && queue_.setSeedingOptions(selected_, options)) {
            saveEdits(); persist(); refresh();
        }
    }
    void populate(const DownloadSnapshot& snapshot, const Config& config) {
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
    void refresh() {
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
        for (HWND c : {source_, browse_, read_, limit_}) EnableWindow(c, !closing_);
        for (HWND c : {folder_, choose_}) EnableWindow(c, editable());
        for (HWND c : {downloadLimit_, uploadLimit_, applyLimits_})
            EnableWindow(c, entry && !closing_ && !entry->removing && state.state != DownloadState::Stopping);
        for (HWND c : {all_, none_, priority_, apply_}) EnableWindow(c, displayed_ && editable());
        EnableWindow(start_, editable());
        EnableWindow(stop_, entry && (busy || entry->queued) && !closing_ && state.state != DownloadState::Stopping);
        EnableWindow(remove_, entry && !entry->removing && !closing_);
        EnableWindow(seedSettings_, entry && !entry->removing && !closing_ && state.state != DownloadState::Stopping);
        EnableWindow(open_, entry && !text(folder_).empty());
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
                std::to_wstring(displayed_->size()) + L"  ·  " + bytes(size)).c_str());
        } else SetWindowTextW(summary_, L"Файлы появятся после чтения метаданных");
        if (state.state == DownloadState::Failed && previous_ != state.state && !smoke)
            MessageBoxW(handle, wide(state.error).c_str(), L"Ошибка загрузки", MB_OK | MB_ICONERROR);
        previous_ = state.state;
        if (closing_ && queue_.closed()) {
            DestroyWindow(handle);
        } else if (smoke && !closing_) {
            if (!smokeSecond.empty()) { smokeQueue(); return; }
            if (!busy && state.state == DownloadState::Ready && !smokeStarted_) {
                if (ListView_GetItemCount(files_) == 0) { exitCode = 1; close(); }
                else { smokeStarted_ = true; SendMessageW(handle, WM_COMMAND, Start, 0); }
            } else if (!busy && state.state == DownloadState::Complete) close();
            else if (state.state == DownloadState::Failed || GetTickCount64() > smokeDeadline_) { exitCode = 1; close(); }
        }
    }
    void smokeQueue() {
        if (GetTickCount64() > smokeDeadline_ || std::any_of(entries_.begin(), entries_.end(), [](const auto& entry) {
            return entry.download.state == DownloadState::Failed;
        })) { exitCode = 1; close(); return; }
        if (!smokeStarted_) {
            if (entries_.size() != 2 || std::any_of(entries_.begin(), entries_.end(), [](const auto& entry) { return entry.busy; })) return;
            smokeStarted_ = true; // Command handlers refresh the window recursively.
            SendMessageW(handle, WM_COMMAND, SeedSettings, 0);
            SetWindowTextW(downloadLimit_, L"32"); SetWindowTextW(uploadLimit_, L"16");
            SendMessageW(handle, WM_COMMAND, ApplyLimits, 0);
            ListView_SetItemState(files_, 0, LVIS_SELECTED, LVIS_SELECTED);
            SendMessageW(priority_, CB_SETCURSEL, 1, 0);
            SendMessageW(handle, WM_COMMAND, Apply, 0);
            SendMessageW(handle, WM_COMMAND, Start, 0);
            ListView_SetItemState(jobs_, 1, LVIS_SELECTED, LVIS_SELECTED);
            SetWindowTextW(downloadLimit_, L"64"); SetWindowTextW(uploadLimit_, L"0");
            SendMessageW(handle, WM_COMMAND, ApplyLimits, 0);
            ListView_SetItemState(files_, 0, LVIS_SELECTED, LVIS_SELECTED);
            SendMessageW(priority_, CB_SETCURSEL, 2, 0);
            SendMessageW(handle, WM_COMMAND, Apply, 0);
            SendMessageW(handle, WM_COMMAND, Start, 0);
            const auto entries = queue_.snapshots();
            if (entries[0].config.file_priorities.at(0) != FilePriority::High ||
                entries[1].config.file_priorities.at(0) != FilePriority::Low || !entries[1].queued ||
                entries[0].config.max_download_speed != 32768 || entries[0].config.max_upload_speed != 16384 ||
                entries[1].config.max_download_speed != 65536 || entries[1].config.max_upload_speed != 0) {
                exitCode = 1; close();
            }
        } else if (std::all_of(entries_.begin(), entries_.end(), [](const auto& entry) {
            return !entry.busy && entry.download.state == DownloadState::Complete;
        })) close();
    }
    const wchar_t* jobState(const QueueSnapshot& entry) const {
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
    void refreshJobs() {
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
    void close() {
        if (closing_) return;
        // Persist before stopping so current edits are included in the saved list.
        try { saveEdits(); persist(); } catch (const std::exception& error) {
            if (!smoke) MessageBoxW(handle, wide(error.what()).c_str(), L"Не удалось сохранить список", MB_OK | MB_ICONERROR);
            else exitCode = 1;
        }
        closing_ = true; queue_.close();
    }
    LRESULT dispatch(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE: create(); return 0;
        case WM_SIZE: layout(); return 0;
        case WM_GETMINMAXINFO: {
            auto info = reinterpret_cast<MINMAXINFO*>(lparam); info->ptMinTrackSize = {1050, 730}; return 0;
        }
        case WM_TIMER: refresh(); return 0;
        case WM_CLOSE: close(); return 0;
        case WM_DESTROY: KillTimer(handle, 1); PostQuitMessage(exitCode); return 0;
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
                if (((change->uOldState ^ change->uNewState) & LVIS_STATEIMAGEMASK) && !editable()) return TRUE;
            }
            if (header->idFrom == Files && header->code == LVN_ITEMCHANGED && !filling_) {
                auto change = reinterpret_cast<NMLISTVIEW*>(lparam);
                if ((change->uOldState ^ change->uNewState) & LVIS_STATEIMAGEMASK) dirty_ = true;
                if (change->iItem >= 0 && static_cast<size_t>(change->iItem) < priorities_.size() &&
                    ListView_GetCheckState(files_, change->iItem) && priorities_[change->iItem] == FilePriority::Skip) {
                    priorities_[change->iItem] = FilePriority::Normal;
                    ListView_SetItemText(files_, change->iItem, 2, const_cast<PWSTR>(L"Обычный"));
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
                const auto directory = fs::absolute(fs::u8path(utf8(text(folder_))));
                fs::create_directories(directory);
                ShellExecuteW(handle, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
            }
            case SelectAll: case SelectNone:
                if (editable()) for (size_t i = 0; i < priorities_.size(); ++i) ListView_SetCheckState(files_, i, LOWORD(wparam) == SelectAll);
                break;
            case Apply:
                if (editable()) {
                    const FilePriority values[] = {FilePriority::Normal, FilePriority::High, FilePriority::Low, FilePriority::Skip};
                    const auto selection = SendMessageW(priority_, CB_GETCURSEL, 0, 0);
                    if (selection < 0 || selection > 3) break;
                    for (int i = -1; (i = ListView_GetNextItem(files_, i, LVNI_SELECTED)) != -1;) {
                        priorities_[i] = values[selection];
                        ListView_SetCheckState(files_, i, values[selection] != FilePriority::Skip);
                        ListView_SetItemText(files_, i, 2, const_cast<PWSTR>(priorityName(values[selection])));
                    }
                    dirty_ = true;
                }
                break;
            }
            return 0;
        }
        return DefWindowProcW(handle, message, wparam, lparam);
    }
};
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    OleInitialize(nullptr);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    Window window;
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc == 5 && std::wstring(argv[1]) == L"--ui-test") {
        window.smoke = true; window.smokeSource = argv[2]; window.smokeFolder = argv[3]; window.smokeConfig = argv[4];
    }
    if (argv && argc == 6 && std::wstring(argv[1]) == L"--ui-queue-test") {
        window.smoke = true; window.smokeSource = argv[2]; window.smokeSecond = argv[3];
        window.smokeFolder = argv[4]; window.smokeConfig = argv[5];
    }
    if (argv) LocalFree(argv);
    WNDCLASSEXW cls{}; cls.cbSize = sizeof(cls); cls.hInstance = instance;
    cls.lpfnWndProc = Window::procedure; cls.lpszClassName = L"TorrentClientWindow";
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
    RegisterClassExW(&cls);
    HWND hwnd = CreateWindowExW(0, cls.lpszClassName, L"TorrentClient", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1200, 850, nullptr, nullptr, instance, &window);
    if (!hwnd) { OleUninitialize(); return 1; }
    ShowWindow(hwnd, window.smoke ? SW_HIDE : show);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    OleUninitialize();
    return static_cast<int>(message.wParam);
}
