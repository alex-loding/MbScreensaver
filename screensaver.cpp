// screensaver.cpp
// miniblink screensaver - supports /s /c /p
// Build: g++ screensaver.cpp -I. -o screensaver.scr -luser32 -lcomdlg32 -mwindows
// Deploy: copy screensaver.scr + mb132_x64.dll to C:\Windows\System32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <tlhelp32.h>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cwctype>
#include "mb.h"

// Registry key
#define REG_KEY   L"Software\\MbScreensaver"
#define REG_VAL   L"Url"
#define DEFAULT_URL ""

// Control IDs
#define IDC_EDIT_URL   101
#define IDC_BTN_FILE   102
#define IDC_BTN_OK     103
#define IDC_BTN_CANCEL 104
#define IDC_LBL_URL    105
#define IDC_LBL_TIP    106
#define IDC_BTN_CLEAR  107

// Globals
static mbWebView g_view      = 0;
static HHOOK     g_kbHook    = NULL;
static HHOOK     g_mouseHook = NULL;
static POINT     g_lastMouse = { -1, -1 };
static bool      g_preview   = false;
static HWND      g_hostHwnd  = NULL;
static WNDPROC   g_hostProc  = NULL;
static bool      g_exitAsked = false;
static bool      g_cursorHidden = false;
static bool      g_escOnly = false;

static void AttachHostWindowProc();
static void CleanupRuntime(bool restoreCursor);
static bool RegWriteUrl(const std::wstring& url);
static void FocusWebViewWindow();

static void HideCursorIfNeeded()
{
    if (!g_cursorHidden) {
        ShowCursor(FALSE);
        g_cursorHidden = true;
    }
}

static void RestoreCursorIfNeeded()
{
    if (g_cursorHidden) {
        ShowCursor(TRUE);
        g_cursorHidden = false;
    }
}

static void RequestExit()
{
    if (g_exitAsked) return;
    g_exitAsked = true;

    if (g_view) {
        HWND hwnd = mbGetHostHWND(g_view);
        if (hwnd && IsWindow(hwnd)) {
            PostMessage(hwnd, WM_CLOSE, 0, 0);
        }
    }

    mbExitMessageLoop();
}

static void FocusWebViewWindow()
{
    if (!g_view) return;

    HWND hwnd = mbGetHostHWND(g_view);
    if (hwnd && IsWindow(hwnd)) {
        SetForegroundWindow(hwnd);
        SetActiveWindow(hwnd);
        SetFocus(hwnd);
    }

    mbSetFocus(g_view);
}

static LRESULT CALLBACK HostWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (wParam == VK_ESCAPE) {
            RequestExit();
            return 0;
        }
        break;
    case WM_MOUSEACTIVATE:
        if (g_view) {
            mbSetFocus(g_view);
        }
        return MA_ACTIVATE;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_SETFOCUS:
        if (g_view) {
            mbSetFocus(g_view);
        }
        break;
    case WM_CLOSE:
    case WM_DESTROY:
    case WM_ENDSESSION:
        RequestExit();
        break;
    default:
        break;
    }

    if (g_hostProc) {
        return CallWindowProc(g_hostProc, hWnd, msg, wParam, lParam);
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

static void AttachHostWindowProc()
{
    if (!g_view) return;

    HWND hwnd = mbGetHostHWND(g_view);
    if (!hwnd || hwnd == g_hostHwnd || g_hostProc) return;

    g_hostHwnd = hwnd;
    g_hostProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)HostWndProc);
}

static void CleanupRuntime(bool restoreCursor)
{
    if (g_kbHook) {
        UnhookWindowsHookEx(g_kbHook);
        g_kbHook = NULL;
    }
    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = NULL;
    }

    if (g_hostHwnd && g_hostProc && IsWindow(g_hostHwnd)) {
        SetWindowLongPtrW(g_hostHwnd, GWLP_WNDPROC, (LONG_PTR)g_hostProc);
    }
    g_hostHwnd = NULL;
    g_hostProc = NULL;

    if (g_view) {
        mbDestroyWebView(g_view);
        g_view = 0;
    }
    mbUninit();

    if (restoreCursor) {
        RestoreCursorIfNeeded();
    }

    g_lastMouse.x = -1;
    g_lastMouse.y = -1;
    g_exitAsked = false;
}
// -------------------------------------------------------
static std::wstring GetDefaultUrl()
{
    wchar_t buf[1024] = {};
    MultiByteToWideChar(CP_UTF8, 0, DEFAULT_URL, -1, buf, 1024);
    return std::wstring(buf);
}

