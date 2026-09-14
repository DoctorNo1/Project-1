// ScreenJournal — a personal, low-profile screen journal for Windows.
//
// What it does:
//   * Captures the whole (multi-monitor) desktop once per minute.
//   * Saves each capture as a timestamped PNG into  Documents\ScreenJournal.
//   * Only captures during the time windows listed in  hours.txt  (if any).
//   * Can start automatically when you log in (toggle from the tray menu).
//   * Runs quietly in the background (no console window, nothing flashing).
//
// What it deliberately does NOT do:
//   * It does not hide from the person using the computer. The process runs
//     under its own name, is visible in Task Manager, autostart uses the normal
//     HKCU\...\Run key (shown in Task Manager > Startup), and it shows a tray
//     icon with a right-click menu so the owner can always find, pause or quit.
//
// This is meant to run on your OWN machine, with your knowledge, as a memory
// aid / activity log. It is not a covert-monitoring tool and is intentionally
// easy to find and stop.
//
// Build (Visual Studio Developer Command Prompt) — /utf-8 keeps the Ukrainian
// menu text and hours.txt template correct:
//   cl /utf-8 /EHsc /O2 /DUNICODE /D_UNICODE screenjournal.cpp ^
//      gdiplus.lib gdi32.lib user32.lib shell32.lib ole32.lib advapi32.lib ^
//      /link /SUBSYSTEM:WINDOWS
//
// Build (MinGW-w64):
//   g++ -O2 -municode -mwindows screenjournal.cpp -o screenjournal.exe ^
//       -lgdiplus -lgdi32 -luser32 -lshell32 -lole32 -ladvapi32

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS   // allow sscanf / _wfopen without MSVC C4996
#endif

#include <windows.h>
#include <shlobj.h>       // SHGetKnownFolderPath
#include <gdiplus.h>
#include <shellapi.h>     // Shell_NotifyIcon
#include <string>
#include <vector>
#include <cstdio>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
static const UINT    kCaptureIntervalMs = 60 * 1000;   // once per minute
static const wchar_t kFolderName[]       = L"ScreenJournal";
static const wchar_t kHoursFileName[]    = L"hours.txt";
static const wchar_t kWindowClass[]      = L"ScreenJournalHiddenWindow";
static const wchar_t kAppTitle[]         = L"Screen Journal";
static const wchar_t kMutexName[]        = L"ScreenJournal_SingleInstance_Mutex";

// Registry autostart (per-user; visible in Task Manager > Startup).
static const wchar_t kRunKey[]   = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t kRunValue[] = L"ScreenJournal";

