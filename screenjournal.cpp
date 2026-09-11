// ScreenJournal — a personal, low-profile screen journal for Windows.
//
// What it does:
//   * Captures the whole (multi-monitor) desktop once per minute.
//   * Saves each capture as a timestamped PNG into  Documents\ScreenJournal.
//   * Runs quietly in the background (no console window, nothing flashing).
//
// What it deliberately does NOT do:
//   * It does not hide from the person using the computer. The process runs
//     under its own name, is visible in Task Manager, and shows a tray icon
//     with a right-click menu so the owner can always open the folder or quit.
//
// This is meant to run on your OWN machine, with your knowledge, as a memory
// aid / activity log. It is not a covert-monitoring tool and is intentionally
// easy to find and stop.
//
// Build (Visual Studio Developer Command Prompt):
//   cl /EHsc /O2 /DUNICODE /D_UNICODE screenjournal.cpp ^
//      gdiplus.lib gdi32.lib user32.lib shell32.lib ole32.lib /link /SUBSYSTEM:WINDOWS
//
// Build (MinGW-w64):
//   g++ -O2 -municode -mwindows screenjournal.cpp -o screenjournal.exe ^
//       -lgdiplus -lgdi32 -luser32 -lshell32 -lole32

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <shlobj.h>       // SHGetKnownFolderPath
#include <gdiplus.h>
#include <shellapi.h>     // Shell_NotifyIcon
#include <string>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
static const UINT   kCaptureIntervalMs = 60 * 1000;   // once per minute
static const wchar_t kFolderName[]      = L"ScreenJournal";
static const wchar_t kWindowClass[]     = L"ScreenJournalHiddenWindow";
static const wchar_t kAppTitle[]        = L"Screen Journal";
static const wchar_t kMutexName[]       = L"ScreenJournal_SingleInstance_Mutex";

#define WM_TRAYICON   (WM_APP + 1)
#define ID_TIMER      1
#define IDM_OPEN      2001
#define IDM_STATUS    2002
#define IDM_EXIT      2003

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static NOTIFYICONDATA g_nid = {};
static std::wstring    g_outputDir;
static bool            g_capturing = true;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Resolve the encoder CLSID for a given MIME type (e.g. L"image/png").
static bool GetEncoderClsid(const wchar_t* mimeType, CLSID* clsid)
{
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0)
        return false;

    Gdiplus::ImageCodecInfo* codecs =
        static_cast<Gdiplus::ImageCodecInfo*>(malloc(size));
    if (!codecs)
        return false;

    Gdiplus::GetImageEncoders(num, size, codecs);
    bool found = false;
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(codecs[i].MimeType, mimeType) == 0) {
            *clsid = codecs[i].Clsid;
            found = true;
            break;
        }
    }
    free(codecs);
    return found;
}

// Build (and create if needed) the output directory: Documents\ScreenJournal.
static std::wstring ResolveOutputDir()
{
    std::wstring dir;
    PWSTR docs = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
        dir = docs;
        CoTaskMemFree(docs);
    } else {
        wchar_t buf[MAX_PATH] = {};
        GetCurrentDirectoryW(MAX_PATH, buf);
        dir = buf;
    }
    dir += L"\\";
    dir += kFolderName;
    CreateDirectoryW(dir.c_str(), nullptr);   // ignore "already exists"
    return dir;
}

// Timestamped file name: screen_YYYY-MM-DD_HH-MM-SS.png
static std::wstring MakeFilePath()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[128];
    swprintf(name, 128,
             L"\\screen_%04d-%02d-%02d_%02d-%02d-%02d.png",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return g_outputDir + name;
}

// Capture the entire virtual desktop (all monitors) to a PNG file.
static bool CaptureScreen()
{
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (w <= 0 || h <= 0)
        return false;

    HDC screenDC = GetDC(nullptr);
    if (!screenDC)
        return false;

    HDC     memDC  = CreateCompatibleDC(screenDC);
    HBITMAP bmp    = CreateCompatibleBitmap(screenDC, w, h);
    HGDIOBJ oldBmp = SelectObject(memDC, bmp);

    bool ok = false;
    if (BitBlt(memDC, 0, 0, w, h, screenDC, x, y, SRCCOPY | CAPTUREBLT)) {
        Gdiplus::Bitmap bitmap(bmp, nullptr);
        CLSID pngClsid;
        if (GetEncoderClsid(L"image/png", &pngClsid)) {
            std::wstring path = MakeFilePath();
            ok = (bitmap.Save(path.c_str(), &pngClsid, nullptr) == Gdiplus::Ok);
        }
    }

    SelectObject(memDC, oldBmp);
    DeleteObject(bmp);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
    return ok;
}

// ---------------------------------------------------------------------------
// Tray icon
// ---------------------------------------------------------------------------
static void AddTrayIcon(HWND hwnd)
{
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"Screen Journal — активний");
    Shell_NotifyIcon(NIM_ADD, &g_nid);
}

static void RemoveTrayIcon()
{
    Shell_NotifyIcon(NIM_DELETE, &g_nid);
}

static void ShowTrayMenu(HWND hwnd)
{
    POINT pt;
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (g_capturing ? MF_CHECKED : 0),
                IDM_STATUS, L"Знімки екрана активні");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_OPEN, L"Відкрити папку зі знімками");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Вихід");

    // Required so the menu closes correctly when clicking elsewhere.
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        AddTrayIcon(hwnd);
        SetTimer(hwnd, ID_TIMER, kCaptureIntervalMs, nullptr);
        CaptureScreen();   // take an initial shot right away
        return 0;

    case WM_TIMER:
        if (wParam == ID_TIMER && g_capturing)
            CaptureScreen();
        return 0;

    case WM_TRAYICON:
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_LBUTTONUP)
            ShowTrayMenu(hwnd);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_STATUS:
            g_capturing = !g_capturing;
            g_nid.uFlags = NIF_TIP;
            wcscpy_s(g_nid.szTip, g_capturing
                     ? L"Screen Journal — активний"
                     : L"Screen Journal — призупинено");
            Shell_NotifyIcon(NIM_MODIFY, &g_nid);
            break;
        case IDM_OPEN:
            ShellExecuteW(hwnd, L"open", g_outputDir.c_str(),
                          nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            break;
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, ID_TIMER);
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    // Single instance: don't run several copies at once.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr,
                    L"Screen Journal вже запущено.",
                    kAppTitle, MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    // Initialise GDI+.
    Gdiplus::GdiplusStartupInput gdipInput;
    ULONG_PTR gdipToken = 0;
    if (Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr) != Gdiplus::Ok)
        return 1;

    g_outputDir = ResolveOutputDir();

    // Register a message-only-ish hidden window (no visible window is created,
    // but the tray icon keeps the app fully discoverable and controllable).
    WNDCLASSW wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = kWindowClass;
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, kWindowClass, kAppTitle,
                                0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, hInstance, nullptr);
    if (!hwnd) {
        Gdiplus::GdiplusShutdown(gdipToken);
        return 1;
    }

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    Gdiplus::GdiplusShutdown(gdipToken);
    if (mutex)
        ReleaseMutex(mutex);
    return static_cast<int>(msg.wParam);
}
