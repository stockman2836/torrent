#include "main_window.h"
#include "win32_helpers.h"
#include <commctrl.h>
#include <uxtheme.h>
#include <algorithm>
#include <stdexcept>

namespace torrent::desktop {
using namespace app;
namespace fs = std::filesystem;

HWND MainWindow::control(const wchar_t* cls, const wchar_t* label, DWORD style, int id) {
    HWND child = CreateWindowExW(cls == WC_EDITW ? WS_EX_CLIENTEDGE : 0, cls, label,
        WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, handle_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (!child) throw std::runtime_error("Cannot create window control");
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    return child;
}

void MainWindow::createControls() {
    HDC dc = GetDC(handle_);
    const int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(handle_, dc);
    font_ = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    heading_ = CreateFontW(-MulDiv(21, dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    title_ = control(L"STATIC", L"TorrentClient", SS_LEFT);
    SendMessageW(title_, WM_SETFONT, reinterpret_cast<WPARAM>(heading_), TRUE);
    subtitle_ = control(L"STATIC", L"Добавляйте торренты и управляйте загрузками в списке слева", SS_LEFT);
    networkSettings_ = control(L"BUTTON", L"Настройки сети…", WS_TABSTOP, EditNetworkSettings);
    globalSpeedLimits_ = control(L"BUTTON", L"Общая скорость…", WS_TABSTOP, GlobalSpeedLimits);
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
    rateLabel_ = control(L"STATIC", L"Лимиты торрента (КиБ/с) · 0 — без ограничений", SS_LEFT);
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
    networkStatus_ = control(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS);
    networkDetails_ = control(L"BUTTON", L"Пиры и трекер…", WS_TABSTOP, NetworkDetails);
    open_ = control(L"BUTTON", L"Открыть папку", WS_TABSTOP, OpenFolder);
}

void MainWindow::layout() {
    if (!font_) return;
    RECT bounds{}; GetClientRect(handle_, &bounds);
    const int w = bounds.right, h = bounds.bottom, m = 24, left = 328, bottom = h - 237;
    const auto move = [](HWND c, int x, int y, int width, int height) { MoveWindow(c, x, y, std::max(1, width), height, TRUE); };
    move(title_, m, 18, w-500, 42); move(subtitle_, m, 66, w-48, 25);
    move(globalSpeedLimits_, w-434, 25, 200, 34);
    move(networkSettings_, w-224, 25, 200, 34);
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
    move(networkStatus_, left, bottom+149, w-left-209, 25);
    move(networkDetails_, w-204, bottom+141, 180, 32);
    move(start_, left, h-53, 150, 34); move(stop_, left+162, h-53, 140, 34); move(open_, w-184, h-53, 160, 34);
    move(seedSettings_, left+314, h-53, 140, 34);
}

} // namespace torrent::desktop