#define WM_TRAYICON   (WM_APP + 1)
#define ID_TIMER      1
#define IDM_OPEN      2001
#define IDM_STATUS    2002
#define IDM_EXIT      2003
#define IDM_AUTOSTART 2004
#define IDM_HOURS     2005

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static NOTIFYICONDATA g_nid = {};
static std::wstring    g_outputDir;
static bool            g_capturing = true;   // master pause toggle

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Full path to this running executable.
static std::wstring GetExePath()
{
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

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

static std::wstring HoursFilePath()
{
    return g_outputDir + L"\\" + kHoursFileName;
}

// ---------------------------------------------------------------------------
// Active hours (read from hours.txt)
// ---------------------------------------------------------------------------
struct TimeRange { int start; int end; };   // minutes from midnight [start, end)

// Parse one line like "08:00-12:00" into a range. Returns false if not a range.
static bool ParseRange(const char* line, TimeRange* out)
{
    int sh, sm, eh, em;
    if (sscanf(line, " %d:%d - %d:%d", &sh, &sm, &eh, &em) == 4) {
        if (sh >= 0 && sh < 24 && sm >= 0 && sm < 60 &&
            eh >= 0 && eh < 24 && em >= 0 && em < 60) {
            out->start = sh * 60 + sm;
            out->end   = eh * 60 + em;
            return true;
        }
    }
    return false;
}

// Create a documented hours.txt the first time, so the user knows the format.
static void EnsureHoursFile()
{
    std::wstring path = HoursFilePath();
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        return;   // already exists — never overwrite the user's settings

    FILE* f = _wfopen(path.c_str(), L"w");
    if (!f)
        return;
    // UTF-8 BOM so Notepad shows the Ukrainian comments correctly.
    fputs("\xEF\xBB\xBF"
          "# Години, коли робити знімки екрана.\n"
          "# Формат: HH:MM-HH:MM, один діапазон у рядку (24-годинний час).\n"
          "# Рядки, що починаються з #, ігноруються.\n"
          "# Якщо файл порожній або без коректних діапазонів — знімки цілий день.\n"
          "# Діапазон через північ теж працює, напр. 22:00-06:00.\n"
          "#\n"
          "# Приклад (прибери # на початку рядка, щоб увімкнути):\n"
          "# 08:00-12:00\n"
          "# 13:00-17:00\n",
          f);
    fclose(f);
}

// Is the current local time inside an active window?
// No file / no valid ranges  ->  active all day.
static bool IsWithinActiveHours()
{
    FILE* f = _wfopen(HoursFilePath().c_str(), L"r");
    if (!f)
        return true;

    std::vector<TimeRange> ranges;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        const char* p = line;
        // Skip a UTF-8 BOM if it survived on the first line.
        if ((unsigned char)p[0] == 0xEF &&
            (unsigned char)p[1] == 0xBB &&
            (unsigned char)p[2] == 0xBF)
            p += 3;
        while (*p == ' ' || *p == '\t')
            ++p;
        if (*p == '#' || *p == '\r' || *p == '\n' || *p == '\0')
            continue;
        TimeRange r;
        if (ParseRange(p, &r))
            ranges.push_back(r);
    }
    fclose(f);

    if (ranges.empty())
        return true;

    SYSTEMTIME st;
    GetLocalTime(&st);
    const int now = st.wHour * 60 + st.wMinute;

    for (const TimeRange& r : ranges) {
        if (r.start == r.end)
            continue;                       // empty range
        if (r.start < r.end) {
            if (now >= r.start && now < r.end)
                return true;
        } else {                            // range wraps past midnight
            if (now >= r.start || now < r.end)
                return true;
        }
    }
    return false;
}

// Capture only when not paused AND inside the configured hours.
static bool ShouldCaptureNow()
{
    return g_capturing && IsWithinActiveHours();
}

// ---------------------------------------------------------------------------
// Autostart (HKCU\...\Run)
// ---------------------------------------------------------------------------
static bool IsAutostartEnabled()
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    LONG rc = RegQueryValueExW(key, kRunValue, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

static void SetAutostart(bool enable)
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;

    if (enable) {
        std::wstring cmd = L"\"" + GetExePath() + L"\"";
        RegSetValueExW(key, kRunValue, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(cmd.c_str()),
                       static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}

// ---------------------------------------------------------------------------
// Screenshot
// ---------------------------------------------------------------------------

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
    AppendMenuW(menu, MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : 0),
                IDM_AUTOSTART, L"Запускати при вході в систему");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_OPEN,  L"Відкрити папку зі знімками");
    AppendMenuW(menu, MF_STRING, IDM_HOURS, L"Змінити години (hours.txt)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT,  L"Вихід");

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
        if (ShouldCaptureNow())
            CaptureScreen();   // take an initial shot if we're inside active hours
        return 0;

    case WM_TIMER:
        if (wParam == ID_TIMER && ShouldCaptureNow())
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
        case IDM_AUTOSTART:
            SetAutostart(!IsAutostartEnabled());
            break;
        case IDM_OPEN:
            ShellExecuteW(hwnd, L"open", g_outputDir.c_str(),
                          nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case IDM_HOURS:
            EnsureHoursFile();   // make sure it exists before opening
            ShellExecuteW(hwnd, L"open", HoursFilePath().c_str(),
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
    EnsureHoursFile();   // create a documented template on first run

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
