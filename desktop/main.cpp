#include "main_window.h"
#include <commctrl.h>
#include <shellapi.h>
#include <ole2.h>
#include <stdexcept>

using namespace torrent::desktop;

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    OleInitialize(nullptr);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    SmokeTest smoke;
    try { smoke = parseSmokeTest(argc, argv); }
    catch (const std::invalid_argument&) {
        if (argv) LocalFree(argv);
        OleUninitialize(); return 2;
    }
    if (argv) LocalFree(argv);
    MainWindow window(std::move(smoke));
    WNDCLASSEXW cls{}; cls.cbSize = sizeof(cls); cls.hInstance = instance;
    cls.lpfnWndProc = MainWindow::procedure; cls.lpszClassName = L"TorrentClientWindow";
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
    RegisterClassExW(&cls);
    HWND hwnd = CreateWindowExW(0, cls.lpszClassName, L"TorrentClient", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1200, 850, nullptr, nullptr, instance, &window);
    if (!hwnd) { OleUninitialize(); return 1; }
    ShowWindow(hwnd, window.hidden() ? SW_HIDE : show);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    OleUninitialize();
    return static_cast<int>(message.wParam);
}