static std::wstring TrimWide(const std::wstring& s)
{
    size_t begin = 0;
    while (begin < s.size() && iswspace(s[begin])) ++begin;

    size_t end = s.size();
    while (end > begin && iswspace(s[end - 1])) --end;

    return s.substr(begin, end - begin);
}

static std::wstring NormalizeUrl(const std::wstring& raw)
{
    std::wstring url = TrimWide(raw);
    if (url.empty()) return url;

    if (url.rfind(L"http://", 0) == 0 ||
        url.rfind(L"https://", 0) == 0 ||
        url.rfind(L"file:///", 0) == 0 ||
        url.rfind(L"about:", 0) == 0 ||
        url.rfind(L"data:", 0) == 0) {
        return url;
    }

    bool isDrivePath = (url.size() >= 3 && url[1] == L':' && (url[2] == L'\\' || url[2] == L'/'));
    bool isUNCPath = (url.size() >= 2 && url[0] == L'\\' && url[1] == L'\\');
    if (isDrivePath || isUNCPath) {
        for (auto& ch : url) {
            if (ch == L'\\') ch = L'/';
        }
        if (isUNCPath) {
            return L"file:" + url;
        }
        return L"file:///" + url;
    }

    return url;
}

static bool TryGetBundledHtmlUrl(std::wstring& outUrl)
{
    outUrl.clear();

    wchar_t modulePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(NULL, modulePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return false;
    }

    std::wstring dir(modulePath);
    size_t pos = dir.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return false;
    }

    std::wstring htmlPath = dir.substr(0, pos + 1) + L"screensaver.html";
    DWORD attrs = GetFileAttributesW(htmlPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return false;
    }

    std::wstring normalized = NormalizeUrl(htmlPath);
    if (normalized.empty()) {
        return false;
    }

    outUrl = normalized;
    return true;
}

static bool RegReadUrlRaw(std::wstring& outUrl)
{
    outUrl.clear();

    HKEY hKey;
    wchar_t buf[1024] = {};
    DWORD size = sizeof(buf);
    DWORD type = 0;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    LONG rc = RegQueryValueExW(hKey, REG_VAL, NULL, &type, (LPBYTE)buf, &size);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS || !(type == REG_SZ || type == REG_EXPAND_SZ) || buf[0] == 0) {
        return false;
    }

    outUrl = buf;
    return true;
}

static std::wstring RegReadUrl()
{
    std::wstring url;
    if (RegReadUrlRaw(url)) {
        url = NormalizeUrl(url);
        if (!url.empty()) {
            return url;
        }
    }

    url = NormalizeUrl(GetDefaultUrl());
    if (!url.empty()) {
        return url;
    }

    std::wstring bundledUrl;
    if (TryGetBundledHtmlUrl(bundledUrl)) {
        RegWriteUrl(bundledUrl);
        return bundledUrl;
    }

    return L"";
}


static void LoadConfiguredUrl(mbWebView webView)
{
    if (!webView) return;

    std::wstring wurl = RegReadUrl();
    if (wurl.empty()) {
        mbLoadURL(webView, "about:blank");
        return;
    }

    char url[1024] = {};
    WideCharToMultiByte(CP_UTF8, 0, wurl.c_str(), -1, url, 1024, NULL, NULL);
    mbLoadURL(webView, url);
}
static bool RegWriteUrl(const std::wstring& url)
{
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL,
        REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) != ERROR_SUCCESS) {
        return false;
    }

    LONG rc = RegSetValueExW(hKey, REG_VAL, 0, REG_SZ,
        (LPBYTE)url.c_str(), (DWORD)((url.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
    return rc == ERROR_SUCCESS;
}

static bool RegClearUrl()
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        return true;
    }

    LONG rc = RegDeleteValueW(hKey, REG_VAL);
    RegCloseKey(hKey);
    return (rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND);
}

// -------------------------------------------------------
// Hooks
// -------------------------------------------------------
LRESULT CALLBACK KbProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && wParam == WM_KEYDOWN) {
        KBDLLHOOKSTRUCT* kb = (KBDLLHOOKSTRUCT*)lParam;
        if (!g_escOnly || kb->vkCode == VK_ESCAPE) {
            RestoreCursorIfNeeded();
            RequestExit();
            return 1;
        }
    }
    return CallNextHookEx(g_kbHook, nCode, wParam, lParam);
}

LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION) {
        MSLLHOOKSTRUCT* ms = (MSLLHOOKSTRUCT*)lParam;
        if (wParam == WM_MOUSEMOVE) {
            if (g_lastMouse.x == -1) {
                g_lastMouse = ms->pt;
            } else {
                int dx = abs(ms->pt.x - g_lastMouse.x);
                int dy = abs(ms->pt.y - g_lastMouse.y);
                if (dx > 10 || dy > 10) {
                    RestoreCursorIfNeeded();
                    RequestExit();
                    return 1;
                }
            }
        }
        if (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN || wParam == WM_MBUTTONDOWN) {
            RestoreCursorIfNeeded();
            RequestExit();
            return 1;
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

static void ApplyFullscreenBeforeLoad(mbWebView webView)
{
    if (!webView || g_preview) return;

    HWND hwnd = mbGetHostHWND(webView);
    if (!hwnd) return;

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    LONG style = GetWindowLong(hwnd, GWL_STYLE);
    style |= (WS_POPUP | WS_VISIBLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
    SetWindowLong(hwnd, GWL_STYLE, style);

    LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
    exStyle &= ~(WS_EX_NOACTIVATE | WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
    SetWindowLong(hwnd, GWL_EXSTYLE, exStyle);

    // Use normal top z-order to avoid native popup controls (e.g. <select>) being
    // obscured by a forced topmost host window.
    EnableWindow(hwnd, TRUE);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, sw, sh, SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    mbResize(webView, sw, sh);
}

// -------------------------------------------------------
// Loading finish callback - keep focus after document load
// -------------------------------------------------------
void MB_CALL_TYPE onLoadingFinish(
    mbWebView webView, void* param, void* frame,
    const utf8* url, mbLoadingResult result, const utf8* failedReason)
{
    if (g_preview) return;
    if (result != MB_LOADING_SUCCEEDED) return;

    FocusWebViewWindow();
}

// -------------------------------------------------------
// Settings dialog window proc
// -------------------------------------------------------
static HWND g_hEditUrl = NULL;

LRESULT CALLBACK SettingsWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wParam);

        if (id == IDC_BTN_FILE) {
            wchar_t path[MAX_PATH] = {};
            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner   = hWnd;
            ofn.lpstrFilter = L"HTML \u6587\u4EF6\0*.html;*.htm\0\u6240\u6709\u6587\u4EF6\0*.*\0";
            ofn.lpstrFile   = path;
            ofn.nMaxFile    = MAX_PATH;
            ofn.lpstrTitle  = L"\u9009\u62E9 HTML \u6587\u4EF6";
            ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                std::wstring fileUrl = L"file:///";
                std::wstring p(path);
                for (auto& c : p) if (c == L'\\') c = L'/';
                fileUrl += p;
                SetWindowTextW(g_hEditUrl, fileUrl.c_str());
            }
            return 0;
        }

        if (id == IDC_BTN_CLEAR) {
            if (!RegClearUrl()) {
                MessageBoxW(hWnd, L"\u6E05\u9664\u5DF2\u4FDD\u5B58\u7684 URL \u5931\u8D25\u3002", L"\u9519\u8BEF", MB_OK | MB_ICONERROR);
                return 0;
            }
            SetWindowTextW(g_hEditUrl, L"");
            SetFocus(g_hEditUrl);
            return 0;
        }


        if (id == IDC_BTN_OK || id == IDOK) {
            wchar_t buf[1024] = {};
            GetWindowTextW(g_hEditUrl, buf, 1024);

            std::wstring input = TrimWide(std::wstring(buf));
            if (input.empty()) {
                if (!RegClearUrl()) {
                    MessageBoxW(hWnd, L"\u6E05\u9664\u5DF2\u4FDD\u5B58\u7684 URL \u5931\u8D25\u3002", L"\u9519\u8BEF", MB_OK | MB_ICONERROR);
                    return 0;
                }
            } else {
                std::wstring normalized = NormalizeUrl(input);
                if (!RegWriteUrl(normalized)) {
                    MessageBoxW(hWnd, L"\u5C06 URL \u4FDD\u5B58\u5230\u6CE8\u518C\u8868\u5931\u8D25\u3002", L"\u9519\u8BEF", MB_OK | MB_ICONERROR);
                    return 0;
                }

                std::wstring verify;
                if (!RegReadUrlRaw(verify) || TrimWide(verify) != normalized) {
                    MessageBoxW(hWnd, L"\u4FDD\u5B58\u540E URL \u9A8C\u8BC1\u5931\u8D25\u3002", L"\u9519\u8BEF", MB_OK | MB_ICONERROR);
                    return 0;
                }
            }

            DestroyWindow(hWnd);
            PostQuitMessage(0);
            return 0;
        }

        if (id == IDC_BTN_CANCEL || id == IDCANCEL) {
            DestroyWindow(hWnd);
            PostQuitMessage(0);
            return 0;
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hWnd);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// -------------------------------------------------------
// Show settings window
// -------------------------------------------------------
void ShowSettings(HWND hwndParent)
{
    WNDCLASSW wc    = {};
    wc.lpfnWndProc  = SettingsWndProc;
    wc.hInstance    = GetModuleHandle(NULL);
    wc.hbrBackground= (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName= L"MbScrCfg";
    wc.hCursor      = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int dw = 500, dh = 180;

    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        L"MbScrCfg", L"\u5C4F\u5E55\u4FDD\u62A4\u7A0B\u5E8F\u8BBE\u7F6E",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        (sw - dw) / 2, (sh - dh) / 2, dw, dh,
        hwndParent, NULL, GetModuleHandle(NULL), NULL
    );

    // Label
    CreateWindowW(L"STATIC", L"URL \u6216\u672C\u5730 HTML \u8DEF\u5F84\uFF1A",
        WS_CHILD | WS_VISIBLE,
        10, 14, 460, 18, hDlg, (HMENU)IDC_LBL_URL, GetModuleHandle(NULL), NULL);

    // URL edit box
    g_hEditUrl = CreateWindowExW(WS_EX_CLIENTEDGE,
        L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        10, 36, 350, 24, hDlg, (HMENU)IDC_EDIT_URL, GetModuleHandle(NULL), NULL);

    // Browse button
    CreateWindowW(L"BUTTON", L"\u6D4F\u89C8...",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        368, 36, 108, 24, hDlg, (HMENU)IDC_BTN_FILE, GetModuleHandle(NULL), NULL);

    // Tip label
    CreateWindowW(L"STATIC", L"\u652F\u6301 http/https URL \u548C\u672C\u5730\u6587\u4EF6\uFF08file:///C:/path/to/file.html\uFF09",
        WS_CHILD | WS_VISIBLE,
        10, 68, 470, 18, hDlg, (HMENU)IDC_LBL_TIP, GetModuleHandle(NULL), NULL);

    // Clear saved URL button
    CreateWindowW(L"BUTTON", L"\u6E05\u9664\u5DF2\u4FDD\u5B58",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        190, 100, 90, 28, hDlg, (HMENU)IDC_BTN_CLEAR, GetModuleHandle(NULL), NULL);


    // OK button
    CreateWindowW(L"BUTTON", L"\u786E\u5B9A",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        290, 100, 90, 28, hDlg, (HMENU)IDC_BTN_OK, GetModuleHandle(NULL), NULL);

    // Cancel button
    CreateWindowW(L"BUTTON", L"\u53D6\u6D88",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        390, 100, 90, 28, hDlg, (HMENU)IDC_BTN_CANCEL, GetModuleHandle(NULL), NULL);

    // Fill current saved URL
    SetWindowTextW(g_hEditUrl, RegReadUrl().c_str());

    // Message loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.hwnd == g_hEditUrl && msg.message == WM_KEYDOWN) {
            bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (ctrlDown && (msg.wParam == L'A' || msg.wParam == L'a')) {
                SendMessageW(g_hEditUrl, EM_SETSEL, 0, -1);
                continue;
            }
        }
        if (!IsDialogMessage(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
}

// -------------------------------------------------------
// /s - run screensaver fullscreen
// -------------------------------------------------------
void RunScreensaver()
{
    g_exitAsked = false;
    g_lastMouse.x = -1;
    g_lastMouse.y = -1;

    mbSetMbMainDllPath(L"mb132_x64.dll");
    mbInit(nullptr);
    mbEnableHighDPISupport();

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    g_view = mbCreateWebCustomWindow(
        NULL,
        WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        WS_EX_APPWINDOW,
        0, 0, sw, sh
    );
    mbOnLoadingFinish(g_view, onLoadingFinish, nullptr);
    mbSetCookieEnabled(g_view, true);

    ApplyFullscreenBeforeLoad(g_view);
    mbShowWindow(g_view, TRUE);
    AttachHostWindowProc();
    FocusWebViewWindow();
    LoadConfiguredUrl(g_view);

    // System screensaver behavior: hide cursor and place it at screen center.
    HideCursorIfNeeded();
    SetCursorPos(sw / 2, sh / 2);

    // In system-launched /s mode: any key/mouse interaction exits screensaver.
    g_escOnly = false;
    g_kbHook = SetWindowsHookEx(WH_KEYBOARD_LL, KbProc, NULL, 0);
    g_mouseHook = SetWindowsHookEx(WH_MOUSE_LL, MouseProc, NULL, 0);

    mbRunMessageLoop();
    CleanupRuntime(true);
}

// -------------------------------------------------------
// /p - preview inside screensaver settings miniature
// -------------------------------------------------------
void RunPreview(HWND hwndPreview)
{
    g_preview = true;
    g_exitAsked = false;

    mbSetMbMainDllPath(L"mb132_x64.dll");
    mbInit(nullptr);

    RECT rc;
    GetClientRect(hwndPreview, &rc);
    int w = rc.right  - rc.left;
    int h = rc.bottom - rc.top;

    g_view = mbCreateWebWindow(MB_WINDOW_TYPE_POPUP, NULL, 0, 0, w, h);

    HWND hwndMb = mbGetHostHWND(g_view);
    if (hwndMb) {
        SetParent(hwndMb, hwndPreview);
        SetWindowLong(hwndMb, GWL_STYLE, WS_CHILD | WS_VISIBLE);
        SetWindowPos(hwndMb, NULL, 0, 0, w, h, SWP_NOZORDER | SWP_FRAMECHANGED);
    }

    mbSetCookieEnabled(g_view, true);

    LoadConfiguredUrl(g_view);

    mbShowWindow(g_view, TRUE);
    AttachHostWindowProc();
    mbRunMessageLoop();

    CleanupRuntime(false);
    g_preview = false;
}


// -------------------------------------------------------
// Detect if launched by system screensaver host
// Returns true if parent process is winlogon / scrnsave
// Returns false if launched manually (e.g. explorer)
// -------------------------------------------------------
static bool IsLaunchedBySystem()
{
    DWORD myPid     = GetCurrentProcessId();
    DWORD parentPid = 0;

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (pe.th32ProcessID == myPid) {
                parentPid = pe.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(hSnap, &pe));
    }

    // Now find parent process name
    std::wstring parentName;
    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (pe.th32ProcessID == parentPid) {
                parentName = pe.szExeFile;
                // lowercase
                for (auto& c : parentName) c = (wchar_t)towlower(c);
                break;
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);

    // System launches screensaver from winlogon, logonui, or taskeng
    return (parentName.find(L"winlogon") != std::wstring::npos
         || parentName.find(L"logonui")  != std::wstring::npos
         || parentName.find(L"taskeng")  != std::wstring::npos
         || parentName.find(L"svchost")  != std::wstring::npos);
}

// -------------------------------------------------------
// Direct run - fullscreen, ESC only to exit (no mouse hook)
// -------------------------------------------------------
void RunDirect()
{
    g_exitAsked = false;

    mbSetMbMainDllPath(L"mb132_x64.dll");
    mbInit(nullptr);
    mbEnableHighDPISupport();

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    g_view = mbCreateWebCustomWindow(
        NULL,
        WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        WS_EX_APPWINDOW,
        0, 0, sw, sh
    );
    mbOnLoadingFinish(g_view, onLoadingFinish, nullptr);
    mbSetCookieEnabled(g_view, true);

    ApplyFullscreenBeforeLoad(g_view);
    mbShowWindow(g_view, TRUE);
    AttachHostWindowProc();
    FocusWebViewWindow();
    LoadConfiguredUrl(g_view);

    mbRunMessageLoop();
    CleanupRuntime(false);
}


// -------------------------------------------------------
// WinMain
// -------------------------------------------------------
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR lpCmdLine, int)
{
    std::string cmd(lpCmdLine);
    std::string low = cmd;
    for (auto& c : low) c = (char)tolower((unsigned char)c);

    // Uncomment below to debug which branch is taken:
    // MessageBoxA(NULL, cmd.empty() ? "(empty)" : cmd.c_str(), "cmdline", MB_OK);

    if (low.find("/s") != std::string::npos) {
        if (IsLaunchedBySystem()) {
            // Activated by system: classic screensaver behavior (input exits).
            RunScreensaver();
        } else {
            // Double-clicked .scr manually: ESC only
            RunDirect();
        }

    } else if (low.find("/p") != std::string::npos) {
        // Preview mode
        HWND hwndPreview = NULL;
        size_t pos = low.find("/p");
        std::string rest = cmd.substr(pos + 2);
        size_t i = 0;
        while (i < rest.size() && (rest[i] == ' ' || rest[i] == ':')) i++;
        if (i < rest.size())
            hwndPreview = (HWND)(LONG_PTR)atoll(rest.substr(i).c_str());
        if (hwndPreview)
            RunPreview(hwndPreview);

    } else if (low.find("/c") != std::string::npos) {
        // Settings dialog
        ShowSettings(NULL);

    } else {
        // No args: direct run, ESC to exit, no mouse hook
        RunDirect();
    }

    return 0;
}















