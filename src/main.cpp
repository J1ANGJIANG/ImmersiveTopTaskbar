#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <oleauto.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <winhttp.h>

#include "resource.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cwctype>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

// 程序自身版本（与 installer.iss 的 #define MyAppVersion 保持一致）。
constexpr auto kAppVersion = L"1.1.0";
// GitHub Releases 更新源（owner/repo）。
constexpr auto kUpdateOwner = L"J1ANGJIANG";
constexpr auto kUpdateRepo = L"ImmersiveTopTaskbar";
constexpr auto kInstallerAssetPrefix = "ImmersiveTopTaskbar-Setup-";
constexpr auto kInstallerAssetSuffix = ".exe";
constexpr auto kFeedbackEmail = L"jianghongfu123@gmail.com";
constexpr auto kFeedbackIssueTitle = L"用户反馈";

constexpr UINT_PTR kStateTimer = 1;
constexpr UINT_PTR kAnimTimer = 2;
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kRefreshMessage = WM_APP + 2;
constexpr UINT kReassertMessage = WM_APP + 3;
constexpr UINT kStateIntervalMs = 33;
constexpr UINT kAnimIntervalMs = 8;
constexpr DWORD kAnimDurationMs = 150;
constexpr DWORD kLightAnimDurationMs = 420;
constexpr DWORD kMaximizedTargetHoldMs = 1500;
constexpr DWORD kRestoreDebounceMs = 300;
constexpr DWORD kReapplyIntervalMs = 250;
constexpr DWORD kInteractionReapplyIntervalMs = 6;
constexpr DWORD kMoveSizeReapplyIntervalMs = 120;
constexpr DWORD kTaskbarInteractionSettleMs = 2200;
constexpr DWORD kDefaultReassertDurationMs = 2000;
constexpr DWORD kDefaultReassertIntervalMs = 80;
constexpr DWORD kSettingsSampleSettleMs = 160;
constexpr DWORD kShellThemeSwitchCooldownMs = 650;
constexpr DWORD kShellThemeFastSwitchCooldownMs = 80;
constexpr DWORD kShellThemeSettleDelayMs = 180;
constexpr int kSettingsNeutralDriftDistance2 = 64;
constexpr int kTrayIconId = 100;
constexpr int kTrayMenuCheckUpdate = 1;
constexpr int kTrayMenuFeedback = 2;
constexpr int kTrayMenuAbout = 3;
constexpr int kTrayMenuExit = 4;
constexpr int kFeedbackEditId = 2101;
constexpr int kFeedbackSubmitId = 2102;
constexpr int kFeedbackCancelId = 2103;
constexpr int kSeamCoverHeightInactive = 0;
constexpr int kSeamCoverHeightActive = 0;

using SetProcessDpiAwarenessContextProc = BOOL(WINAPI *)(DPI_AWARENESS_CONTEXT);
using InjectExplorerTAPProc = HRESULT (*)(HWND window, REFIID riid, void **ppv);

struct ITaskbarAppearanceService : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetTaskbarAppearance(HWND taskbar, int brush, UINT color) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetTaskbarBlur(HWND taskbar, UINT color, FLOAT blurAmount) = 0;
    virtual HRESULT STDMETHODCALLTYPE ReturnTaskbarToDefaultAppearance(HWND taskbar) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetTaskbarBorderVisibility(HWND taskbar, BOOL visible) = 0;
    virtual HRESULT STDMETHODCALLTYPE RestoreAllTaskbarsToDefault() = 0;
    virtual HRESULT STDMETHODCALLTYPE RestoreAllTaskbarsToDefaultWhenProcessDies(DWORD pid) = 0;
    virtual HRESULT STDMETHODCALLTYPE KillExplorerWhenPackageUninstalls(LPCWSTR packageFullName) = 0;
};

constexpr CLSID kClsidTaskbarAppearanceService {
    0x50e9ab23,
    0x97b4,
    0x4dba,
    { 0x8f, 0x44, 0x5c, 0xd3, 0x42, 0xf3, 0x0b, 0x78 },
};

constexpr IID kIidTaskbarAppearanceService {
    0x5bcf9150,
    0xc28a,
    0x4ef2,
    { 0x91, 0x3c, 0x4c, 0x3e, 0xa2, 0xf5, 0xea, 0xd0 },
};

struct Color {
    uint8_t r = 32;
    uint8_t g = 32;
    uint8_t b = 32;
};

struct Taskbar {
    HWND hwnd = nullptr;
    HMONITOR monitor = nullptr;
    RECT rect {};
    std::vector<HWND> paintTargets;
    bool top = false;
    bool tinted = false;
    Color current {};
    Color from {};
    Color target {};
    DWORD animStart = 0;
    DWORD animDuration = kAnimDurationMs;
    bool animating = false;
    DWORD lastAppliedAt = 0;
    HWND targetWindow = nullptr;
    HWND lastMaximizedWindow = nullptr;
    DWORD lastMaximizedSeen = 0;
    bool restorePending = false;
    DWORD restorePendingAt = 0;
    DWORD restoreReassertUntil = 0;
    DWORD lastRestoreReassertAt = 0;
    Color rawTarget {};
    Color pendingSettingsTarget {};
    Color earlyProbeTarget {};
    int correctionR = 0;
    int correctionG = 0;
    int correctionB = 0;
    DWORD lastCalibration = 0;
    DWORD pendingSettingsTargetAt = 0;
    bool pendingSettingsTargetValid = false;
    DWORD earlyProbeTargetAt = 0;
    HWND earlyProbeWindow = nullptr;
    bool earlyProbeTargetValid = false;
    HWND seamCover = nullptr;
    int seamCoverHeight = kSeamCoverHeightInactive;
};

HWND g_window = nullptr;
HINSTANCE g_instance = nullptr;
UINT g_taskbarCreatedMessage = 0;
HWINEVENTHOOK g_foregroundHook = nullptr;
HWINEVENTHOOK g_locationHook = nullptr;
HWINEVENTHOOK g_minimizeHook = nullptr;
HWINEVENTHOOK g_moveSizeHook = nullptr;
HHOOK g_mouseHook = nullptr;
HWND g_moveSizeWindow = nullptr;
bool g_moveSizeTransparentMode = false;
HWND g_minimizingWindow = nullptr;
DWORD g_taskbarInteractionUntil = 0;
std::map<HWND, Taskbar> g_taskbars;
HANDLE g_singleInstance = nullptr;
std::filesystem::path g_logPath;
std::filesystem::path g_statePath;
std::filesystem::path g_shellThemeStatePath;
UINT g_ttbApplyColorPreviewMessage = 0;
UINT g_ttbForceRefreshMessage = 0;
HWND g_ttbWindow = nullptr;
bool g_comInitialized = false;
HMODULE g_explorerTapDll = nullptr;
InjectExplorerTAPProc g_injectExplorerTap = nullptr;
ITaskbarAppearanceService *g_ttbAppearanceService = nullptr;
std::filesystem::path g_ttbSettingsPath;
std::filesystem::path g_ttbBackupPath;
std::string g_ttbOriginalMaximizedSection;
bool g_ttbMaximizedAppearanceCaptured = false;
std::string g_ttbOriginalDesktopSection;
bool g_ttbDesktopAppearanceCaptured = false;
bool g_ttbDesktopTakeoverActive = false;
Color g_ttbDesktopTakeoverColor {};
Color g_ttbMaximizedSyncColor {};
bool g_ttbMaximizedSynced = false;
double g_colorBrightness = 1.0;
double g_colorSaturation = 1.0;
int g_colorOffset = 0;
bool g_shellThemeCaptured = false;
DWORD g_originalSystemUsesLightTheme = 0;
DWORD g_lastSystemUsesLightTheme = 0xFFFFFFFF;
DWORD g_lastShellThemeSwitchAt = 0;
DWORD g_shellThemeSettlingUntil = 0;
bool g_shellThemeBroadcasting = false;
bool g_taskbarStateUpdating = false;
std::atomic_bool g_updateCheckRunning = false;
ULONG_PTR g_gdiplusToken = 0;
bool g_gdiplusStarted = false;

struct ScopedTaskbarStateUpdate {
    bool previous = false;

    ScopedTaskbarStateUpdate() : previous(g_taskbarStateUpdating) {
        g_taskbarStateUpdating = true;
    }

    ~ScopedTaskbarStateUpdate() {
        g_taskbarStateUpdating = previous;
    }
};

bool LoadExplorerTapBackend();
bool EnsureExplorerTapService(HWND taskbar);
bool IsTranslucentTbActive();
bool IsUsableForegroundWindow(HWND hwnd);
void DestroySeamCover(Taskbar &tb);
void ApplyTaskbarIconTheme(Taskbar &tb, const Color *color);
void ApplyTaskbarColor(Taskbar &tb, const Color &color);
void ReassertTintedTaskbars();
bool SyncTtbMaximizedAppearance(const Taskbar &tb, const Color &color);
void RestoreTtbManagedAppearancesOnce();
void RestoreShellThemeIfNoTintedTaskbars();
void ForceRestoreShellThemeNow(const char *reason);
void ShowDonation();
void ShowFeedback(HWND owner);
bool EnsureGdiPlus();
Gdiplus::Bitmap *LoadPngBitmapResource(int resourceId);

std::string Narrow(const std::wstring &value) {
    if (value.empty()) {
        return {};
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring Widen(const std::string &value) {
    if (value.empty()) {
        return {};
    }
    int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), -1, nullptr, 0);
    if (size <= 1) {
        return {};
    }
    std::wstring out(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), -1, out.data(), size);
    return out;
}

std::wstring PercentEncodeUtf8(const std::wstring &value) {
    const std::string utf8 = Narrow(value);
    constexpr wchar_t hex[] = L"0123456789ABCDEF";
    std::wstring out;
    out.reserve(utf8.size() * 3);
    for (unsigned char ch : utf8) {
        const bool keep =
            std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~';
        if (keep) {
            out.push_back(static_cast<wchar_t>(ch));
        } else {
            out.push_back(L'%');
            out.push_back(hex[(ch >> 4) & 0x0F]);
            out.push_back(hex[ch & 0x0F]);
        }
    }
    return out;
}

bool HasVisibleText(const std::wstring &value) {
    return std::any_of(value.begin(), value.end(), [](wchar_t ch) {
        return !std::iswspace(static_cast<wint_t>(ch));
    });
}

void Log(const std::string &line) {
    if (g_logPath.empty()) {
        return;
    }
    std::ofstream file(g_logPath, std::ios::app);
    if (!file) {
        return;
    }
    SYSTEMTIME st {};
    GetLocalTime(&st);
    file << st.wHour << ':' << st.wMinute << ':' << st.wSecond << '.' << st.wMilliseconds << ' ' << line << '\n';
}

bool HasPassedPreflightOnce() {
    if (g_statePath.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::exists(g_statePath, ec);
}

void MarkPreflightPassed() {
    if (g_statePath.empty()) {
        return;
    }
    std::ofstream file(g_statePath, std::ios::binary | std::ios::trunc);
    if (!file) {
        Log("failed to write preflight state path=" + Narrow(g_statePath.wstring()));
        return;
    }
    file << "preflight-ok-v1\n";
}

std::filesystem::path ExecutableDirectory() {
    wchar_t path[MAX_PATH] {};
    const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return {};
    }
    return std::filesystem::path(path).parent_path();
}

std::filesystem::path LocalAppDataDirectory() {
    wchar_t localAppData[MAX_PATH] {};
    const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return {};
    }
    return std::filesystem::path(localAppData);
}

bool OpenShellTarget(const std::wstring &target, const wchar_t *verb = L"open") {
    HINSTANCE result = ShellExecuteW(nullptr, verb, target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<intptr_t>(result) > 32;
}

int Width(const RECT &rc) {
    return rc.right - rc.left;
}

int Height(const RECT &rc) {
    return rc.bottom - rc.top;
}

int ClampInt(int value, int lo, int hi) {
    return std::max(lo, std::min(hi, value));
}

uint8_t ClampByte(double value) {
    return static_cast<uint8_t>(ClampInt(static_cast<int>(std::lround(value)), 0, 255));
}

double Luminance(const Color &c) {
    const auto channel = [](double value) {
        value /= 255.0;
        return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(c.r) + 0.7152 * channel(c.g) + 0.0722 * channel(c.b);
}

Color Lerp(const Color &a, const Color &b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    return {
        ClampByte(a.r + (b.r - a.r) * t),
        ClampByte(a.g + (b.g - a.g) * t),
        ClampByte(a.b + (b.b - a.b) * t),
    };
}

Color CalibrateColor(const Color &color) {
    const double gray = static_cast<double>(color.r) * 0.2126 +
                        static_cast<double>(color.g) * 0.7152 +
                        static_cast<double>(color.b) * 0.0722;
    const auto channel = [&](uint8_t value) {
        const double saturated = gray + (static_cast<double>(value) - gray) * g_colorSaturation;
        return ClampByte(saturated * g_colorBrightness + g_colorOffset);
    };
    return { channel(color.r), channel(color.g), channel(color.b) };
}

int ColorDistance2(const Color &a, const Color &b) {
    const int dr = static_cast<int>(a.r) - static_cast<int>(b.r);
    const int dg = static_cast<int>(a.g) - static_cast<int>(b.g);
    const int db = static_cast<int>(a.b) - static_cast<int>(b.b);
    return dr * dr + dg * dg + db * db;
}

uint8_t MedianChannel(std::vector<uint8_t> values) {
    if (values.empty()) {
        return 32;
    }
    const size_t mid = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + mid, values.end());
    return values[mid];
}

Color MedianColor(const std::vector<Color> &values) {
    std::vector<uint8_t> r;
    std::vector<uint8_t> g;
    std::vector<uint8_t> b;
    r.reserve(values.size());
    g.reserve(values.size());
    b.reserve(values.size());
    for (const auto &c : values) {
        r.push_back(c.r);
        g.push_back(c.g);
        b.push_back(c.b);
    }
    return { MedianChannel(r), MedianChannel(g), MedianChannel(b) };
}

DWORD ToRgba(const Color &c, uint8_t alpha = 0xFF) {
    return (static_cast<DWORD>(c.r) << 24) |
           (static_cast<DWORD>(c.g) << 16) |
           (static_cast<DWORD>(c.b) << 8) |
           static_cast<DWORD>(alpha);
}

std::string Hex32(uint32_t value) {
    char buffer[16] {};
    std::snprintf(buffer, sizeof(buffer), "0x%08X", value);
    return buffer;
}

std::string HexHr(HRESULT hr) {
    return Hex32(static_cast<uint32_t>(hr));
}

bool IsWindowCloaked(HWND hwnd) {
    BOOL cloaked = FALSE;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked;
}

bool IsShellWindow(HWND hwnd) {
    wchar_t cls[128] {};
    GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls)));
    const std::wstring name(cls);
    return name == L"Shell_TrayWnd" ||
           name == L"Shell_SecondaryTrayWnd" ||
           name == L"Progman" ||
           name == L"WorkerW";
}

bool IsWindowMinimizedState(HWND hwnd);
bool IsWindowOffscreen(HWND hwnd);

bool IsTaskbarTransientWindow(HWND hwnd) {
    if (!hwnd) {
        return false;
    }
    wchar_t className[128] {};
    GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
    const std::wstring cls(className);
    return cls == L"Shell_TrayWnd" ||
           cls == L"Shell_SecondaryTrayWnd" ||
           cls == L"TopLevelWindowForOverflowXamlIsland" ||
           cls == L"XamlExplorerHostIslandWindow" ||
           cls == L"ForegroundStaging";
}

bool IsDesktopForeground(HWND hwnd) {
    if (!hwnd) {
        return true;
    }
    wchar_t className[128] {};
    GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
    const std::wstring cls(className);
    return cls == L"Progman" || cls == L"WorkerW";
}

bool IsShowDesktopLikeForeground(HWND hwnd) {
    if (IsDesktopForeground(hwnd)) {
        return true;
    }
    if (!hwnd || IsShellWindow(hwnd) || IsTaskbarTransientWindow(hwnd)) {
        return false;
    }
    return IsWindowOffscreen(hwnd) || IsWindowMinimizedState(hwnd);
}

bool IsShellInteractionActive(HWND hwnd) {
    return IsTaskbarTransientWindow(hwnd);
}

bool IsMoveSizeHoldForTaskbar(const Taskbar &tb) {
    (void)tb;
    return g_moveSizeTransparentMode && g_moveSizeWindow && IsWindow(g_moveSizeWindow);
}

bool IsTaskbarInteractionProtected() {
    return static_cast<LONG>(g_taskbarInteractionUntil - GetTickCount()) > 0;
}

bool IsWindowInMinimizeTransition(HWND hwnd) {
    return hwnd && g_minimizingWindow && hwnd == g_minimizingWindow;
}

void ExtendTaskbarInteractionProtection() {
    const DWORD deadline = GetTickCount() + kTaskbarInteractionSettleMs;
    if (!IsTaskbarInteractionProtected() ||
        static_cast<LONG>(deadline - g_taskbarInteractionUntil) > 0) {
        g_taskbarInteractionUntil = deadline;
    }
    if (g_window) {
        PostMessageW(g_window, kReassertMessage, 0, 0);
        SetTimer(g_window, kAnimTimer, kAnimIntervalMs, nullptr);
        PostMessageW(g_window, kRefreshMessage, 0, 0);
    }
}

bool IsPointInsideTaskbar(POINT point) {
    for (const auto &[_, tb] : g_taskbars) {
        if (tb.top && IsWindow(tb.hwnd) && PtInRect(&tb.rect, point)) {
            return true;
        }
    }
    return false;
}

LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION &&
        (wparam == WM_LBUTTONDOWN || wparam == WM_RBUTTONDOWN || wparam == WM_MBUTTONDOWN)) {
        const auto *event = reinterpret_cast<const MSLLHOOKSTRUCT *>(lparam);
        if (event && IsPointInsideTaskbar(event->pt)) {
            ExtendTaskbarInteractionProtection();
            Log("[TASKBAR] pointer interaction protection extended");
        }
    }
    return CallNextHookEx(g_mouseHook, code, wparam, lparam);
}

std::wstring WindowClassName(HWND hwnd) {
    wchar_t cls[128] {};
    GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls)));
    return cls;
}

std::wstring WindowTitle(HWND hwnd) {
    wchar_t title[256] {};
    GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
    return title;
}

bool IsWindowsSettingsWindow(HWND hwnd) {
    if (WindowClassName(hwnd) != L"ApplicationFrameWindow") {
        return false;
    }
    const std::wstring title = WindowTitle(hwnd);
    return title.find(L"设置") != std::wstring::npos ||
           title.find(L"Settings") != std::wstring::npos;
}

// A window counts as a maximized candidate regardless of transient foreground
// state. Cloaking is deliberately NOT checked here: during tray clicks, task
// switching or Start/Task View animations the background maximized window may be
// briefly cloaked by DWM, but it still "exists and is maximized", so the taskbar
// must keep its immersive color. However, we DO check IsIconic: minimized windows
// must not be treated as maximized targets (they're minimized to taskbar, not visible).
bool IsWindowMinimizedState(HWND hwnd) {
    if (!hwnd || IsIconic(hwnd)) {
        return true;
    }
    WINDOWPLACEMENT placement {};
    placement.length = sizeof(placement);
    if (!GetWindowPlacement(hwnd, &placement)) {
        return false;
    }
    return placement.showCmd == SW_SHOWMINIMIZED || placement.showCmd == SW_MINIMIZE;
}

bool IsWindowOffscreen(HWND hwnd) {
    RECT rc {};
    if (!GetWindowRect(hwnd, &rc)) {
        return true;
    }
    return rc.right <= -30000 || rc.bottom <= -30000 || rc.left >= 30000 || rc.top >= 30000;
}

bool CachedMaximizedTargetGone(const Taskbar &tb) {
    return !tb.lastMaximizedWindow ||
           !IsWindow(tb.lastMaximizedWindow) ||
           IsWindowMinimizedState(tb.lastMaximizedWindow) ||
           !IsWindowVisible(tb.lastMaximizedWindow) ||
           IsWindowOffscreen(tb.lastMaximizedWindow) ||
           !IsZoomed(tb.lastMaximizedWindow);
}

bool CachedMaximizedWindowBehindForeground(const Taskbar &tb, HWND foreground) {
    if (!foreground || IsZoomed(foreground) || IsShellWindow(foreground) ||
        IsDesktopForeground(foreground) || IsTaskbarTransientWindow(foreground)) {
        return false;
    }
    if (!tb.lastMaximizedWindow || !IsWindow(tb.lastMaximizedWindow) ||
        IsWindowMinimizedState(tb.lastMaximizedWindow) ||
        IsWindowOffscreen(tb.lastMaximizedWindow) ||
        !IsZoomed(tb.lastMaximizedWindow)) {
        return false;
    }
    if (MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST) != tb.monitor ||
        MonitorFromWindow(tb.lastMaximizedWindow, MONITOR_DEFAULTTONEAREST) != tb.monitor) {
        return false;
    }
    return true;
}

bool IsCandidateMaximizedWindow(HWND hwnd) {
    if (!hwnd || hwnd == g_window || IsShellWindow(hwnd) || !IsWindowVisible(hwnd) || IsWindowMinimizedState(hwnd)) {
        return false;
    }
    RECT rc {};
    if (!GetWindowRect(hwnd, &rc)) {
        return false;
    }
    const int w = Width(rc);
    const int h = Height(rc);
    // Filter off-screen cloaked helper windows (Chrome creates -32000,-32000 windows)
    if (rc.left < -30000 || rc.top < -30000 || w <= 160 || h <= 160) {
        return false;
    }
    // Cloaked windows (other virtual desktops, suspended UWP such as the
    // Settings app, DWM-transition helpers) are ghost targets and must never
    // tint the taskbar - regardless of whether the foreground is the desktop,
    // a regular window, or a shell transient window. The only exception is the
    // foreground itself (it cannot be cloaked while active, but be defensive).
    // The DWM brief-cloak race during task switching is absorbed separately by
    // the StableMaximizedWindow hold-cache path, which tolerates a cloaked
    // cached target while a shell transition owns the foreground.
    const HWND foregroundForCloak = GetForegroundWindow();
    if (IsWindowCloaked(hwnd) && hwnd != foregroundForCloak) {
        return false;
    }
    return true;
}

bool IsUsableForegroundWindow(HWND hwnd) {
    if (!hwnd || hwnd == g_window || IsShellWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd) || IsWindowCloaked(hwnd)) {
        return false;
    }
    RECT rc {};
    if (!GetWindowRect(hwnd, &rc)) {
        return false;
    }
    return Width(rc) > 160 && Height(rc) > 160;
}

struct MaximizedWindowSearch {
    HMONITOR monitor = nullptr;
    HWND result = nullptr;
};

BOOL CALLBACK FindMaximizedWindowProc(HWND hwnd, LPARAM lparam) {
    auto *search = reinterpret_cast<MaximizedWindowSearch *>(lparam);
    if (!IsCandidateMaximizedWindow(hwnd) || !IsZoomed(hwnd)) {
        return TRUE;
    }
    if (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) != search->monitor) {
        return TRUE;
    }
    
    // Log the first maximized window found on each scan
    static std::map<HMONITOR, HWND> lastFoundByMonitor;
    if (!search->result && lastFoundByMonitor[search->monitor] != hwnd) {
        std::ostringstream ss;
        ss << "[FIND] maximized hwnd=0x" << std::hex << reinterpret_cast<uintptr_t>(hwnd)
           << std::dec << " class=" << Narrow(WindowClassName(hwnd))
           << " title=" << Narrow(WindowTitle(hwnd));
        Log(ss.str());
        lastFoundByMonitor[search->monitor] = hwnd;
    }
    
    search->result = hwnd;
    return FALSE;
}

HWND FindTopMaximizedWindow(HMONITOR monitor) {
    MaximizedWindowSearch search { monitor, nullptr };
    EnumWindows(FindMaximizedWindowProc, reinterpret_cast<LPARAM>(&search));
    return search.result;
}

HWND StableMaximizedWindow(Taskbar &tb) {
    if (!tb.top) {
        return nullptr;
    }

    const HWND current = FindTopMaximizedWindow(tb.monitor);
    if (current) {
        tb.lastMaximizedWindow = current;
        tb.lastMaximizedSeen = GetTickCount();
        return current;
    }

    // Hold keeps last maximized window only while it's still zoomed and not iconic.
    // This absorbs EnumWindows timing races and brief cloak/uncloak during app switches,
    // but immediately releases on minimize (IsIconic) or restore (not IsZoomed).
    if (tb.lastMaximizedWindow && IsWindow(tb.lastMaximizedWindow)) {
        const bool minimized = IsWindowMinimizedState(tb.lastMaximizedWindow) ||
                             !IsWindowVisible(tb.lastMaximizedWindow) ||
                             IsWindowOffscreen(tb.lastMaximizedWindow);
        const bool zoomed = IsZoomed(tb.lastMaximizedWindow);
        const bool cloaked = IsWindowCloaked(tb.lastMaximizedWindow);
        const bool transientForeground = IsTaskbarTransientWindow(GetForegroundWindow());
        const DWORD age = GetTickCount() - tb.lastMaximizedSeen;
        
        // Detailed hold decision logging
        static std::map<HWND, DWORD> lastHoldLogMs;
        if (age < kMaximizedTargetHoldMs && (!lastHoldLogMs.contains(tb.hwnd) || age - lastHoldLogMs[tb.hwnd] > 200)) {
            std::ostringstream ss;
            ss << "[HOLD] cached=0x" << std::hex << reinterpret_cast<uintptr_t>(tb.lastMaximizedWindow)
               << std::dec << " minimized=" << minimized << " cloaked=" << cloaked
               << " zoomed=" << zoomed << " transient=" << transientForeground
               << " age=" << age << "ms class=" << Narrow(WindowClassName(tb.lastMaximizedWindow));
            Log(ss.str());
            lastHoldLogMs[tb.hwnd] = age;
        }
        
        // If minimized: release immediately regardless of hold period
        if (minimized) {
            Log("[HOLD] released: window is minimized or offscreen");
            return nullptr;
        }
        
        // If not zoomed: release immediately
        if (!zoomed) {
            Log("[HOLD] released: window not zoomed");
            return nullptr;
        }

        // Tray overflow and task-switch UI may cloak the real maximized window.
        // Keep only the cached target while one of those shell transition windows
        // owns the foreground; never select an unrelated cloaked window.
        if (cloaked) {
            if (transientForeground) {
                return tb.lastMaximizedWindow;
            }
            Log("[HOLD] released: cached window cloaked outside shell transition");
            return nullptr;
        }
        
        // Still zoomed and not iconic: hold for kMaximizedTargetHoldMs
        if (age <= kMaximizedTargetHoldMs) {
            return tb.lastMaximizedWindow;
        }
        
        Log("[HOLD] released: hold period expired");
    }
    return nullptr;
}

BOOL CALLBACK EnumTaskbarProc(HWND hwnd, LPARAM) {
    const std::wstring name = WindowClassName(hwnd);
    if (name != L"Shell_TrayWnd" && name != L"Shell_SecondaryTrayWnd") {
        return TRUE;
    }

    auto registerTaskbar = [](HWND taskbar) {
        if (!IsWindow(taskbar)) {
            return;
        }

        RECT rect {};
        if (!GetWindowRect(taskbar, &rect) || Width(rect) <= 0 || Height(rect) <= 0) {
            return;
        }

        const HMONITOR monitor = MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi {};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(monitor, &mi)) {
            return;
        }

        const int monitorWidth = Width(mi.rcMonitor);
        const bool top = std::abs(rect.top - mi.rcMonitor.top) <= 2 &&
                         Width(rect) >= monitorWidth / 2 &&
                         Height(rect) <= 140;

        Taskbar tb {};
        if (auto old = g_taskbars.find(taskbar); old != g_taskbars.end()) {
            tb = old->second;
        }
        tb.hwnd = taskbar;
        tb.monitor = monitor;
        tb.rect = rect;
        tb.top = top;
        tb.paintTargets.clear();
        tb.paintTargets.push_back(taskbar);
        g_taskbars[taskbar] = tb;
    };

    registerTaskbar(hwnd);
    return TRUE;
}

void AddFindWindowTaskbarFallbacks() {
    auto registerTaskbar = [](HWND taskbar) {
        if (!IsWindow(taskbar) || g_taskbars.contains(taskbar)) {
            return;
        }

        RECT rect {};
        if (!GetWindowRect(taskbar, &rect) || Width(rect) <= 0 || Height(rect) <= 0) {
            return;
        }

        const HMONITOR monitor = MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi {};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(monitor, &mi)) {
            return;
        }

        const int monitorWidth = Width(mi.rcMonitor);
        const bool top = std::abs(rect.top - mi.rcMonitor.top) <= 2 &&
                         Width(rect) >= monitorWidth / 2 &&
                         Height(rect) <= 140;

        Taskbar tb {};
        tb.hwnd = taskbar;
        tb.monitor = monitor;
        tb.rect = rect;
        tb.top = top;
        tb.paintTargets.push_back(taskbar);
        g_taskbars[taskbar] = tb;
        Log("taskbar fallback registered hwnd=0x" + Hex32(static_cast<DWORD>(reinterpret_cast<uintptr_t>(taskbar))));
    };

    registerTaskbar(FindWindowW(L"Shell_TrayWnd", nullptr));
    for (HWND secondary = nullptr;
         (secondary = FindWindowExW(nullptr, secondary, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr;) {
        registerTaskbar(secondary);
    }
}

bool IsTranslucentTbProcessWindow(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) {
        return false;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return false;
    }

    wchar_t imagePath[MAX_PATH] {};
    DWORD size = static_cast<DWORD>(std::size(imagePath));
    const BOOL ok = QueryFullProcessImageNameW(process, 0, imagePath, &size);
    CloseHandle(process);
    if (!ok || size == 0) {
        return false;
    }

    std::wstring filename = std::filesystem::path(imagePath).filename().wstring();
    std::transform(filename.begin(), filename.end(), filename.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return filename == L"translucenttb.exe";
}

BOOL CALLBACK EnumTranslucentTbProc(HWND hwnd, LPARAM lparam) {
    if (WindowClassName(hwnd) == L"TrayWindow" &&
        WindowTitle(hwnd) == L"TranslucentTB" &&
        IsTranslucentTbProcessWindow(hwnd)) {
        *reinterpret_cast<HWND *>(lparam) = hwnd;
        return FALSE;
    }
    return TRUE;
}

HWND FindTranslucentTbWindow() {
    HWND hwnd = nullptr;
    EnumWindows(EnumTranslucentTbProc, reinterpret_cast<LPARAM>(&hwnd));
    return hwnd;
}

std::filesystem::path FindTranslucentTbSettingsPath() {
    wchar_t localAppData[MAX_PATH] {};
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return {};
    }

    const std::filesystem::path packages = std::filesystem::path(localAppData) / L"Packages";
    std::error_code ec;
    if (!std::filesystem::exists(packages, ec)) {
        return {};
    }

    for (const auto &entry : std::filesystem::directory_iterator(packages, ec)) {
        if (ec || !entry.is_directory()) {
            continue;
        }

        std::wstring name = entry.path().filename().wstring();
        std::transform(name.begin(), name.end(), name.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });
        if (name.find(L"translucenttb") == std::wstring::npos) {
            continue;
        }

        const auto settings = entry.path() / L"RoamingState" / L"settings.json";
        if (std::filesystem::exists(settings, ec)) {
            return settings;
        }
    }

    return {};
}

bool ReadWholeFile(const std::filesystem::path &path, std::string &content) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool WriteWholeFile(const std::filesystem::path &path, const std::string &content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(out);
}

bool FindJsonObjectRange(const std::string &content, const std::string &marker, size_t &open, size_t &close) {
    const size_t markerPos = content.find(marker);
    if (markerPos == std::string::npos) {
        return false;
    }

    open = content.find('{', markerPos + marker.size());
    if (open == std::string::npos) {
        return false;
    }

    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (size_t i = open; i < content.size(); ++i) {
        const char ch = content[i];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }

        if (ch == '"') {
            inString = true;
        } else if (ch == '{') {
            depth += 1;
        } else if (ch == '}') {
            depth -= 1;
            if (depth == 0) {
                close = i;
                return true;
            }
        }
    }

    return false;
}

bool EnsureTranslucentTbSettingsPath() {
    if (!g_ttbSettingsPath.empty()) {
        return true;
    }

    g_ttbSettingsPath = FindTranslucentTbSettingsPath();
    if (g_ttbSettingsPath.empty()) {
        Log("TTB settings.json not found");
        return false;
    }
    g_ttbBackupPath = g_ttbSettingsPath.parent_path() / L"settings.immersive-takeover-backup.json";
    return true;
}

void ForceRefreshTranslucentTb() {
    if (!g_ttbWindow || !IsWindow(g_ttbWindow)) {
        g_ttbWindow = FindTranslucentTbWindow();
    }
    if (g_ttbForceRefreshMessage && g_ttbWindow && IsWindow(g_ttbWindow)) {
        PostMessageW(g_ttbWindow, g_ttbForceRefreshMessage, 0, 0);
    }
}

bool CaptureTtbSection(const std::string &content, const std::string &marker, std::string &sectionOut) {
    size_t open = 0;
    size_t close = 0;
    if (!FindJsonObjectRange(content, marker, open, close)) {
        return false;
    }
    sectionOut = content.substr(open, close - open + 1);
    return true;
}

bool ReplaceTtbSection(std::string &content, const std::string &marker, const std::string &section) {
    size_t open = 0;
    size_t close = 0;
    if (!FindJsonObjectRange(content, marker, open, close)) {
        return false;
    }
    content.replace(open, close - open + 1, section);
    return true;
}

bool ReplaceJsonStringProperty(std::string &object, const std::string &key, const std::string &value) {
    const size_t keyPos = object.find(key);
    if (keyPos == std::string::npos) {
        return false;
    }
    const size_t colon = object.find(':', keyPos + key.size());
    const size_t openQuote = object.find('"', colon + 1);
    const size_t closeQuote = object.find('"', openQuote + 1);
    if (colon == std::string::npos || openQuote == std::string::npos || closeQuote == std::string::npos) {
        return false;
    }
    object.replace(openQuote + 1, closeQuote - openQuote - 1, value);
    return true;
}

std::string TtbColorString(const Color &color) {
    char buffer[16] {};
    std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02XFF", color.r, color.g, color.b);
    return buffer;
}

void RestoreTranslucentTbMaximizedAppearance() {
    if (!EnsureTranslucentTbSettingsPath()) {
        return;
    }

    if ((!g_ttbMaximizedAppearanceCaptured || g_ttbOriginalMaximizedSection.empty() ||
         !g_ttbDesktopAppearanceCaptured || g_ttbOriginalDesktopSection.empty()) &&
        !g_ttbBackupPath.empty()) {
        std::string backup;
        if (ReadWholeFile(g_ttbBackupPath, backup)) {
            CaptureTtbSection(backup, "\"maximized_window_appearance\"", g_ttbOriginalMaximizedSection);
            CaptureTtbSection(backup, "\"desktop_appearance\"", g_ttbOriginalDesktopSection);
            g_ttbMaximizedAppearanceCaptured = !g_ttbOriginalMaximizedSection.empty();
            g_ttbDesktopAppearanceCaptured = !g_ttbOriginalDesktopSection.empty();
        }
    }

    if ((!g_ttbMaximizedAppearanceCaptured || g_ttbOriginalMaximizedSection.empty()) &&
        (!g_ttbDesktopAppearanceCaptured || g_ttbOriginalDesktopSection.empty())) {
        return;
    }

    std::string content;
    if (!ReadWholeFile(g_ttbSettingsPath, content)) {
        Log("Failed to read TTB settings for restore path=" + Narrow(g_ttbSettingsPath.wstring()));
        return;
    }

    if (g_ttbMaximizedAppearanceCaptured && !g_ttbOriginalMaximizedSection.empty()) {
        ReplaceTtbSection(content, "\"maximized_window_appearance\"", g_ttbOriginalMaximizedSection);
    }
    if (g_ttbDesktopAppearanceCaptured && !g_ttbOriginalDesktopSection.empty()) {
        ReplaceTtbSection(content, "\"desktop_appearance\"", g_ttbOriginalDesktopSection);
    }

    if (WriteWholeFile(g_ttbSettingsPath, content)) {
        std::error_code ec;
        if (!g_ttbBackupPath.empty()) {
            std::filesystem::remove(g_ttbBackupPath, ec);
        }
        Log("TTB managed appearances restored");
        ForceRefreshTranslucentTb();
        g_ttbOriginalMaximizedSection.clear();
        g_ttbMaximizedAppearanceCaptured = false;
        g_ttbOriginalDesktopSection.clear();
        g_ttbDesktopAppearanceCaptured = false;
        g_ttbDesktopTakeoverActive = false;
        g_ttbMaximizedSynced = false;
        g_ttbMaximizedSyncColor = {};
    }
}

bool ApplyTranslucentTbDesktopTakeover(const Color &color) {
    (void)color;
    // Keep TTB's original desktop appearance untouched during normal runtime.
    // The desktop section is global and an async settings refresh here can make
    // the old opaque brush visible for one frame when the last window minimizes.
    // ExplorerTAP owns the visible maximized taskbar; TTB's original clear
    // desktop appearance remains the fallback when the user returns to desktop.
    return false;
}

void RestoreTranslucentTbDesktopAppearance() {
    if (!g_ttbDesktopTakeoverActive || !EnsureTranslucentTbSettingsPath()) {
        return;
    }
    if ((!g_ttbDesktopAppearanceCaptured || g_ttbOriginalDesktopSection.empty()) &&
        !g_ttbBackupPath.empty()) {
        std::string backup;
        if (ReadWholeFile(g_ttbBackupPath, backup)) {
            CaptureTtbSection(backup, "\"desktop_appearance\"", g_ttbOriginalDesktopSection);
            g_ttbDesktopAppearanceCaptured = !g_ttbOriginalDesktopSection.empty();
        }
    }
    if (!g_ttbDesktopAppearanceCaptured || g_ttbOriginalDesktopSection.empty()) {
        return;
    }

    std::string content;
    if (!ReadWholeFile(g_ttbSettingsPath, content)) {
        return;
    }
    ReplaceTtbSection(content, "\"desktop_appearance\"", g_ttbOriginalDesktopSection);
    if (WriteWholeFile(g_ttbSettingsPath, content)) {
        g_ttbDesktopTakeoverActive = false;
        ForceRefreshTranslucentTb();
        Log("TTB desktop baseline restored");
    }
}

bool EnableTranslucentTbMaximizedAppearance() {
    if (!EnsureTranslucentTbSettingsPath()) {
        return false;
    }

    std::string content;
    if (!ReadWholeFile(g_ttbSettingsPath, content)) {
        Log("Failed to read TTB settings for enable path=" + Narrow(g_ttbSettingsPath.wstring()));
        return false;
    }

    size_t open = 0;
    size_t close = 0;
    if (!FindJsonObjectRange(content, "\"maximized_window_appearance\"", open, close)) {
        Log("TTB maximized appearance section not found");
        return false;
    }

    if (!g_ttbMaximizedAppearanceCaptured) {
        if (!g_ttbBackupPath.empty() && ReadWholeFile(g_ttbBackupPath, content)) {
            CaptureTtbSection(content, "\"maximized_window_appearance\"", g_ttbOriginalMaximizedSection);
            ReadWholeFile(g_ttbSettingsPath, content);
        } else {
            g_ttbOriginalMaximizedSection = content.substr(open, close - open + 1);
            if (!g_ttbBackupPath.empty()) {
                WriteWholeFile(g_ttbBackupPath, content);
            }
        }
        g_ttbMaximizedAppearanceCaptured = !g_ttbOriginalMaximizedSection.empty();
    }

    std::string section = content.substr(open, close - open + 1);
    const size_t enabledKey = section.find("\"enabled\"");
    if (enabledKey == std::string::npos) {
        Log("TTB maximized appearance enabled key not found");
        return false;
    }
    const size_t valueStart = section.find_first_not_of(" \t\r\n", section.find(':', enabledKey) + 1);
    if (valueStart == std::string::npos) {
        return false;
    }
    const bool alreadyEnabled = section.compare(valueStart, 4, "true") == 0;
    if (!alreadyEnabled) {
        section.replace(valueStart, 5, "true");
    }
    if (!ReplaceJsonStringProperty(section, "\"accent\"", "opaque")) {
        Log("TTB maximized appearance accent key not found");
        return false;
    }
    ReplaceTtbSection(content, "\"maximized_window_appearance\"", section);
    if (!WriteWholeFile(g_ttbSettingsPath, content)) {
        Log("Failed to enable TTB maximized appearance");
        return false;
    }
    Log("TTB maximized appearance enabled with opaque accent");
    if (!g_ttbAppearanceService) {
        ForceRefreshTranslucentTb();
    }
    return true;
}

bool DisableTranslucentTbMaximizedAppearance() {
    if (!EnsureTranslucentTbSettingsPath()) {
        return false;
    }

    std::string content;
    if (!ReadWholeFile(g_ttbSettingsPath, content)) {
        return false;
    }

    size_t open = 0;
    size_t close = 0;
    if (!FindJsonObjectRange(content, "\"maximized_window_appearance\"", open, close)) {
        return false;
    }

    if (!g_ttbMaximizedAppearanceCaptured) {
        if (!g_ttbBackupPath.empty() && ReadWholeFile(g_ttbBackupPath, content)) {
            CaptureTtbSection(content, "\"maximized_window_appearance\"", g_ttbOriginalMaximizedSection);
            ReadWholeFile(g_ttbSettingsPath, content);
        } else {
            g_ttbOriginalMaximizedSection = content.substr(open, close - open + 1);
            if (!g_ttbBackupPath.empty()) {
                WriteWholeFile(g_ttbBackupPath, content);
            }
        }
        g_ttbMaximizedAppearanceCaptured = !g_ttbOriginalMaximizedSection.empty();
    }

    std::string section = content.substr(open, close - open + 1);
    const size_t enabledKey = section.find("\"enabled\"");
    if (enabledKey == std::string::npos) {
        return false;
    }
    const size_t colon = section.find(':', enabledKey);
    const size_t valueStart = section.find_first_not_of(" \t\r\n", colon + 1);
    if (colon == std::string::npos || valueStart == std::string::npos) {
        return false;
    }
    if (section.compare(valueStart, 5, "false") != 0) {
        section.replace(valueStart, 4, "false");
        ReplaceTtbSection(content, "\"maximized_window_appearance\"", section);
        if (!WriteWholeFile(g_ttbSettingsPath, content)) {
            return false;
        }
        Log("TTB maximized appearance disabled temporarily for ExplorerTAP");
        // TTB must reload settings even when ExplorerTAP COM is live, or its
        // event-driven repaints keep using the stale (enabled) section.
        ForceRefreshTranslucentTb();
    }
    return true;
}

bool ApplyTranslucentTbSettingsColor(const Color &color) {
    if (!EnsureTranslucentTbSettingsPath()) {
        return false;
    }

    std::string content;
    if (!ReadWholeFile(g_ttbSettingsPath, content)) {
        return false;
    }
    size_t open = 0;
    size_t close = 0;
    if (!FindJsonObjectRange(content, "\"maximized_window_appearance\"", open, close)) {
        return false;
    }

    std::string section = content.substr(open, close - open + 1);
    if (!ReplaceJsonStringProperty(section, "\"color\"", TtbColorString(color))) {
        Log("TTB maximized appearance color key not found");
        return false;
    }
    ReplaceTtbSection(content, "\"maximized_window_appearance\"", section);
    if (!WriteWholeFile(g_ttbSettingsPath, content)) {
        return false;
    }
    ForceRefreshTranslucentTb();
    return true;
}

// Color endorsement: TranslucentTB re-applies its own taskbar brush on every
// shell event (foreground switch, move-size start/end, tray clicks). When its
// maximized state is disabled it repaints CLEAR, racing our ExplorerTAP writes
// and showing as transparency flicker. Keep TTB's maximized appearance enabled
// with the same opaque color we render, so TTB's event-driven repaints write
// the identical color and the race becomes invisible.
bool SyncTtbMaximizedAppearance(const Taskbar &tb, const Color &color) {
    // TTB's maximized section is global (one color for all monitors). Endorse
    // the foreground monitor's immersive color; other monitors stay
    // ExplorerTAP-authoritative. Single-monitor systems always qualify.
    if (g_taskbars.size() > 1) {
        const HWND fg = GetForegroundWindow();
        const HMONITOR fgMon = fg ? MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST) : nullptr;
        if (!fgMon || tb.monitor != fgMon) {
            return false;
        }
    }
    if (g_ttbMaximizedSynced && ColorDistance2(g_ttbMaximizedSyncColor, color) < 9) {
        return true;
    }
    if (!EnsureTranslucentTbSettingsPath()) {
        return false;
    }

    std::string content;
    if (!ReadWholeFile(g_ttbSettingsPath, content)) {
        return false;
    }

    size_t open = 0;
    size_t close = 0;
    if (!FindJsonObjectRange(content, "\"maximized_window_appearance\"", open, close)) {
        Log("TTB maximized appearance section not found");
        return false;
    }

    // Back up the original section once so shutdown can restore the user config.
    if (!g_ttbMaximizedAppearanceCaptured) {
        std::string originalSource = content;
        if (!g_ttbBackupPath.empty()) {
            std::string backup;
            if (ReadWholeFile(g_ttbBackupPath, backup)) {
                originalSource = std::move(backup);
            } else {
                WriteWholeFile(g_ttbBackupPath, content);
            }
        }
        CaptureTtbSection(originalSource, "\"maximized_window_appearance\"", g_ttbOriginalMaximizedSection);
        g_ttbMaximizedAppearanceCaptured = !g_ttbOriginalMaximizedSection.empty();
    }

    std::string section = content.substr(open, close - open + 1);

    const size_t enabledKey = section.find("\"enabled\"");
    if (enabledKey != std::string::npos) {
        const size_t valueStart = section.find_first_not_of(" \t\r\n", section.find(':', enabledKey) + 1);
        if (valueStart != std::string::npos && section.compare(valueStart, 4, "true") != 0) {
            section.replace(valueStart, 5, "true");
        }
    }
    if (!ReplaceJsonStringProperty(section, "\"accent\"", "opaque")) {
        Log("TTB maximized appearance accent key not found");
        return false;
    }
    if (!ReplaceJsonStringProperty(section, "\"color\"", TtbColorString(color))) {
        Log("TTB maximized appearance color key not found");
        return false;
    }
    ReplaceTtbSection(content, "\"maximized_window_appearance\"", section);
    if (!WriteWholeFile(g_ttbSettingsPath, content)) {
        return false;
    }

    g_ttbMaximizedSyncColor = color;
    g_ttbMaximizedSynced = true;
    ForceRefreshTranslucentTb();
    Log("TTB maximized appearance synced color=" + TtbColorString(color));
    return true;
}

void DesyncTtbMaximizedAppearance() {
    g_ttbMaximizedSynced = false;
    g_ttbMaximizedSyncColor = {};
    // Hand the maximized state back to TTB's fallback (desktop = clear), so
    // TTB's event repaints write transparency again while we are restored.
    DisableTranslucentTbMaximizedAppearance();
}

// Restore both the maximized (disable) and desktop (clear) sections in a
// single file read+write+refresh. The restore-to-desktop transition otherwise
// pays two full TTB reload round-trips (one for Desync, one for
// RestoreTranslucentTbDesktopAppearance), which is the dominant source of
// "taskbar stays colored after minimizing" latency when the COM channel is
// down and we cannot ReturnTaskbarToDefaultAppearance immediately.
void RestoreTtbManagedAppearancesOnce() {
    g_ttbMaximizedSynced = false;
    g_ttbMaximizedSyncColor = {};
    if (!EnsureTranslucentTbSettingsPath()) {
        return;
    }

    std::string content;
    if (!ReadWholeFile(g_ttbSettingsPath, content)) {
        return;
    }

    bool changed = false;

    // maximized_window_appearance: flip enabled true -> false.
    {
        size_t open = 0;
        size_t close = 0;
        if (FindJsonObjectRange(content, "\"maximized_window_appearance\"", open, close)) {
            std::string section = content.substr(open, close - open + 1);
            const size_t enabledKey = section.find("\"enabled\"");
            if (enabledKey != std::string::npos) {
                const size_t valueStart = section.find_first_not_of(
                    " \t\r\n", section.find(':', enabledKey) + 1);
                if (valueStart != std::string::npos &&
                    section.compare(valueStart, 4, "true") == 0) {
                    // "true" is 4 chars; replace exactly 4. Using 5 here would
                    // eat the trailing comma/whitespace/} and corrupt the JSON.
                    section.replace(valueStart, 4, "false");
                    content.replace(open, close - open + 1, section);
                    changed = true;
                }
            }
        }
    }

    // desktop_appearance: restore the captured original (clear) section.
    if (g_ttbDesktopAppearanceCaptured && !g_ttbOriginalDesktopSection.empty()) {
        if (ReplaceTtbSection(content, "\"desktop_appearance\"", g_ttbOriginalDesktopSection)) {
            changed = true;
            g_ttbDesktopTakeoverActive = false;
        }
    }

    if (changed) {
        WriteWholeFile(g_ttbSettingsPath, content);
        ForceRefreshTranslucentTb();
        Log("TTB managed appearances restored in one pass");
    }
}

// Rec. 709 perceived luminance, 0..1.
float PerceivedLuminance(const Color &c) {
    return 0.2126f * (c.r / 255.0f) +
           0.7152f * (c.g / 255.0f) +
           0.0722f * (c.b / 255.0f);
}

bool ReadSystemUsesLightTheme(DWORD &value) {
    DWORD type = 0;
    DWORD size = sizeof(value);
    const LSTATUS status = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"SystemUsesLightTheme",
        RRF_RT_REG_DWORD,
        &type,
        &value,
        &size);
    return status == ERROR_SUCCESS && type == REG_DWORD;
}

bool WriteSystemUsesLightTheme(DWORD value) {
    HKEY key = nullptr;
    const LSTATUS openStatus = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0,
        nullptr,
        0,
        KEY_SET_VALUE,
        nullptr,
        &key,
        nullptr);
    if (openStatus != ERROR_SUCCESS) {
        Log("shell theme open registry failed status=" + std::to_string(openStatus));
        return false;
    }

    const LSTATUS setStatus = RegSetValueExW(
        key,
        L"SystemUsesLightTheme",
        0,
        REG_DWORD,
        reinterpret_cast<const BYTE *>(&value),
        sizeof(value));
    RegCloseKey(key);
    if (setStatus != ERROR_SUCCESS) {
        Log("shell theme write registry failed status=" + std::to_string(setStatus));
        return false;
    }
    return true;
}

bool ReadStoredShellThemeOriginal(DWORD &value) {
    if (g_shellThemeStatePath.empty()) {
        return false;
    }
    std::ifstream file(g_shellThemeStatePath, std::ios::binary);
    if (!file) {
        return false;
    }
    int stored = -1;
    file >> stored;
    if (stored != 0 && stored != 1) {
        return false;
    }
    value = static_cast<DWORD>(stored);
    return true;
}

void StoreShellThemeOriginal(DWORD value) {
    if (g_shellThemeStatePath.empty()) {
        return;
    }
    std::error_code ec;
    if (std::filesystem::exists(g_shellThemeStatePath, ec)) {
        return;
    }
    std::ofstream file(g_shellThemeStatePath, std::ios::binary | std::ios::trunc);
    if (!file) {
        Log("shell theme original store failed path=" + Narrow(g_shellThemeStatePath.wstring()));
        return;
    }
    file << (value ? 1 : 0) << '\n';
    Log("shell theme original stored SystemUsesLightTheme=" + std::to_string(value ? 1 : 0));
}

void ClearStoredShellThemeOriginal() {
    if (g_shellThemeStatePath.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::remove(g_shellThemeStatePath, ec);
}

void BroadcastShellThemeChange() {
    g_shellThemeBroadcasting = true;
    SendNotifyMessageW(
        HWND_BROADCAST,
        WM_SETTINGCHANGE,
        0,
        reinterpret_cast<LPARAM>(L"ImmersiveColorSet"));
    SendNotifyMessageW(
        HWND_BROADCAST,
        WM_SETTINGCHANGE,
        0,
        reinterpret_cast<LPARAM>(L"WindowsThemeElement"));
    g_shellThemeBroadcasting = false;
}

bool CaptureShellTheme() {
    if (g_shellThemeCaptured) {
        return true;
    }
    DWORD current = 0;
    if (!ReadSystemUsesLightTheme(current)) {
        Log("shell theme capture failed");
        return false;
    }
    DWORD storedOriginal = 0;
    g_originalSystemUsesLightTheme =
        ReadStoredShellThemeOriginal(storedOriginal) ? storedOriginal : current;
    g_lastSystemUsesLightTheme = current;
    g_shellThemeCaptured = true;
    std::ostringstream ss;
    ss << "shell theme captured SystemUsesLightTheme=" << current
       << " original=" << g_originalSystemUsesLightTheme;
    Log(ss.str());
    return true;
}

bool SetShellTheme(
    DWORD value,
    const char *reason,
    bool force = false,
    DWORD cooldownMs = kShellThemeSwitchCooldownMs) {
    if (!CaptureShellTheme()) {
        return false;
    }
    value = value ? 1 : 0;
    DWORD current = g_lastSystemUsesLightTheme;
    ReadSystemUsesLightTheme(current);
    if (current == value) {
        g_lastSystemUsesLightTheme = value;
        return true;
    }

    const DWORD now = GetTickCount();
    if (!force &&
        g_lastShellThemeSwitchAt &&
        now - g_lastShellThemeSwitchAt < cooldownMs) {
        return false;
    }
    if (value != g_originalSystemUsesLightTheme) {
        StoreShellThemeOriginal(g_originalSystemUsesLightTheme);
    }
    if (!WriteSystemUsesLightTheme(value)) {
        return false;
    }

    g_lastShellThemeSwitchAt = now;
    g_shellThemeSettlingUntil = now + kShellThemeSettleDelayMs;
    g_lastSystemUsesLightTheme = value;
    BroadcastShellThemeChange();
    std::ostringstream ss;
    ss << "shell theme set SystemUsesLightTheme=" << value << " reason=" << reason;
    Log(ss.str());
    return true;
}

void UpdateShellThemeForTaskbarColor(const Color &color, bool fast) {
    const float lum = PerceivedLuminance(color);
    if (fast && lum >= 0.76f) {
        SetShellTheme(1, "light-target", true, kShellThemeFastSwitchCooldownMs);
    } else if (fast && lum <= 0.34f) {
        SetShellTheme(0, "dark-target", true, kShellThemeFastSwitchCooldownMs);
    } else if (lum >= 0.67f) {
        SetShellTheme(1, "light-taskbar", false, kShellThemeSwitchCooldownMs);
    } else if (lum <= 0.45f) {
        SetShellTheme(0, "dark-taskbar", false, kShellThemeSwitchCooldownMs);
    }
}

bool IsShellThemeSettlingForColor(const Color &color) {
    if (static_cast<LONG>(g_shellThemeSettlingUntil - GetTickCount()) <= 0) {
        return false;
    }
    const float lum = PerceivedLuminance(color);
    return lum >= 0.67f || lum <= 0.45f;
}

void RestoreOriginalShellTheme() {
    if (!g_shellThemeCaptured) {
        return;
    }
    if (SetShellTheme(g_originalSystemUsesLightTheme, "restore-original", true)) {
        ClearStoredShellThemeOriginal();
    }
}

void ForceRestoreShellThemeNow(const char *reason) {
    DWORD original = 0;
    if (ReadStoredShellThemeOriginal(original)) {
        g_originalSystemUsesLightTheme = original;
        g_shellThemeCaptured = true;
    } else if (g_shellThemeCaptured) {
        original = g_originalSystemUsesLightTheme;
    } else if (!ReadSystemUsesLightTheme(original)) {
        return;
    }

    original = original ? 1 : 0;
    WriteSystemUsesLightTheme(original);
    g_lastSystemUsesLightTheme = original;
    BroadcastShellThemeChange();
    ClearStoredShellThemeOriginal();
    Log(std::string("shell theme force restored SystemUsesLightTheme=") +
        std::to_string(original) + " reason=" + reason);
}

void RestoreShellThemeIfNoTintedTaskbars() {
    for (const auto &[_, tb] : g_taskbars) {
        if (tb.tinted) {
            return;
        }
    }
    RestoreOriginalShellTheme();
}

void RememberEarlyProbeTarget(Taskbar &tb, HWND window, const Color &color) {
    const float lum = PerceivedLuminance(color);
    if (lum < 0.72f && lum > 0.38f) {
        return;
    }
    tb.earlyProbeTarget = color;
    tb.earlyProbeTargetAt = GetTickCount();
    tb.earlyProbeWindow = window;
    tb.earlyProbeTargetValid = true;
}

bool UseEarlyProbeTarget(Taskbar &tb, HWND window, Color &color) {
    if (!tb.earlyProbeTargetValid || tb.earlyProbeWindow != window) {
        return false;
    }
    if (GetTickCount() - tb.earlyProbeTargetAt > 1400) {
        tb.earlyProbeTargetValid = false;
        return false;
    }

    const float probeLum = PerceivedLuminance(tb.earlyProbeTarget);
    const float sampleLum = PerceivedLuminance(color);
    if (probeLum >= 0.72f && sampleLum <= 0.58f) {
        color = tb.earlyProbeTarget;
        return true;
    }
    if (probeLum <= 0.38f && sampleLum >= 0.58f) {
        color = tb.earlyProbeTarget;
        return true;
    }
    return false;
}

// System tray icons (time, battery, network, etc.) are drawn in a fixed theme
// color that Windows picks from the system theme, not from the actual taskbar
// background. With a light immersive color, those white icons become invisible.
// Use DWM's immersive dark mode attribute so the taskbar's XAML subtree
// re-renders in the opposite theme, giving the icons the right contrast for
// our background without touching the color or brightness we just picked.
struct IconThemeSnapshot {
    BOOL dark20 = FALSE;
    BOOL dark19 = FALSE;
    HRESULT hr20 = E_FAIL;
    HRESULT hr19 = E_FAIL;
};

// Pass color == nullptr to restore the DWM dark-mode values captured before
// this tool first adjusted the taskbar/XAML foreground contrast.
void ApplyTaskbarIconTheme(HWND taskbar, const Color *color) {
    if (!IsWindow(taskbar)) {
        return;
    }
    // Win11 uses index 20, Win10 1903-21H2 uses 19. Set both so the call works
    // regardless of build, and log each so we know which one took effect.
    constexpr DWORD kImmersiveDarkWin11 = 20;
    constexpr DWORD kImmersiveDarkWin10 = 19;

    auto applyAttr = [taskbar](DWORD attr, BOOL dark) {
        return DwmSetWindowAttribute(taskbar, attr, &dark, sizeof(dark));
    };

    static std::map<HWND, IconThemeSnapshot> original;
    if (!original.contains(taskbar)) {
        IconThemeSnapshot snap {};
        snap.hr20 = DwmGetWindowAttribute(taskbar, kImmersiveDarkWin11, &snap.dark20, sizeof(snap.dark20));
        snap.hr19 = DwmGetWindowAttribute(taskbar, kImmersiveDarkWin10, &snap.dark19, sizeof(snap.dark19));
        original[taskbar] = snap;
        std::ostringstream ss;
        ss << "[ICON-THEME-DWM] captured hwnd=0x" << std::hex << reinterpret_cast<uintptr_t>(taskbar)
           << std::dec << " dark20=" << snap.dark20 << " hr20=" << HexHr(snap.hr20)
           << " dark19=" << snap.dark19 << " hr19=" << HexHr(snap.hr19);
        Log(ss.str());
    }

    if (!color) {
        const IconThemeSnapshot snap = original[taskbar];
        const HRESULT hr20 = applyAttr(kImmersiveDarkWin11, SUCCEEDED(snap.hr20) ? snap.dark20 : FALSE);
        const HRESULT hr19 = applyAttr(kImmersiveDarkWin10, SUCCEEDED(snap.hr19) ? snap.dark19 : FALSE);
        std::ostringstream ss;
        ss << "[ICON-THEME-DWM] reset hwnd=0x" << std::hex << reinterpret_cast<uintptr_t>(taskbar)
           << " hr20=" << HexHr(hr20) << " hr19=" << HexHr(hr19);
        Log(ss.str());
        return;
    }

    const float lum = PerceivedLuminance(*color);
    const BOOL wantDark = lum <= 0.5f ? TRUE : FALSE;
    const HRESULT hr20 = applyAttr(kImmersiveDarkWin11, wantDark);
    const HRESULT hr19 = applyAttr(kImmersiveDarkWin10, wantDark);

    static std::map<HWND, std::tuple<BOOL, HRESULT, HRESULT>> lastLogged;
    const bool firstLog = !lastLogged.contains(taskbar);
    auto &last = lastLogged[taskbar];
    if (firstLog ||
        std::get<0>(last) != wantDark ||
        std::get<1>(last) != hr20 ||
        std::get<2>(last) != hr19) {
        std::ostringstream ss;
        ss << "[ICON-THEME-DWM] dark=" << (wantDark ? "TRUE" : "FALSE")
           << " hwnd=0x" << std::hex << reinterpret_cast<uintptr_t>(taskbar) << std::dec
           << " lum=" << lum
           << " hr20=" << HexHr(hr20)
           << " hr19=" << HexHr(hr19)
           << " color=(" << static_cast<int>(color->r) << ','
           << static_cast<int>(color->g) << ',' << static_cast<int>(color->b) << ')';
        Log(ss.str());
        last = {wantDark, hr20, hr19};
    }
}

void ApplyTaskbarIconTheme(Taskbar &tb, const Color *color) {
    bool appliedMain = false;
    for (HWND target : tb.paintTargets) {
        if (!IsWindow(target)) {
            continue;
        }
        ApplyTaskbarIconTheme(target, color);
        appliedMain = appliedMain || target == tb.hwnd;
    }
    if (!appliedMain) {
        ApplyTaskbarIconTheme(tb.hwnd, color);
    }
}

bool ApplyTranslucentTbPreview(HWND taskbar, const Color &color, uint8_t alpha = 0xFF) {
    if (!g_ttbApplyColorPreviewMessage) {
        return false;
    }

    if (!g_ttbWindow || !IsWindow(g_ttbWindow)) {
        g_ttbWindow = FindTranslucentTbWindow();
        if (g_ttbWindow) {
            Log("TranslucentTB backend available");
        }
    }

    if (!g_ttbWindow) {
        return false;
    }

    DWORD_PTR result = 1;
    const bool sent = SendMessageTimeoutW(
        g_ttbWindow,
        g_ttbApplyColorPreviewMessage,
        2,
        static_cast<LPARAM>(ToRgba(color, alpha)),
        SMTO_ABORTIFHUNG,
        80,
        &result);
    const bool ok = sent && result == 0;
    static std::map<HWND, DWORD> lastLoggedByTaskbar;
    DWORD &lastLoggedColor = lastLoggedByTaskbar[taskbar];
    const DWORD rgba = ToRgba(color, alpha);
    if (!ok || rgba != lastLoggedColor) {
        std::ostringstream ss;
        ss << "TTB preview sent=" << sent << " result=" << result << " rgba=" << Hex32(rgba);
        Log(ss.str());
        lastLoggedColor = rgba;
    }
    return ok;
}

void RedrawTaskbarNow(Taskbar &tb) {
    auto redraw = [](HWND hwnd) {
        if (!IsWindow(hwnd)) {
            return;
        }
        RedrawWindow(
            hwnd,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_ERASENOW | RDW_UPDATENOW | RDW_ALLCHILDREN);
        UpdateWindow(hwnd);
    };

    redraw(tb.hwnd);
    for (HWND target : tb.paintTargets) {
        if (target != tb.hwnd) {
            redraw(target);
        }
    }
    Log("[RESTORE] targeted taskbar redraw requested");
}

bool ApplyTranslucentTbCom(Taskbar &tb, const Color &color) {
    if (!EnsureExplorerTapService(tb.hwnd)) {
        return false;
    }

    const UINT colorAbgr = (static_cast<UINT>(0xFF) << 24) |
                           (static_cast<UINT>(color.b) << 16) |
                           (static_cast<UINT>(color.g) << 8) |
                           static_cast<UINT>(color.r);
    const HRESULT hr = g_ttbAppearanceService->SetTaskbarAppearance(tb.hwnd, 1, colorAbgr);
    if (SUCCEEDED(hr)) {
        tb.lastAppliedAt = GetTickCount();
    }
    static std::map<HWND, UINT> lastLoggedByTaskbar;
    UINT &lastLoggedColor = lastLoggedByTaskbar[tb.hwnd];
    if (FAILED(hr) || colorAbgr != lastLoggedColor) {
        std::ostringstream ss;
        ss << "ExplorerTAP SetTaskbarAppearance hr=" << HexHr(hr)
           << " hwnd=0x" << std::hex << reinterpret_cast<uintptr_t>(tb.hwnd)
           << " abgr=" << Hex32(colorAbgr);
        Log(ss.str());
        lastLoggedColor = colorAbgr;
    }
    return SUCCEEDED(hr);
}

bool RestoreTranslucentTbCom(Taskbar &tb) {
    if (!EnsureExplorerTapService(tb.hwnd)) {
        return false;
    }
    ApplyTaskbarIconTheme(tb, nullptr);
    // TranslucentTB's "Clear" mode maps to a transparent SolidColor brush in
    // ExplorerTAP. Returning to Windows' default XAML fill can leave the dark
    // taskbar brush visible on the desktop.
    const HRESULT clearHr = g_ttbAppearanceService->SetTaskbarAppearance(tb.hwnd, 1, 0x00000000);
    Log("ExplorerTAP SetTaskbarAppearance clear hr=" + HexHr(clearHr));
    if (SUCCEEDED(clearHr)) {
        return true;
    }

    const HRESULT defaultHr = g_ttbAppearanceService->ReturnTaskbarToDefaultAppearance(tb.hwnd);
    Log("ExplorerTAP ReturnTaskbarToDefaultAppearance fallback hr=" + HexHr(defaultHr));
    return SUCCEEDED(defaultHr);
}

bool ReassertTaskbarDefault(Taskbar &tb) {
    if (!IsWindow(tb.hwnd) || !g_ttbAppearanceService) {
        return false;
    }
    ApplyTaskbarIconTheme(tb, nullptr);
    const HRESULT hr = g_ttbAppearanceService->SetTaskbarAppearance(tb.hwnd, 1, 0x00000000);
    if (FAILED(hr)) {
        Log("ExplorerTAP clear reassert failed hr=" + HexHr(hr));
    }
    return SUCCEEDED(hr);
}

BOOL CALLBACK EnumPaintTargetProc(HWND child, LPARAM lparam) {
    auto *tb = reinterpret_cast<Taskbar *>(lparam);
    if (!IsWindowVisible(child)) {
        return TRUE;
    }

    RECT rc {};
    if (!GetWindowRect(child, &rc)) {
        return TRUE;
    }

    const std::wstring cls = WindowClassName(child);
    if (cls == L"ImmersiveTopTaskbarSeamCover") {
        return TRUE;
    }
    const bool fullWidth = Width(rc) >= Width(tb->rect) * 8 / 10;
    const bool sameTop = std::abs(rc.top - tb->rect.top) <= 2;
    const bool sameHeight = std::abs(Height(rc) - Height(tb->rect)) <= 4;
    const bool xamlBridge = cls == L"Windows.UI.Composition.DesktopWindowContentBridge";

    if ((xamlBridge || (fullWidth && sameTop && sameHeight)) &&
        std::find(tb->paintTargets.begin(), tb->paintTargets.end(), child) == tb->paintTargets.end()) {
        tb->paintTargets.push_back(child);
    }

    return TRUE;
}

void RefreshTaskbarPaintTargets() {
    for (auto &[_, tb] : g_taskbars) {
        tb.paintTargets.erase(
            std::remove_if(tb.paintTargets.begin(), tb.paintTargets.end(), [](HWND hwnd) {
                return !IsWindow(hwnd);
            }),
            tb.paintTargets.end());
        if (std::find(tb.paintTargets.begin(), tb.paintTargets.end(), tb.hwnd) == tb.paintTargets.end()) {
            tb.paintTargets.insert(tb.paintTargets.begin(), tb.hwnd);
        }
        EnumChildWindows(tb.hwnd, EnumPaintTargetProc, reinterpret_cast<LPARAM>(&tb));
    }
}

void RefreshTaskbars() {
    std::map<HWND, Taskbar> old = std::move(g_taskbars);
    g_taskbars.clear();
    EnumWindows(EnumTaskbarProc, 0);
    if (g_taskbars.empty()) {
        AddFindWindowTaskbarFallbacks();
    }

    for (auto &[hwnd, tb] : g_taskbars) {
        if (auto it = old.find(hwnd); it != old.end()) {
            tb.tinted = it->second.tinted;
            tb.current = it->second.current;
            tb.from = it->second.from;
            tb.target = it->second.target;
            tb.animStart = it->second.animStart;
            tb.animDuration = it->second.animDuration;
            tb.animating = it->second.animating;
            tb.lastAppliedAt = it->second.lastAppliedAt;
            tb.targetWindow = it->second.targetWindow;
            tb.lastMaximizedWindow = it->second.lastMaximizedWindow;
            tb.lastMaximizedSeen = it->second.lastMaximizedSeen;
            tb.restorePending = it->second.restorePending;
            tb.restorePendingAt = it->second.restorePendingAt;
            tb.restoreReassertUntil = it->second.restoreReassertUntil;
            tb.lastRestoreReassertAt = it->second.lastRestoreReassertAt;
            tb.rawTarget = it->second.rawTarget;
            tb.pendingSettingsTarget = it->second.pendingSettingsTarget;
            tb.earlyProbeTarget = it->second.earlyProbeTarget;
            tb.correctionR = it->second.correctionR;
            tb.correctionG = it->second.correctionG;
            tb.correctionB = it->second.correctionB;
            tb.lastCalibration = it->second.lastCalibration;
            tb.pendingSettingsTargetAt = it->second.pendingSettingsTargetAt;
            tb.pendingSettingsTargetValid = it->second.pendingSettingsTargetValid;
            tb.earlyProbeTargetAt = it->second.earlyProbeTargetAt;
            tb.earlyProbeWindow = it->second.earlyProbeWindow;
            tb.earlyProbeTargetValid = it->second.earlyProbeTargetValid;
            tb.seamCover = it->second.seamCover;
            tb.seamCoverHeight = it->second.seamCoverHeight;
            old.erase(it);
        }
    }
    for (auto &[_, tb] : old) {
        DestroySeamCover(tb);
    }
    RefreshTaskbarPaintTargets();
    std::ostringstream ss;
    ss << "taskbars=" << g_taskbars.size();
    for (const auto &[_, tb] : g_taskbars) {
        ss << " hwnd=0x" << std::hex << reinterpret_cast<uintptr_t>(tb.hwnd)
           << std::dec << " top=" << tb.top
           << " rect=(" << tb.rect.left << ',' << tb.rect.top << ',' << tb.rect.right << ',' << tb.rect.bottom << ')'
           << " paintTargets=" << tb.paintTargets.size();
    }
    Log(ss.str());
}

void PaintSeamCover(HWND hwnd, HDC dc) {
    RECT rc {};
    GetClientRect(hwnd, &rc);
    const COLORREF color = static_cast<COLORREF>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    HBRUSH brush = CreateSolidBrush(color);
    if (brush) {
        FillRect(dc, &rc, brush);
        DeleteObject(brush);
    }
}

void DestroySeamCover(Taskbar &tb) {
    if (tb.seamCover && IsWindow(tb.seamCover)) {
        DestroyWindow(tb.seamCover);
    }
    tb.seamCover = nullptr;
}

LRESULT CALLBACK SeamCoverWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_ERASEBKGND:
        PaintSeamCover(hwnd, reinterpret_cast<HDC>(wparam));
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps {};
        HDC dc = BeginPaint(hwnd, &ps);
        PaintSeamCover(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

void RestoreTaskbar(Taskbar &tb, bool reassertDefault = true) {
    DestroySeamCover(tb);
    if (!IsWindow(tb.hwnd)) {
        tb.tinted = false;
        RestoreShellThemeIfNoTintedTaskbars();
        return;
    }

    std::ostringstream ss;
    ss << "[RESTORE] taskbar=0x" << std::hex << reinterpret_cast<uintptr_t>(tb.hwnd)
       << std::dec << " tinted=" << tb.tinted << " ExplorerTAP=" << (g_ttbAppearanceService != nullptr);
    Log(ss.str());

    // Mark the state as restoring before touching either backend. This makes
    // the animation tick drop any stale opaque frame immediately.
    tb.tinted = false;
    tb.restorePending = false;
    tb.restorePendingAt = 0;
    tb.animating = false;
    tb.restoreReassertUntil = reassertDefault ? GetTickCount() + kDefaultReassertDurationMs : 0;
    tb.lastRestoreReassertAt = 0;

    // Theme restoration must not wait behind TranslucentTB's settings refresh.
    // During rapid maximize/minimize loops that refresh can take over a second,
    // leaving tray text in the previous theme after the taskbar has restored.
    RestoreShellThemeIfNoTintedTaskbars();

    const bool restored = RestoreTranslucentTbCom(tb);
    // ExplorerTAP is the only runtime renderer. Do not send TTB preview,
    // settings refresh, DWM notifications, or a synthetic redraw here: each
    // extra path can race Explorer's own brush transition.
    if (restored) {
        Log("[RESTORE] success - ExplorerTAP restored taskbar");
    } else {
        Log("[RESTORE] warning - ExplorerTAP restore failed");
    }
    RestoreTtbManagedAppearancesOnce();

    tb.targetWindow = nullptr;
    tb.lastMaximizedWindow = nullptr;
    tb.rawTarget = {};
    tb.pendingSettingsTarget = {};
    tb.correctionR = 0;
    tb.correctionG = 0;
    tb.correctionB = 0;
    tb.lastCalibration = 0;
    tb.pendingSettingsTargetAt = 0;
    tb.pendingSettingsTargetValid = false;
    tb.seamCoverHeight = kSeamCoverHeightInactive;
    RestoreShellThemeIfNoTintedTaskbars();
    if (reassertDefault) {
        SetTimer(g_window, kAnimTimer, kAnimIntervalMs, nullptr);
    }
}

void RestoreAllTaskbars() {
    for (auto &[_, tb] : g_taskbars) {
        RestoreTaskbar(tb);
    }
    RestoreTranslucentTbMaximizedAppearance();
    ForceRestoreShellThemeNow("restore-all");
}

LONG WINAPI TopLevelExceptionRestoreFilter(EXCEPTION_POINTERS *) {
    ForceRestoreShellThemeNow("unhandled-exception");
    RestoreTranslucentTbMaximizedAppearance();
    return EXCEPTION_EXECUTE_HANDLER;
}

void RefreshExternalBackends() {
    g_ttbWindow = FindTranslucentTbWindow();
    if (g_ttbWindow) {
        Log("TranslucentTB detected; using ExplorerTAP renderer");
    } else {
        Log("TranslucentTB not detected; renderer disabled");
    }
}

void RegisterExplorerTapProcessDeathRestore() {
    bool registered = false;
    for (auto &[_, tb] : g_taskbars) {
        if (!IsWindow(tb.hwnd) || !EnsureExplorerTapService(tb.hwnd)) {
            continue;
        }
        const HRESULT hr =
            g_ttbAppearanceService->RestoreAllTaskbarsToDefaultWhenProcessDies(GetCurrentProcessId());
        Log("ExplorerTAP process-death restore hr=" + HexHr(hr));
        registered = registered || SUCCEEDED(hr);
        break;
    }
    if (!registered) {
        Log("ExplorerTAP process-death restore not registered");
    }
}

std::filesystem::path FindExplorerTapDll() {
    wchar_t localAppData[MAX_PATH] {};
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return {};
    }

    const std::filesystem::path packages = std::filesystem::path(localAppData) / L"Packages";
    std::error_code ec;
    if (!std::filesystem::exists(packages, ec)) {
        return {};
    }

    for (const auto &entry : std::filesystem::directory_iterator(packages, ec)) {
        if (ec || !entry.is_directory()) {
            continue;
        }

        std::wstring name = entry.path().filename().wstring();
        std::transform(name.begin(), name.end(), name.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });
        if (name.find(L"translucenttb") == std::wstring::npos) {
            continue;
        }

        const std::array<std::filesystem::path, 2> candidates {
            entry.path() / L"TempState" / L"ExplorerTAP.dll",
            entry.path() / L"LocalState" / L"ExplorerTAP.dll",
        };
        for (const auto &candidate : candidates) {
            if (std::filesystem::exists(candidate, ec)) {
                return candidate;
            }
        }
    }

    return {};
}

bool LoadExplorerTapBackend() {
    if (g_injectExplorerTap) {
        return true;
    }

    const std::filesystem::path dllPath = FindExplorerTapDll();
    if (dllPath.empty()) {
        Log("ExplorerTAP.dll not found");
        return false;
    }

    g_explorerTapDll = LoadLibraryW(dllPath.c_str());
    if (!g_explorerTapDll) {
        Log("LoadLibrary ExplorerTAP.dll failed gle=" + std::to_string(GetLastError()) + " path=" + Narrow(dllPath.wstring()));
        return false;
    }

    g_injectExplorerTap = reinterpret_cast<InjectExplorerTAPProc>(
        GetProcAddress(g_explorerTapDll, "InjectExplorerTAP"));
    if (!g_injectExplorerTap) {
        Log("GetProcAddress InjectExplorerTAP failed gle=" + std::to_string(GetLastError()));
        FreeLibrary(g_explorerTapDll);
        g_explorerTapDll = nullptr;
        return false;
    }

    Log("ExplorerTAP backend loaded path=" + Narrow(dllPath.wstring()));
    return true;
}

bool EnsureExplorerTapService(HWND taskbar) {
    if (g_ttbAppearanceService) {
        return true;
    }
    // Back off after a failed injection: retrying every animation frame spams
    // the TTB window and floods the log while TTB is unavailable (e.g. after a
    // TTB update). The settings-driven endorsement keeps rendering meanwhile.
    static DWORD s_lastInjectAttempt = 0;
    const DWORD now = GetTickCount();
    if (s_lastInjectAttempt && now - s_lastInjectAttempt < 2000) {
        return false;
    }
    s_lastInjectAttempt = now;
    if (!LoadExplorerTapBackend() || !g_injectExplorerTap || !IsWindow(taskbar)) {
        return false;
    }

    HRESULT hr = g_injectExplorerTap(
        taskbar,
        kIidTaskbarAppearanceService,
        reinterpret_cast<void **>(&g_ttbAppearanceService));
    Log("ExplorerTAP InjectExplorerTAP hr=" + HexHr(hr));
    if (FAILED(hr) || !g_ttbAppearanceService) {
        g_ttbAppearanceService = nullptr;
        return false;
    }
    Log("ExplorerTAP service cached");
    return true;
}

// ---- Startup usage notice + environment detection ----

// A single checkable environment item shown in the startup report. `pass`
// drives the ✅/❌ glyph; `required` decides whether a failure blocks launch;
// `fix` is a one-line remedy shown when the item fails.
struct EnvCheck {
    std::wstring label;
    bool pass = false;
    bool required = true;
    std::wstring fix;
};

// 读取任务栏对齐方式：注册表 HKCU\...\Explorer\Advanced 的 TaskbarAl
// 值，1 = 顶部，0/缺省 = 底部。返回 true 表示任务栏在顶部。
bool IsTaskbarTopAligned() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    const LONG rc = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
        L"TaskbarAl",
        RRF_RT_REG_DWORD, nullptr, &value, &size);
    return rc == ERROR_SUCCESS && value == 1;
}

// Remove the no-op loop: `FindTranslucentTbSettingsPath` is the Store signal.
// Declare the RtlGetVersion extended struct directly so we don't depend on the
// SDK exposing RTL_OSVERSIONINFOW (it lives in winternl.h, not windows.h).
struct RtlOsVersionInfoW {
    ULONG dwOSVersionInfoSize = 0;
    ULONG dwMajorVersion = 0;
    ULONG dwMinorVersion = 0;
    ULONG dwBuildNumber = 0;
    ULONG dwPlatformId = 0;
    wchar_t szCSDVersion[128] {};
};

// Parse the "Major.Minor.Build.Revision" tuple embedded in a Store package
// Resolve the TranslucentTB version quad. The version only exists in the
// package FULL name (e.g. "28017CharlesMilette.TranslucentTB_2026.1.0.0_x64__<hash>"),
// which is embedded in the running process image path for Store installs:
//   C:\Program Files\WindowsApps\..._2026.1.0.0_x64__<hash>\TranslucentTB.exe
// The %LOCALAPPDATA%\Packages\<folder> name is the PackageFamilyName and does
// NOT contain the version, so we must NOT parse that.
bool ParseTtbVersion(DWORD &major, DWORD &minor, DWORD &build, DWORD &revision) {
    HWND ttbWnd = FindTranslucentTbWindow();
    if (!ttbWnd) {
        return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(ttbWnd, &pid);
    if (!pid) {
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return false;
    }
    wchar_t imagePath[MAX_PATH] {};
    DWORD size = static_cast<DWORD>(std::size(imagePath));
    const BOOL ok = QueryFullProcessImageNameW(process, 0, imagePath, &size);
    CloseHandle(process);
    if (!ok || size == 0) {
        return false;
    }

    const std::wstring path(imagePath);
    // 直接在整条路径里找 "TranslucentTB_" 之后的版本四元组。
    //   例: C:\...\28017CharlesMilette.TranslucentTB_2026.1.0.0_x64__<hash>\TranslucentTB.exe
    // 这样不依赖目录名的最后一个 '\'，避免把 exe 文件名误当成目录名。
    const std::wstring marker = L"TranslucentTB_";
    size_t pos = path.find(marker);
    if (pos == std::wstring::npos) {
        // 小写兜底
        std::wstring lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });
        pos = lower.find(L"translucenttb_");
        if (pos == std::wstring::npos) {
            return false;
        }
    }
    std::wstring ver = path.substr(pos + marker.size());  // "2026.1.0.0_x64__<hash>\..."
    ver = ver.substr(0, ver.find(L'_'));                    // "2026.1.0.0"

    unsigned long a = 0, b = 0, c = 0, d = 0;
    wchar_t *endp = nullptr;
    a = wcstoul(ver.c_str(), &endp, 10);
    if (endp && *endp == L'.') { b = wcstoul(endp + 1, &endp, 10); }
    if (endp && *endp == L'.') { c = wcstoul(endp + 1, &endp, 10); }
    if (endp && *endp == L'.') { d = wcstoul(endp + 1, &endp, 10); }
    major = a; minor = b; build = c; revision = d;
    return a != 0;
}

bool IsStoreTranslucentTb() {
    // The settings.json lives under %LOCALAPPDATA%\Packages\<pf>\RoamingState,
    // which only the Microsoft Store (MSIX/appx) flavor of TranslucentTB uses.
    // The portable build keeps its config next to the exe, not under Packages,
    // so "settings.json under Packages" is a reliable Store-edition signal.
    return !FindTranslucentTbSettingsPath().empty();
}

// Show the first-run usage notice. Returns true only after the user ticks the
// "我已阅读并同意" checkbox and presses 同意. Returns false if they decline
// (or close the window), which aborts launch.
bool ShowStartupNotice() {
    // The "同意" button is the OK button; the user must click it to proceed.
    // Cancel (and closing the box) aborts so the user is never forced through.
    int result = MessageBoxW(
        nullptr,
        L"使用须知\r\n\r\n"
        L"1. ImmersiveTopTaskbar 需要 Microsoft Store 版 TranslucentTB（版本 2026.1 或更高）"
        L" 处于运行状态，并且任务栏位于屏幕顶部。\r\n\r\n"
        L"2. 运行期间本程序会采样最大化窗口的颜色来为任务栏着色，"
        L" 并可能在需要时临时改写 TranslucentTB 的 settings.json（并在退出时恢复）。\r\n\r\n"
        L"3. 请勿在运行中途强制结束进程以免留下临时配置；如遇异常，可运行 "
        L"ImmersiveTopTaskbar.exe --restore-ttb 恢复。\r\n\r\n"
        L"点击“确定”表示你已阅读并同意以上内容，随后将进入环境检测。",
        L"ImmersiveTopTaskbar — 使用须知",
        MB_OKCANCEL | MB_ICONINFORMATION | MB_SETFOREGROUND);
    return result == IDOK;
}

// 把 build 号映射成用户能看懂的 Windows 版本名（如 "26H2" / "25H2" / "24H2"）。
std::wstring BuildToFriendlyName(DWORD build) {
    if (build >= 26200) {
        return L"26H2 或更高";
    }
    if (build >= 26100) {
        return L"25H2";
    }
    if (build >= 22621) {
        return L"24H2 及更早";
    }
    return L"较早版本";
}

bool RunPreflightChecks(bool showSuccessDialog) {
    std::vector<EnvCheck> checks;

    // 1) CPU 架构（x64 原生程序，非 x64 无法运行——属于硬性必需项）。
#ifdef _WIN64
    checks.push_back({ L"系统架构为 64 位（x64）", true, true, L"" });
#else
    checks.push_back({ L"系统架构为 64 位（x64）", false, true, L"请使用 64 位版本的 Windows。" });
#endif

    // 2) Windows 版本：这是「建议项」——旧版也可以靠注册表/第三方把任务栏
    //    置于顶部，不影响本程序运行，故仅作建议，不阻断启动。
    const DWORD osBuild = [] {
        using RtlGetVersionProc = LONG(WINAPI *)(RtlOsVersionInfoW *);
        auto rtlGetVersion = reinterpret_cast<RtlGetVersionProc>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        RtlOsVersionInfoW osvi {};
        if (rtlGetVersion) {
            rtlGetVersion(&osvi);
        }
        return osvi.dwBuildNumber;
    }();
    {
        const bool ok = osBuild >= 26200;
        std::wstring label = L"Windows 11 版本：" + BuildToFriendlyName(osBuild);
        if (!ok) {
            label += L"（当前版本不支持原生“任务栏位置”设置）";
        } else {
            label += L"（支持原生“任务栏位置”设置）";
        }
        std::wstring fix = ok ? L"" :
            L"当前系统低于 26H2，没有“任务栏位置”原生选项；如任务栏已在顶部，此项可忽略。";
        checks.push_back({ label, ok, false, fix });  // required=false → 建议项
    }

    // 3) TranslucentTB 正在运行（必需）。
    const bool ttbRunning = FindTranslucentTbWindow() != nullptr;
    checks.push_back({ L"TranslucentTB 正在运行", ttbRunning, true,
        ttbRunning ? L"" : L"请先启动 TranslucentTB（Microsoft Store 版）。" });

    // 4) TranslucentTB 为 Store 版且版本 ≥ 2026.1（必需）。
    const bool isStore = IsStoreTranslucentTb();
    DWORD vMajor = 0, vMinor = 0, vBuild = 0, vRev = 0;
    const bool versionParsed = ParseTtbVersion(vMajor, vMinor, vBuild, vRev);
    const bool ttbVersionOk = versionParsed && (vMajor > 2026 || (vMajor == 2026 && vMinor >= 1));
    std::wstring ttbVersionLabel = L"TranslucentTB 为 Microsoft Store 版（版本 ≥ 2026.1）";
    if (versionParsed) {
        ttbVersionLabel += L"（当前 " + std::to_wstring(vMajor) + L"." + std::to_wstring(vMinor) +
                           L"." + std::to_wstring(vBuild) + L"." + std::to_wstring(vRev) + L"）";
    }
    {
        std::wstring fix;
        if (!isStore) {
            fix = L"请安装 Microsoft Store 版 TranslucentTB（便携版不兼容）。";
        } else if (!ttbVersionOk) {
            fix = L"TranslucentTB 版本过低，请在 Microsoft Store 更新到 2026.1 或更高。";
        }
        checks.push_back({ ttbVersionLabel, isStore && ttbVersionOk, true, fix });
    }

    // 5) ExplorerTAP.dll 可加载（必需）。
    const bool tapLoaded = LoadExplorerTapBackend();
    checks.push_back({ L"ExplorerTAP.dll 可加载", tapLoaded, true,
        tapLoaded ? L"" : L"TranslucentTB 版本可能不兼容，请更新到最新版后重试。" });

    // 6) 任务栏位于屏幕顶部（必需；合并了「几何位置」与「注册表 TaskbarAl」两种判据，
    //    任一满足即视为顶部，避免重复两条造成困惑）。
    RefreshTaskbars();
    bool hasTop = false;
    for (const auto &[_, tb] : g_taskbars) {
        if (tb.top && IsWindow(tb.hwnd)) {
            hasTop = true;
            break;
        }
    }
    const bool taskbarTopAligned = IsTaskbarTopAligned();
    const bool topOk = hasTop || taskbarTopAligned;
    {
        std::wstring label = L"任务栏位于屏幕顶部";
        std::wstring fix;
        if (!topOk) {
            fix = L"请将任务栏移动到屏幕顶部（设置 → 个性化 → 任务栏 → 任务栏行为 → 位置选“顶部”）。";
        }
        checks.push_back({ label, topOk, true, fix });
    }

    // 生成报告：✅/❌ + 标签；失败项紧跟一行「→ 修复建议」。
    std::wstring report = L"环境检测结果：\r\n\r\n";
    bool anyRequiredFail = false;
    for (const auto &check : checks) {
        report += (check.pass ? L"✅ " : L"❌ ");
        report += check.label;
        if (check.required) {
            report += L"（必需）";
        } else {
            report += L"（建议）";
        }
        report += L"\r\n";
        if (!check.pass && !check.fix.empty()) {
            report += L"    → " + check.fix + L"\r\n";
        }
        if (check.required && !check.pass) {
            anyRequiredFail = true;
        }
    }

    std::wstring fullMsg = report;
    if (anyRequiredFail) {
        fullMsg += L"\r\n上述 ❌ 必需项未通过，程序无法启动，请按建议修复后重试。";
    } else {
        fullMsg += L"\r\n必需项全部通过，程序即将启动。";
    }
    Log("preflight report: " + Narrow(fullMsg));
    if (anyRequiredFail || showSuccessDialog) {
        MessageBoxW(nullptr, fullMsg.c_str(), L"ImmersiveTopTaskbar — 环境检测",
                    anyRequiredFail ? MB_ICONWARNING : MB_ICONINFORMATION | MB_OK);
    } else {
        Log("preflight passed silently");
    }
    return !anyRequiredFail;
}

int RunExplorerTapProbe() {
    RefreshTaskbars();
    if (!LoadExplorerTapBackend()) {
        Log("TAP probe: backend unavailable");
        return 2;
    }

    for (auto &[_, tb] : g_taskbars) {
        if (!tb.top || !IsWindow(tb.hwnd)) {
            continue;
        }

        Log("TAP probe: calling InjectExplorerTAP");
        ITaskbarAppearanceService *service = nullptr;
        HRESULT hr = g_injectExplorerTap(tb.hwnd, kIidTaskbarAppearanceService, reinterpret_cast<void **>(&service));
        Log("TAP probe: InjectExplorerTAP hr=" + HexHr(hr));
        if (service) {
            const UINT color = (static_cast<UINT>(0xFF) << 24) |
                               (static_cast<UINT>(144) << 16) |
                               (static_cast<UINT>(96) << 8) |
                               static_cast<UINT>(48);
            Log("TAP probe: calling SetTaskbarAppearance");
            const HRESULT applyHr = service->SetTaskbarAppearance(tb.hwnd, 1, color);
            Log("TAP probe: SetTaskbarAppearance hr=" + HexHr(applyHr));
            const HRESULT restoreHr = service->ReturnTaskbarToDefaultAppearance(tb.hwnd);
            Log("TAP probe: ReturnTaskbarToDefaultAppearance hr=" + HexHr(restoreHr));
            service->Release();
            Log("TAP probe: service acquired and released");
            const bool solidOk = SUCCEEDED(applyHr);
            return solidOk ? 0 : 3;
        }
        return SUCCEEDED(hr) ? 4 : 3;
    }

    Log("TAP probe: top taskbar not found");
    return 5;
}

std::vector<Color> SampleGrid(HDC dc, const RECT &sample) {
    std::vector<Color> colors;
    colors.reserve(4 * 64);

    const int width = Width(sample);
    const int height = Height(sample);
    if (width <= 0 || height <= 0) {
        return colors;
    }

    BITMAPINFO bmi {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        return colors;
    }

    HDC memDc = CreateCompatibleDC(dc);
    if (!memDc) {
        DeleteObject(bitmap);
        return colors;
    }

    HGDIOBJ oldBitmap = SelectObject(memDc, bitmap);
    if (!BitBlt(memDc, 0, 0, width, height, dc, sample.left, sample.top, SRCCOPY)) {
        SelectObject(memDc, oldBitmap);
        DeleteDC(memDc);
        DeleteObject(bitmap);
        return colors;
    }

    const auto *pixels = static_cast<const uint8_t *>(bits);
    constexpr int rows = 4;
    constexpr int cols = 64;
    for (int row = 0; row < rows; ++row) {
        const int y = ((height - 1) * row) / std::max(1, rows - 1);
        for (int col = 0; col < cols; ++col) {
            const int x = ((width - 1) * col) / std::max(1, cols - 1);
            const size_t offset = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
            colors.push_back({
                pixels[offset + 2],
                pixels[offset + 1],
                pixels[offset],
            });
        }
    }

    SelectObject(memDc, oldBitmap);
    DeleteDC(memDc);
    DeleteObject(bitmap);
    return colors;
}

struct DominantResult {
    Color color {};
    double coverage = 0.0;
};

int ColorChroma(const Color &color) {
    const int maxChannel = std::max({ static_cast<int>(color.r), static_cast<int>(color.g), static_cast<int>(color.b) });
    const int minChannel = std::min({ static_cast<int>(color.r), static_cast<int>(color.g), static_cast<int>(color.b) });
    return maxChannel - minChannel;
}

int MaxColorChannel(const Color &color) {
    return std::max({ static_cast<int>(color.r), static_cast<int>(color.g), static_cast<int>(color.b) });
}

bool IsNeutralDarkColor(const Color &color) {
    return MaxColorChannel(color) <= 64 && ColorChroma(color) <= 4;
}

DominantResult DominantBackgroundColor(const std::vector<Color> &colors) {
    struct Bucket {
        int count = 0;
        int r = 0;
        int g = 0;
        int b = 0;
    };

    constexpr int bucketSize = 4;
    std::map<int, Bucket> buckets;
    for (const auto &c : colors) {
        const int rq = c.r / bucketSize;
        const int gq = c.g / bucketSize;
        const int bq = c.b / bucketSize;
        const int key = (rq << 16) | (gq << 8) | bq;
        auto &bucket = buckets[key];
        bucket.count += 1;
        bucket.r += c.r;
        bucket.g += c.g;
        bucket.b += c.b;
    }

    auto best = buckets.end();
    for (auto it = buckets.begin(); it != buckets.end(); ++it) {
        if (best == buckets.end() || it->second.count > best->second.count) {
            best = it;
        }
    }

    if (best == buckets.end() || best->second.count == 0) {
        return { MedianColor(colors), 0.0 };
    }

    Color seed {
        static_cast<uint8_t>(best->second.r / best->second.count),
        static_cast<uint8_t>(best->second.g / best->second.count),
        static_cast<uint8_t>(best->second.b / best->second.count),
    };

    std::vector<Color> kept;
    kept.reserve(colors.size());
    const int threshold = 5 * 5;
    for (const auto &c : colors) {
        if (ColorDistance2(c, seed) <= threshold) {
            kept.push_back(c);
        }
    }

    if (kept.size() < colors.size() / 8) {
        return { seed, static_cast<double>(best->second.count) / static_cast<double>(colors.size()) };
    }

    int r = 0;
    int g = 0;
    int b = 0;
    for (const auto &c : kept) {
        r += c.r;
        g += c.g;
        b += c.b;
    }

    return {
        {
            static_cast<uint8_t>(r / static_cast<int>(kept.size())),
            static_cast<uint8_t>(g / static_cast<int>(kept.size())),
            static_cast<uint8_t>(b / static_cast<int>(kept.size())),
        },
        static_cast<double>(kept.size()) / static_cast<double>(colors.size()),
    };
}

Color SampleTaskbarBackground(const Taskbar &tb) {
    HDC dc = GetDC(nullptr);
    if (!dc) {
        return {};
    }

    const int width = std::min(320, std::max(32, Width(tb.rect) / 8));
    const int y = tb.rect.top + std::max(2, Height(tb.rect) / 2 - 4);
    const RECT leftSample { tb.rect.left + 8, y, tb.rect.left + 8 + width, y + 8 };
    const RECT rightSample { tb.rect.right - 8 - width, y, tb.rect.right - 8, y + 8 };
    std::vector<Color> colors = SampleGrid(dc, leftSample);
    std::vector<Color> rightColors = SampleGrid(dc, rightSample);
    colors.insert(colors.end(), rightColors.begin(), rightColors.end());
    ReleaseDC(nullptr, dc);

    return colors.empty() ? Color {} : DominantBackgroundColor(colors).color;
}

bool ComputeRobustTopColor(HWND foreground, const Taskbar &taskbar, Color &outColor) {
    RECT windowRect {};
    if (!GetWindowRect(foreground, &windowRect)) {
        return false;
    }

    MONITORINFO mi {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(taskbar.monitor, &mi)) {
        return false;
    }

    const bool settingsWindow = IsWindowsSettingsWindow(foreground);
    const bool activeForeground = foreground == GetForegroundWindow();
    if (settingsWindow && !activeForeground) {
        return false;
    }
    const int windowWidth = Width(windowRect);
    const int leftInset = settingsWindow ?
        std::max(24, windowWidth / 4) :
        std::max(24, windowWidth / 40);
    const int rightInset = settingsWindow ?
        std::max(24, windowWidth / 10) :
        leftInset;
    const int left = ClampInt(windowRect.left + leftInset, mi.rcMonitor.left, mi.rcMonitor.right - 4);
    const int right = ClampInt(windowRect.right - rightInset, mi.rcMonitor.left + 4, mi.rcMonitor.right);

    if (right - left < 16) {
        return false;
    }

    HDC dc = GetDC(nullptr);
    if (!dc) {
        return false;
    }

    const int baseTop = std::max(windowRect.top, taskbar.rect.bottom);
    DominantResult best {};
    RECT bestRect {};
    bool found = false;

    // Settings subpages can have a neutral gray top surface while the content
    // below briefly keeps the blue home-page brush. Prefer the shallow neutral
    // band in that case so the top taskbar stays visually attached.
    const std::vector<std::pair<int, int>> bands = settingsWindow ?
        std::vector<std::pair<int, int>> {
            { taskbar.seamCoverHeight + 4, 32 },
            { taskbar.seamCoverHeight + 48, 40 },
        } :
        std::vector<std::pair<int, int>> {
            { taskbar.seamCoverHeight + 8, 16 },
        };

    for (const auto &[offset, height] : bands) {
        const int top = ClampInt(baseTop + offset, mi.rcMonitor.top, mi.rcMonitor.bottom - 4);
        const int bottom = ClampInt(top + height, top + 4, std::min(windowRect.bottom, mi.rcMonitor.bottom));
        const RECT sample {
            left,
            top,
            right,
            bottom,
        };

        if (Width(sample) < 16 || Height(sample) < 4) {
            continue;
        }

        std::vector<Color> colors = SampleGrid(dc, sample);
        if (colors.size() < 24) {
            continue;
        }

        DominantResult candidate = DominantBackgroundColor(colors);
        if (settingsWindow && IsNeutralDarkColor(candidate.color) && candidate.coverage >= 0.45) {
            const bool bestIsNeutral = found && IsNeutralDarkColor(best.color);
            if (!found || !bestIsNeutral || candidate.coverage > best.coverage * 0.55) {
                best = candidate;
                bestRect = sample;
                found = true;
                continue;
            }
        }

        const double boundaryPenalty = offset <= taskbar.seamCoverHeight + 1 ? 0.82 : 1.0;
        const double candidateScore = candidate.coverage * boundaryPenalty;
        const double bestScore = best.coverage * (bestRect.top <= baseTop + taskbar.seamCoverHeight + 1 ? 0.82 : 1.0);
        if (!found || candidateScore > bestScore) {
            best = candidate;
            bestRect = sample;
            found = true;
        }
    }

    ReleaseDC(nullptr, dc);

    if (!found) {
        return false;
    }

    outColor = best.color;

    static std::map<HWND, std::pair<RECT, Color>> lastLoggedByTaskbar;
    auto &[lastLoggedRect, lastLoggedColor] = lastLoggedByTaskbar[taskbar.hwnd];
    if (ColorDistance2(lastLoggedColor, outColor) > 9 ||
        lastLoggedRect.top != bestRect.top ||
        lastLoggedRect.bottom != bestRect.bottom) {
        std::ostringstream ss;
        ss << "chosen band=(" << bestRect.left << ',' << bestRect.top << ','
           << bestRect.right << ',' << bestRect.bottom << ") coverage="
           << static_cast<int>(best.coverage * 100.0) << "%"
           << " settings=" << settingsWindow;
        Log(ss.str());
        lastLoggedRect = bestRect;
        lastLoggedColor = outColor;
    }

    // Keep the sampled color unchanged; foreground color is controlled by Windows.

    return true;
}

bool ComputeFastThemeProbeColor(HWND window, const Taskbar &taskbar, Color &outColor) {
    RECT windowRect {};
    if (!GetWindowRect(window, &windowRect)) {
        return false;
    }
    const int windowWidth = Width(windowRect);
    const int windowHeight = Height(windowRect);
    if (windowWidth < 64 || windowHeight < 64) {
        return false;
    }

    HDC screenDc = GetDC(nullptr);
    if (screenDc) {
        HDC memDc = CreateCompatibleDC(screenDc);
        HBITMAP bitmap = memDc ? CreateCompatibleBitmap(screenDc, windowWidth, windowHeight) : nullptr;
        HGDIOBJ oldBitmap = bitmap ? SelectObject(memDc, bitmap) : nullptr;
        if (memDc && bitmap) {
            constexpr UINT kPrintWindowRenderFullContent = 0x00000002;
            if (PrintWindow(window, memDc, kPrintWindowRenderFullContent)) {
                const int sampleWidth = std::min(900, std::max(320, windowWidth / 3));
                const int left = ClampInt(windowWidth / 2 - sampleWidth / 2, 0, windowWidth - 32);
                const int y = ClampInt(160, 48, windowHeight - 24);
                const RECT sample {
                    left,
                    y,
                    ClampInt(left + sampleWidth, left + 32, windowWidth),
                    ClampInt(y + 48, y + 12, windowHeight),
                };
                std::vector<Color> colors = SampleGrid(memDc, sample);
                if (colors.size() >= 24) {
                    const Color printed = DominantBackgroundColor(colors).color;
                    const float lum = PerceivedLuminance(printed);
                    if (lum >= 0.72f || lum <= 0.38f) {
                        outColor = printed;
                        static std::map<HWND, Color> lastPrintedProbe;
                        if (!lastPrintedProbe.contains(window) ||
                            ColorDistance2(lastPrintedProbe[window], printed) > 9) {
                            std::ostringstream ss;
                            ss << "theme probe print rgb=("
                               << static_cast<int>(printed.r) << ','
                               << static_cast<int>(printed.g) << ','
                               << static_cast<int>(printed.b) << ')';
                            Log(ss.str());
                            lastPrintedProbe[window] = printed;
                        }
                        if (oldBitmap) {
                            SelectObject(memDc, oldBitmap);
                        }
                        DeleteObject(bitmap);
                        DeleteDC(memDc);
                        ReleaseDC(nullptr, screenDc);
                        return true;
                    }
                }
            }
        }
        if (oldBitmap) {
            SelectObject(memDc, oldBitmap);
        }
        if (bitmap) {
            DeleteObject(bitmap);
        }
        if (memDc) {
            DeleteDC(memDc);
        }
        ReleaseDC(nullptr, screenDc);
    }

    MONITORINFO mi {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(taskbar.monitor, &mi)) {
        return false;
    }

    const int sampleWidth = std::min(900, std::max(320, Width(windowRect) / 3));
    const int centerX = (std::max(windowRect.left, mi.rcMonitor.left) +
                         std::min(windowRect.right, mi.rcMonitor.right)) / 2;
    const int left = ClampInt(centerX - sampleWidth / 2, mi.rcMonitor.left, mi.rcMonitor.right - 32);
    const int right = ClampInt(left + sampleWidth, left + 32, mi.rcMonitor.right);

    const int baseTop = std::max({ windowRect.top, taskbar.rect.bottom, mi.rcMonitor.top });
    const int y = ClampInt(baseTop + 110, mi.rcMonitor.top, std::min(windowRect.bottom, mi.rcMonitor.bottom) - 24);
    const RECT sample {
        left,
        y,
        right,
        ClampInt(y + 42, y + 12, std::min(windowRect.bottom, mi.rcMonitor.bottom)),
    };
    if (Width(sample) < 32 || Height(sample) < 12) {
        return false;
    }

    HDC dc = GetDC(nullptr);
    if (!dc) {
        return false;
    }
    std::vector<Color> colors = SampleGrid(dc, sample);
    ReleaseDC(nullptr, dc);
    if (colors.size() < 24) {
        return false;
    }

    outColor = DominantBackgroundColor(colors).color;
    return true;
}

Color ApplyEdgeCorrection(const Taskbar &tb, const Color &color) {
    (void)tb;
    return color;
}

bool UpdateEdgeCalibration(Taskbar &tb, HWND window, bool activeTarget) {
    (void)tb;
    (void)window;
    (void)activeTarget;
    return false;
}

bool StartAnimation(Taskbar &tb, HWND targetWindow, const Color &rawTarget) {
    const bool targetWindowSwitch = !tb.tinted || tb.targetWindow != targetWindow;
    if (tb.targetWindow && tb.targetWindow != targetWindow) {
        tb.correctionR = 0;
        tb.correctionG = 0;
        tb.correctionB = 0;
        tb.lastCalibration = 0;
        tb.pendingSettingsTargetValid = false;
        Log("edge correction reset for target switch");
    }

    Color target = CalibrateColor(ApplyEdgeCorrection(tb, rawTarget));
    const float currentLum = PerceivedLuminance(tb.target);
    const float targetLum = PerceivedLuminance(target);
    const bool fastLuminanceJump =
        (targetLum >= 0.76f && currentLum <= 0.55f) ||
        (targetLum <= 0.34f && currentLum >= 0.55f);
    const bool fastThemeSwitch = targetWindowSwitch || fastLuminanceJump;
    UpdateShellThemeForTaskbarColor(target, fastThemeSwitch);
    // Ignore tiny sampling noise; restarting a 150 ms animation for every small
    // pixel fluctuation is visible as taskbar flicker during system UI changes.
    const bool settingsTarget = IsWindowsSettingsWindow(targetWindow);
    const int noiseThreshold = settingsTarget ? 9 : 100;
    const bool settingsNeutralPending =
        settingsTarget &&
        tb.pendingSettingsTargetValid &&
        IsNeutralDarkColor(tb.pendingSettingsTarget) &&
        !IsNeutralDarkColor(target) &&
        MaxColorChannel(target) <= 64 &&
        ColorDistance2(tb.pendingSettingsTarget, target) <= kSettingsNeutralDriftDistance2;
    if (ColorDistance2(tb.target, target) < noiseThreshold && tb.tinted && !settingsNeutralPending) {
        tb.targetWindow = targetWindow;
        tb.rawTarget = rawTarget;
        tb.pendingSettingsTargetValid = false;
        SyncTtbMaximizedAppearance(tb, target);
        return false;
    }
    if (settingsTarget && tb.tinted) {
        const DWORD now = GetTickCount();
        if (settingsNeutralPending) {
            if (now - tb.pendingSettingsTargetAt < kSettingsSampleSettleMs) {
                return false;
            }
            target = tb.pendingSettingsTarget;
        } else if (!tb.pendingSettingsTargetValid ||
            ColorDistance2(tb.pendingSettingsTarget, target) >= noiseThreshold) {
            tb.pendingSettingsTarget = target;
            tb.pendingSettingsTargetAt = now;
            tb.pendingSettingsTargetValid = true;
            return false;
        }
        if (now - tb.pendingSettingsTargetAt < kSettingsSampleSettleMs) {
            return false;
        }
    }
    if (settingsTarget) {
        tb.from = target;
        tb.current = target;
        tb.target = target;
        tb.targetWindow = targetWindow;
        tb.rawTarget = rawTarget;
        tb.animating = false;
        tb.tinted = true;
        tb.pendingSettingsTargetValid = false;
        tb.restoreReassertUntil = 0;
        tb.lastRestoreReassertAt = 0;
        SyncTtbMaximizedAppearance(tb, target);
        ApplyTaskbarColor(tb, tb.current);
        return false;
    }
    // Keep TranslucentTB's original desktop and maximized rules untouched.
    // ExplorerTAP owns the per-taskbar sampled color for the whole runtime.
    const bool lightTransition =
        PerceivedLuminance(target) >= 0.68f &&
        PerceivedLuminance(tb.current) <= 0.62f;
    tb.from = tb.current;
    tb.target = target;
    tb.targetWindow = targetWindow;
    tb.rawTarget = rawTarget;
    tb.animStart = GetTickCount();
    tb.animDuration = lightTransition ? kLightAnimDurationMs : kAnimDurationMs;
    tb.animating = true;
    tb.tinted = true;
    tb.pendingSettingsTargetValid = false;
    tb.restoreReassertUntil = 0;
    tb.lastRestoreReassertAt = 0;
    SyncTtbMaximizedAppearance(tb, target);
    if (lightTransition) {
        Log("light taskbar transition duration=" + std::to_string(tb.animDuration));
    }
    SetTimer(g_window, kAnimTimer, kAnimIntervalMs, nullptr);
    return true;
}

void ApplyTaskbarColor(Taskbar &tb, const Color &color) {
    if (!IsTranslucentTbActive()) {
        Log("TranslucentTB not active; skipped taskbar color apply");
        return;
    }
    if (!ApplyTranslucentTbCom(tb, color)) {
        // Do not fall back to TranslucentTB preview. A failed ExplorerTAP
        // write must not hand runtime ownership to a second renderer.
        Log("ExplorerTAP color apply failed; keeping current taskbar state");
        return;
    }
    ApplyTaskbarIconTheme(tb, &color);

    static std::map<HWND, DWORD> lastMeasured;
    const DWORD now = GetTickCount();
    if (now - lastMeasured[tb.hwnd] > 1000) {
        lastMeasured[tb.hwnd] = now;
        const Color actual = SampleTaskbarBackground(tb);
        std::ostringstream ss;
        ss << "render measurement target=(" << static_cast<int>(color.r) << ','
           << static_cast<int>(color.g) << ',' << static_cast<int>(color.b)
           << ") actual=(" << static_cast<int>(actual.r) << ','
           << static_cast<int>(actual.g) << ',' << static_cast<int>(actual.b) << ')';
        Log(ss.str());
    }
}

void ReassertTintedTaskbars() {
    for (auto &[_, tb] : g_taskbars) {
        if (tb.tinted && IsWindow(tb.hwnd)) {
            ApplyTaskbarColor(tb, tb.current);
        }
    }
}

bool ReassertTaskbarColor(Taskbar &tb) {
    if (!IsTranslucentTbActive()) {
        return false;
    }
    // Reassert only through ExplorerTAP so TTB never competes for ownership.
    const bool applied = ApplyTranslucentTbCom(tb, tb.current);
    if (applied) {
        ApplyTaskbarIconTheme(tb, &tb.current);
    }
    return applied;
}

bool IsTranslucentTbActive() {
    if (!g_ttbWindow || !IsWindow(g_ttbWindow)) {
        g_ttbWindow = FindTranslucentTbWindow();
    }
    return g_ttbWindow && IsWindow(g_ttbWindow);
}

void EvaluateState() {
    if (g_taskbarStateUpdating) {
        PostMessageW(g_window, kRefreshMessage, 0, 0);
        return;
    }
    ScopedTaskbarStateUpdate stateUpdate;

    if (g_taskbars.empty()) {
        RefreshTaskbars();
    }
    if (g_moveSizeTransparentMode && g_moveSizeWindow && IsWindow(g_moveSizeWindow)) {
        for (auto &[_, tb] : g_taskbars) {
            if (tb.tinted) {
                Log("[MOVESIZE] transparent while dragging");
                RestoreTaskbar(tb, false);
            }
        }
        return;
    }
    HWND foreground = GetForegroundWindow();
    const bool interactionProtected = IsTaskbarInteractionProtected();
    const bool usable = IsUsableForegroundWindow(foreground);
    const bool maximized = usable && IsZoomed(foreground);
    const bool showDesktopLike = IsShowDesktopLikeForeground(foreground);
    static HWND lastLoggedForeground = nullptr;
    static bool lastLoggedMaximized = false;
    if (foreground != lastLoggedForeground || maximized != lastLoggedMaximized) {
        lastLoggedForeground = foreground;
        lastLoggedMaximized = maximized;
        RECT rc {};
        GetWindowRect(foreground, &rc);
        std::ostringstream ss;
        ss << "foreground hwnd=0x" << std::hex << reinterpret_cast<uintptr_t>(foreground)
           << std::dec << " class=" << Narrow(WindowClassName(foreground))
           << " title=" << Narrow(WindowTitle(foreground))
           << " usable=" << usable
           << " maximized=" << maximized
           << " desktop=" << IsDesktopForeground(foreground)
           << " showDesktopLike=" << showDesktopLike
           << " taskbarTransient=" << IsTaskbarTransientWindow(foreground)
           << " interactionProtected=" << interactionProtected
           << " rect=(" << rc.left << ',' << rc.top << ',' << rc.right << ',' << rc.bottom << ')';
        Log(ss.str());
    }

    bool anyAnimated = false;
    for (auto &[_, tb] : g_taskbars) {
        if (!IsWindow(tb.hwnd) || !tb.top) {
            RestoreTaskbar(tb);
            continue;
        }

        const bool shellTransition = IsTaskbarTransientWindow(foreground);
        const bool shellHold = shellTransition && tb.tinted;
        const bool interactionHold = interactionProtected && tb.tinted;
        const bool moveSizeHold = IsMoveSizeHoldForTaskbar(tb);

        if (showDesktopLike) {
            const bool desktopFg = IsDesktopForeground(foreground);
            HWND stableTarget = tb.tinted ? StableMaximizedWindow(tb) : nullptr;
            const bool cachedTargetGone = CachedMaximizedTargetGone(tb);
            if (tb.tinted && (stableTarget || !cachedTargetGone)) {
                // App switching and taskbar icon clicks can briefly foreground
                // the desktop or an off-screen staging window while another
                // maximized window is still present. Keep the current immersive
                // brush until the real foreground settles instead of flashing
                // the desktop transparent brush.
                tb.targetWindow = nullptr;
                tb.restorePending = false;
                tb.restorePendingAt = 0;
                tb.animating = false;
                if (GetTickCount() - tb.lastAppliedAt >= kInteractionReapplyIntervalMs) {
                    ReassertTaskbarColor(tb);
                }
                SetTimer(g_window, kAnimTimer, kAnimIntervalMs, nullptr);
                Log(desktopFg ?
                    "[RESTORE] hold during transient desktop foreground" :
                    "[RESTORE] hold during show-desktop-like app switch");
                continue;
            }

            tb.targetWindow = nullptr;
            tb.lastMaximizedWindow = nullptr;
            tb.restorePending = false;
            tb.restorePendingAt = 0;
            tb.animating = false;
            if (tb.tinted) {
                Log("[RESTORE] immediate (show desktop state)");
                RestoreTaskbar(tb);
            } else {
                tb.restoreReassertUntil = GetTickCount() + kDefaultReassertDurationMs;
                SetTimer(g_window, kAnimTimer, kAnimIntervalMs, nullptr);
            }
            continue;
        }

        // Explorer/TranslucentTB can replace the brush for the whole settle
        // period after a taskbar click. Do not sample the transient surface or
        // start a new animation during that period; keep asserting the color
        // already rendered for this taskbar.
        //
        // Exception: a shell transition (taskbar / tray overflow / task view owns
        // the foreground) must NOT keep the immersive color when there is no
        // maximized target left. Otherwise minimizing the last maximized window
        // leaves the taskbar colored until the user clicks the desktop, because
        // the foreground briefly parks on Shell_TrayWnd and shellHold sticks.
        if (shellHold) {
            if (StableMaximizedWindow(tb)) {
                tb.restorePending = false;
                if (GetTickCount() - tb.lastAppliedAt >= kInteractionReapplyIntervalMs) {
                    ReassertTaskbarColor(tb);
                }
                SetTimer(g_window, kAnimTimer, kAnimIntervalMs, nullptr);
                continue;
            }
            // No maximized target remains (cache agrees the last one minimized):
            // fall through to the restore path so the taskbar goes transparent
            // without waiting for the foreground to land on Progman.
        } else if (interactionHold || moveSizeHold) {
            tb.restorePending = false;
            if (GetTickCount() - tb.lastAppliedAt >= kInteractionReapplyIntervalMs) {
                ReassertTaskbarColor(tb);
            }
            SetTimer(g_window, kAnimTimer, kAnimIntervalMs, nullptr);
            continue;
        }

        HWND targetWindow = nullptr;
        if (maximized &&
            MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST) == tb.monitor) {
            if (tb.lastMaximizedWindow && tb.lastMaximizedWindow != foreground) {
                tb.lastMaximizedWindow = nullptr;
                tb.lastMaximizedSeen = 0;
                Log("[HOLD] released: foreground maximized target changed");
            }
            if (tb.tinted && tb.targetWindow && tb.targetWindow != foreground) {
                SetShellTheme(g_originalSystemUsesLightTheme, "foreground-maximized-reset", true);
                Log("shell theme reset before foreground maximized target");
            }
            targetWindow = foreground;
        } else if (shellTransition || interactionProtected || IsDesktopForeground(foreground) ||
                   (foreground && !maximized && !showDesktopLike &&
                    MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST) == tb.monitor)) {
            // Use the cached/maximized window while a small foreground window is
            // above it. The taskbar still visually touches the maximized window
            // behind the popup, so immersion should remain attached to that
            // background target. A different foreground maximized window is
            // handled by the branch above and clears the stale cache first.
            targetWindow = CachedMaximizedWindowBehindForeground(tb, foreground) ?
                tb.lastMaximizedWindow :
                StableMaximizedWindow(tb);
        }

        if (tb.tinted && !targetWindow && foreground && foreground != tb.targetWindow &&
            IsUsableForegroundWindow(foreground) && !IsTaskbarTransientWindow(foreground)) {
            static std::map<HWND, HWND> lastThemeResetForegroundByTaskbar;
            if (lastThemeResetForegroundByTaskbar[tb.hwnd] != foreground) {
                SetShellTheme(g_originalSystemUsesLightTheme, "foreground-target-reset", true);
                lastThemeResetForegroundByTaskbar[tb.hwnd] = foreground;
                Log("shell theme reset before new foreground target");
            }
        }
        
        // Enhanced state logging per taskbar
        static std::map<HWND, HWND> lastLoggedTargetByTaskbar;
        static std::map<HWND, bool> lastLoggedTintedByTaskbar;
        static std::map<HWND, bool> lastLoggedRestorePendingByTaskbar;
        
        const bool stateChanged = (targetWindow != lastLoggedTargetByTaskbar[tb.hwnd]) ||
                                  (tb.tinted != lastLoggedTintedByTaskbar[tb.hwnd]) ||
                                  (tb.restorePending != lastLoggedRestorePendingByTaskbar[tb.hwnd]);
        
        if (stateChanged) {
            std::ostringstream ss;
            ss << "[STATE] taskbar=0x" << std::hex << reinterpret_cast<uintptr_t>(tb.hwnd) << std::dec
               << " target=0x" << std::hex << reinterpret_cast<uintptr_t>(targetWindow) << std::dec
               << " tinted=" << tb.tinted
               << " restorePending=" << tb.restorePending
               << " animating=" << tb.animating;
            if (targetWindow) {
                ss << " targetClass=" << Narrow(WindowClassName(targetWindow))
                   << " zoomed=" << IsZoomed(targetWindow)
                   << " visible=" << IsWindowVisible(targetWindow)
                   << " iconic=" << IsIconic(targetWindow);
            }
            if (tb.lastMaximizedWindow) {
                ss << " cached=0x" << std::hex << reinterpret_cast<uintptr_t>(tb.lastMaximizedWindow) << std::dec
                   << " age=" << (GetTickCount() - tb.lastMaximizedSeen) << "ms";
            }
            Log(ss.str());
            
            lastLoggedTargetByTaskbar[tb.hwnd] = targetWindow;
            lastLoggedTintedByTaskbar[tb.hwnd] = tb.tinted;
            lastLoggedRestorePendingByTaskbar[tb.hwnd] = tb.restorePending;
        }

        if (targetWindow) {
            tb.restorePending = false;

            const int desiredSeamHeight =
                targetWindow == foreground ? kSeamCoverHeightActive : kSeamCoverHeightInactive;
            if (tb.seamCoverHeight != desiredSeamHeight) {
                tb.seamCoverHeight = desiredSeamHeight;
                std::ostringstream ss;
                ss << "seam cover height=" << tb.seamCoverHeight
                   << " taskbar=0x" << std::hex << reinterpret_cast<uintptr_t>(tb.hwnd)
                   << " active=" << (targetWindow == foreground);
                Log(ss.str());
            }

            if (IsWindowInMinimizeTransition(targetWindow)) {
                // Once minimize animation starts, freeze the last stable color.
                // Sampling the shrinking window produces a visible flash before
                // the taskbar returns to the desktop's transparent state.
                tb.animating = false;
                if (GetTickCount() - tb.lastAppliedAt >= kInteractionReapplyIntervalMs) {
                    ReassertTaskbarColor(tb);
                }
                SetTimer(g_window, kAnimTimer, kAnimIntervalMs, nullptr);
                continue;
            }

            Color target {};
            if (ComputeRobustTopColor(targetWindow, tb, target)) {
                const Color calibratedTarget = CalibrateColor(target);
                static std::map<HWND, Color> lastLoggedSampleByTaskbar;
                if (!lastLoggedSampleByTaskbar.contains(tb.hwnd) ||
                    ColorDistance2(lastLoggedSampleByTaskbar[tb.hwnd], target) > 9) {
                    std::ostringstream ss;
                    ss << "sample color rgb=(" << static_cast<int>(target.r) << ','
                       << static_cast<int>(target.g) << ','
                       << static_cast<int>(target.b) << ") targets=" << tb.paintTargets.size();
                    Log(ss.str());
                    lastLoggedSampleByTaskbar[tb.hwnd] = target;
                }
                UpdateShellThemeForTaskbarColor(calibratedTarget, true);
                if (StartAnimation(tb, targetWindow, target)) {
                    anyAnimated = true;
                } else if (GetTickCount() - tb.lastAppliedAt >= kReapplyIntervalMs) {
                    // Explorer/TB may reset the XAML brush during tray and system UI animations.
                    ApplyTaskbarColor(tb, tb.current);
                } else if (!tb.animating && UpdateEdgeCalibration(tb, targetWindow, targetWindow == foreground)) {
                    StartAnimation(tb, targetWindow, target);
                    anyAnimated = true;
                }
            }
        } else if (tb.tinted) {
            const DWORD now = GetTickCount();
            const bool desktopFg = IsDesktopForeground(foreground);
            const bool shellTransient = IsTaskbarTransientWindow(foreground);
            const bool cachedTargetGone = CachedMaximizedTargetGone(tb);

            if (g_minimizingWindow && cachedTargetGone) {
                Log("[RESTORE] immediate (minimized target gone)");
                RestoreTaskbar(tb);
            } else if (desktopFg) {
                // The desktop is definitive: no app-switch gap can be mistaken
                // for the final state, so clear the brush immediately.
                Log("[RESTORE] immediate (desktop foreground)");
                RestoreTaskbar(tb);
            } else if (shellTransient && cachedTargetGone) {
                // A shell window after minimizing is definitive too once the
                // last maximized target is gone. Do not let click-settle
                // protection keep the immersive color alive in this case.
                Log("[RESTORE] immediate (shell foreground, cached target gone)");
                RestoreTaskbar(tb);
            } else if (interactionProtected) {
                tb.restorePending = false;
                if (now - tb.lastAppliedAt >= kInteractionReapplyIntervalMs) {
                    ReassertTaskbarColor(tb);
                }
                SetTimer(g_window, kAnimTimer, kAnimIntervalMs, nullptr);
            } else if (!tb.restorePending) {
                // During app switching, the old maximized window can disappear
                // for a few frames before the new one is enumerated. Keep the
                // current opaque brush; never flash transparent in that gap.
                tb.animating = false;
                tb.restorePending = true;
                tb.restorePendingAt = now;
                ReassertTaskbarColor(tb);
                SetTimer(g_window, kAnimTimer, kAnimIntervalMs, nullptr);
                Log("[RESTORE] hold started during target gap for taskbar=0x" +
                    std::to_string(reinterpret_cast<uintptr_t>(tb.hwnd)));
            } else if (!cachedTargetGone) {
                // The cached window is still maximized, so this is an app
                // transition rather than a real restore-to-desktop state.
                tb.restorePending = false;
                ReassertTaskbarColor(tb);
                SetTimer(g_window, kAnimTimer, kAnimIntervalMs, nullptr);
            } else if (now - tb.restorePendingAt >= kRestoreDebounceMs) {
                Log("[RESTORE] executing restore after confirmed target loss for taskbar=0x" +
                    std::to_string(reinterpret_cast<uintptr_t>(tb.hwnd)));
                RestoreTaskbar(tb);
            } else {
                ReassertTaskbarColor(tb);
                SetTimer(g_window, kAnimTimer, kAnimIntervalMs, nullptr);
            }
        }
    }

    if (anyAnimated) {
        SetTimer(g_window, kAnimTimer, kAnimIntervalMs, nullptr);
    }

    // Desktop appearance is no longer modified per transition. TTB keeps its
    // original clear fallback active throughout the process lifetime.
}

void TickAnimation() {
    if (g_taskbarStateUpdating) {
        return;
    }
    ScopedTaskbarStateUpdate stateUpdate;

    bool stillAnimating = false;
    const DWORD now = GetTickCount();
    const bool taskbarInteraction = IsShellInteractionActive(GetForegroundWindow()) ||
                                    IsTaskbarInteractionProtected();
    const bool moveSizeActive =
        g_moveSizeTransparentMode && g_moveSizeWindow && IsWindow(g_moveSizeWindow);

    for (auto &[_, tb] : g_taskbars) {
        if (moveSizeActive) {
            tb.animating = false;
            tb.restorePending = false;
            tb.restorePendingAt = 0;
            if (tb.tinted) {
                Log("[MOVESIZE] transparent during animation tick");
                RestoreTaskbar(tb, false);
            }
            stillAnimating = true;
            continue;
        }

        if (!tb.tinted && static_cast<LONG>(tb.restoreReassertUntil - now) > 0) {
            if (now - tb.lastRestoreReassertAt >= kDefaultReassertIntervalMs) {
                ReassertTaskbarDefault(tb);
                tb.lastRestoreReassertAt = now;
            }
            stillAnimating = true;
            continue;
        }

        const bool appearanceHold = tb.tinted && taskbarInteraction;
        if (appearanceHold) {
            tb.animating = false;
            if (now - tb.lastAppliedAt >= kInteractionReapplyIntervalMs) {
                ReassertTaskbarColor(tb);
            }
            stillAnimating = true;
            continue;
        }

        if (tb.animating) {
            // Once target discovery starts restoring, the old animation must not
            // write another opaque ExplorerTAP frame or re-arm a backend refresh.
            if (tb.restorePending || !tb.targetWindow ||
                !IsWindow(tb.targetWindow) || !IsZoomed(tb.targetWindow)) {
                tb.animating = false;
                continue;
            }

            const DWORD elapsed = now - tb.animStart;
            const DWORD duration = std::max<DWORD>(1, tb.animDuration);
            const double t = std::min(1.0, static_cast<double>(elapsed) / static_cast<double>(duration));
            const double eased = 1.0 - std::pow(1.0 - t, 3.0);
            tb.current = Lerp(tb.from, tb.target, eased);
            ApplyTaskbarColor(tb, tb.current);

            if (t < 1.0) {
                stillAnimating = true;
            } else {
                tb.current = tb.target;
                tb.animating = false;
                ApplyTaskbarColor(tb, tb.current);
                SyncTtbMaximizedAppearance(tb, tb.current);
                if (!tb.restorePending && tb.targetWindow &&
                    UpdateEdgeCalibration(tb, tb.targetWindow,
                                          tb.targetWindow == GetForegroundWindow())) {
                    StartAnimation(tb, tb.targetWindow, tb.rawTarget);
                    stillAnimating = true;
                }
            }
            continue;
        }

        const bool holdAppearance = tb.tinted && taskbarInteraction;
        if (holdAppearance) {
            // Keep asserting after the click too: TranslucentTB can apply its
            // desktop state during the tail of the taskbar activation animation.
            if (now - tb.lastAppliedAt >= kInteractionReapplyIntervalMs) {
                ReassertTaskbarColor(tb);
            }
            stillAnimating = true;
        }
    }

    if (!stillAnimating) {
        KillTimer(g_window, kAnimTimer);
    }
}

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG, LONG, DWORD, DWORD) {
    if (!g_window) {
        return;
    }
    if (event == EVENT_SYSTEM_MOVESIZESTART) {
        static bool s_loggedFirstMoveSize = false;
        if (!s_loggedFirstMoveSize) {
            s_loggedFirstMoveSize = true;
            Log("[HOOK] first MOVESIZESTART received - drag protection alive");
        }
        g_moveSizeWindow = hwnd;
        g_moveSizeTransparentMode = IsZoomed(hwnd);
        for (auto &[_, tb] : g_taskbars) {
            if (hwnd && (hwnd == tb.targetWindow || hwnd == tb.lastMaximizedWindow)) {
                g_moveSizeTransparentMode = true;
                break;
            }
        }
        Log("[MOVESIZE] start hwnd=0x" + std::to_string(reinterpret_cast<uintptr_t>(hwnd)) +
            " transparentMode=" + (g_moveSizeTransparentMode ? "1" : "0"));
        for (auto &[_, tb] : g_taskbars) {
            if (g_moveSizeTransparentMode && tb.tinted) {
                Log("[MOVESIZE] transparent on drag start");
                RestoreTaskbar(tb, false);
            } else if (!g_moveSizeTransparentMode && tb.tinted) {
                Log("[MOVESIZE] keep immersive while dragging floating window");
                ReassertTaskbarColor(tb);
            }
        }
    } else if (event == EVENT_SYSTEM_MOVESIZEEND) {
        Log("[MOVESIZE] end hwnd=0x" + std::to_string(reinterpret_cast<uintptr_t>(hwnd)));
        g_moveSizeWindow = nullptr;
        g_moveSizeTransparentMode = false;
        PostMessageW(g_window, kRefreshMessage, 0, 0);
    } else if (event == EVENT_SYSTEM_MINIMIZESTART) {
        g_minimizingWindow = hwnd;
        Log("[MIN] start hwnd=0x" + std::to_string(reinterpret_cast<uintptr_t>(hwnd)));
    } else if (event == EVENT_SYSTEM_MINIMIZEEND) {
        if (g_minimizingWindow == hwnd) {
            g_minimizingWindow = nullptr;
        }
        Log("[MIN] end hwnd=0x" + std::to_string(reinterpret_cast<uintptr_t>(hwnd)));
    }
    if (event == EVENT_SYSTEM_FOREGROUND &&
        IsTaskbarTransientWindow(hwnd) &&
        !IsDesktopForeground(hwnd)) {
        ExtendTaskbarInteractionProtection();
    }
    if (event == EVENT_SYSTEM_FOREGROUND ||
        event == EVENT_SYSTEM_MINIMIZESTART ||
        event == EVENT_SYSTEM_MINIMIZEEND ||
        event == EVENT_SYSTEM_MOVESIZESTART ||
        event == EVENT_SYSTEM_MOVESIZEEND ||
        (event == EVENT_OBJECT_LOCATIONCHANGE && hwnd == GetForegroundWindow())) {
        PostMessageW(g_window, kRefreshMessage, 0, 0);
    }
}

void AddTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW nid {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kTrayMessage;
    nid.hIcon = LoadIconW(g_instance, MAKEINTRESOURCE(IDI_MAIN_ICON));
    wcscpy_s(nid.szTip, L"Immersive Top Taskbar");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void RemoveTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW nid {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

// ---- 版本更新检查 ----

// 从 GitHub Releases 的 latest JSON 里提取 tag_name（形如 "v1.2.3" 或 "1.2.3"）。
bool ExtractTagName(const std::string &json, std::string &tag) {
    const std::string key = "\"tag_name\"";
    const size_t kp = json.find(key);
    if (kp == std::string::npos) {
        return false;
    }
    size_t q1 = json.find('"', kp + key.size());
    if (q1 == std::string::npos) {
        return false;
    }
    size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) {
        return false;
    }
    tag = json.substr(q1 + 1, q2 - q1 - 1);
    return !tag.empty();
}

bool ExtractJsonStringProperty(const std::string &json, size_t start,
                               const std::string &key, std::string &value,
                               size_t *valueEnd = nullptr) {
    const size_t kp = json.find(key, start);
    if (kp == std::string::npos) {
        return false;
    }
    const size_t colon = json.find(':', kp + key.size());
    if (colon == std::string::npos) {
        return false;
    }
    const size_t q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) {
        return false;
    }
    std::string out;
    bool escaping = false;
    for (size_t i = q1 + 1; i < json.size(); ++i) {
        const char ch = json[i];
        if (escaping) {
            out.push_back(ch);
            escaping = false;
            continue;
        }
        if (ch == '\\') {
            escaping = true;
            continue;
        }
        if (ch == '"') {
            value = std::move(out);
            if (valueEnd) {
                *valueEnd = i + 1;
            }
            return true;
        }
        out.push_back(ch);
    }
    return false;
}

bool EndsWithAscii(const std::string &value, const std::string &suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool ExtractInstallerAssetUrl(const std::string &json, std::string &assetName, std::string &assetUrl) {
    size_t pos = 0;
    for (;;) {
        std::string url;
        size_t end = 0;
        if (!ExtractJsonStringProperty(json, pos, "\"browser_download_url\"", url, &end)) {
            return false;
        }
        const size_t nameSearchStart = json.rfind("\"name\"", end);
        std::string name;
        if (nameSearchStart != std::string::npos) {
            ExtractJsonStringProperty(json, nameSearchStart, "\"name\"", name);
        }
        const bool likelyInstaller =
            EndsWithAscii(url, kInstallerAssetSuffix) &&
            (url.find(kInstallerAssetPrefix) != std::string::npos ||
             name.find(kInstallerAssetPrefix) != std::string::npos);
        if (likelyInstaller) {
            assetName = name.empty() ? url.substr(url.find_last_of('/') + 1) : name;
            assetUrl = url;
            return true;
        }
        pos = end;
    }
}

// 把 "v1.2.3" / "1.2.3" 归一成可比较的三元组。
bool ParseSemver(const std::wstring &s, int &a, int &b, int &c) {
    std::wstring t = s;
    if (!t.empty() && (t[0] == L'v' || t[0] == L'V')) {
        t = t.substr(1);
    }
    a = 0; b = 0; c = 0;
    wchar_t *endp = nullptr;
    const unsigned long va = wcstoul(t.c_str(), &endp, 10);
    if (endp == t.c_str()) {
        return false;
    }
    a = static_cast<int>(va);
    if (endp && *endp == L'.') {
        b = static_cast<int>(wcstoul(endp + 1, &endp, 10));
    }
    if (endp && *endp == L'.') {
        c = static_cast<int>(wcstoul(endp + 1, &endp, 10));
    }
    return true;
}

// 同步请求 GitHub API（UTF-8 字符串返回），成功返回响应体，失败返回空串。
std::string HttpGetUtf8(const std::wstring &host, const std::wstring &path) {
    HINTERNET session = WinHttpOpen(L"ImmersiveTopTaskbar/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        return {};
    }
    HINTERNET conn = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) {
        WinHttpCloseHandle(session);
        return {};
    }
    HINTERNET req = WinHttpOpenRequest(conn, L"GET", path.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       WINHTTP_FLAG_SECURE);
    if (!req) {
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return {};
    }
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    WinHttpSetOption(req, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    WinHttpAddRequestHeaders(req, L"User-Agent: ImmersiveTopTaskbar\r\nAccept: application/vnd.github+json\r\n",
                             0xFFFFFFFF, WINHTTP_ADDREQ_FLAG_ADD);

    std::string body;
    BOOL ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) {
        ok = WinHttpReceiveResponse(req, nullptr);
    }
    if (ok) {
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) {
                break;
            }
            std::vector<char> buf(avail);
            DWORD read = 0;
            if (!WinHttpReadData(req, buf.data(), avail, &read) || read == 0) {
                break;
            }
            body.append(buf.data(), read);
        }
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return ok ? body : std::string{};
}

bool ParseHttpsUrl(const std::wstring &url, std::wstring &host, std::wstring &path) {
    constexpr wchar_t prefix[] = L"https://";
    if (url.rfind(prefix, 0) != 0) {
        return false;
    }
    const size_t hostStart = wcslen(prefix);
    const size_t slash = url.find(L'/', hostStart);
    if (slash == std::wstring::npos || slash == hostStart) {
        return false;
    }
    host = url.substr(hostStart, slash - hostStart);
    path = url.substr(slash);
    return !host.empty() && !path.empty();
}

bool HttpDownloadFile(const std::wstring &url, const std::filesystem::path &outputPath) {
    std::wstring host;
    std::wstring path;
    if (!ParseHttpsUrl(url, host, path)) {
        return false;
    }

    HINTERNET session = WinHttpOpen(L"ImmersiveTopTaskbar/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        return false;
    }
    HINTERNET conn = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) {
        WinHttpCloseHandle(session);
        return false;
    }
    HINTERNET req = WinHttpOpenRequest(conn, L"GET", path.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       WINHTTP_FLAG_SECURE);
    if (!req) {
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    WinHttpSetOption(req, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));
    WinHttpAddRequestHeaders(req, L"User-Agent: ImmersiveTopTaskbar\r\n",
                             0xFFFFFFFF, WINHTTP_ADDREQ_FLAG_ADD);

    std::error_code ec;
    std::filesystem::create_directories(outputPath.parent_path(), ec);
    std::ofstream file(outputPath, std::ios::binary | std::ios::trunc);
    if (!file) {
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return false;
    }

    BOOL ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) {
        ok = WinHttpReceiveResponse(req, nullptr);
    }
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (ok) {
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
        ok = status >= 200 && status < 300;
    }
    if (ok) {
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) {
                break;
            }
            std::vector<char> buf(avail);
            DWORD read = 0;
            if (!WinHttpReadData(req, buf.data(), avail, &read) || read == 0) {
                ok = false;
                break;
            }
            file.write(buf.data(), read);
            if (!file) {
                ok = false;
                break;
            }
        }
    }

    file.close();
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    if (!ok) {
        std::filesystem::remove(outputPath, ec);
    }
    return ok;
}

std::wstring ReleasesLatestUrl() {
    return std::wstring(L"https://github.com/") + kUpdateOwner + L"/" + kUpdateRepo + L"/releases/latest";
}

std::wstring RepositoryIssuesNewUrl(const std::wstring &body) {
    return std::wstring(L"https://github.com/") + kUpdateOwner + L"/" + kUpdateRepo +
           L"/issues/new?title=" + PercentEncodeUtf8(kFeedbackIssueTitle) +
           L"&body=" + PercentEncodeUtf8(body);
}

std::wstring FeedbackBody(const std::wstring &text) {
    return std::wstring(L"反馈内容：\r\n") + text +
           L"\r\n\r\n---\r\n版本：ImmersiveTopTaskbar " + kAppVersion +
           L"\r\n系统：Windows 11\r\n";
}

std::wstring FeedbackSubmitTarget(const std::wstring &text, bool &usesEmail) {
    const std::wstring body = FeedbackBody(text);
    if (wcslen(kFeedbackEmail) > 0) {
        usesEmail = true;
        return std::wstring(L"mailto:") + kFeedbackEmail +
               L"?subject=" + PercentEncodeUtf8(std::wstring(L"ImmersiveTopTaskbar ") + kFeedbackIssueTitle) +
               L"&body=" + PercentEncodeUtf8(body);
    }
    usesEmail = false;
    return RepositoryIssuesNewUrl(body);
}

// 从 GitHub Releases latest 拿 tag_name；失败返回空串。
std::wstring FetchLatestReleaseTag() {
    const std::wstring path =
        std::wstring(L"/repos/") + kUpdateOwner + L"/" + kUpdateRepo + L"/releases/latest";
    const std::string json = HttpGetUtf8(L"api.github.com", path);
    if (json.empty()) {
        return {};
    }
    std::string tag;
    if (!ExtractTagName(json, tag)) {
        return {};
    }
    return Widen(tag);
}

struct LatestReleaseInfo {
    std::wstring tag;
    std::wstring installerUrl;
    std::wstring installerName;
};

enum UpdateDialogAction {
    kUpdateDialogCancel = 0,
    kUpdateDialogAutoInstall = 1001,
    kUpdateDialogOpenGithub = 1002,
};

bool FetchLatestReleaseInfo(LatestReleaseInfo &info) {
    const std::wstring path =
        std::wstring(L"/repos/") + kUpdateOwner + L"/" + kUpdateRepo + L"/releases/latest";
    const std::string json = HttpGetUtf8(L"api.github.com", path);
    if (json.empty()) {
        return false;
    }
    std::string tag;
    if (!ExtractTagName(json, tag)) {
        return false;
    }
    std::string assetName;
    std::string assetUrl;
    ExtractInstallerAssetUrl(json, assetName, assetUrl);
    info.tag = Widen(tag);
    info.installerName = Widen(assetName);
    info.installerUrl = Widen(assetUrl);
    return !info.tag.empty();
}

std::filesystem::path UpdateDownloadPath(const LatestReleaseInfo &info) {
    std::wstring fileName = info.installerName;
    if (fileName.empty()) {
        fileName = L"ImmersiveTopTaskbar-Setup-" + info.tag + L".exe";
    }
    for (wchar_t &ch : fileName) {
        if (ch == L'/' || ch == L'\\' || ch == L':' || ch == L'*' ||
            ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|') {
            ch = L'_';
        }
    }
    return LocalAppDataDirectory() / L"ImmersiveTopTaskbar" / L"Updates" / fileName;
}

struct UpdateWindowState {
    Gdiplus::Bitmap *qr1 = nullptr;
    Gdiplus::Bitmap *qr2 = nullptr;
    std::wstring title;
    std::wstring detail;
    bool newer = false;
    bool fetchFailed = false;
    bool canAutoInstall = false;
    int choice = kUpdateDialogCancel;
};

LRESULT CALLBACK UpdateWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_CREATE) {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(lparam);
        auto *state = reinterpret_cast<UpdateWindowState *>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

        HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        const int buttonY = state->newer ? 466 : 466;
        if (state->newer) {
            HWND autoButton = CreateWindowExW(0, L"BUTTON", L"下载更新",
                                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                             236, buttonY, 116, 32,
                                             hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kUpdateDialogAutoInstall)),
                                             g_instance, nullptr);
            HWND githubButton = CreateWindowExW(0, L"BUTTON", L"去 GitHub 下载",
                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                               364, buttonY, 128, 32,
                                               hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kUpdateDialogOpenGithub)),
                                               g_instance, nullptr);
            HWND cancelButton = CreateWindowExW(0, L"BUTTON", L"取消",
                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                               504, buttonY, 82, 32,
                                               hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kUpdateDialogCancel)),
                                               g_instance, nullptr);
            EnableWindow(autoButton, state->canAutoInstall ? TRUE : FALSE);
            SendMessageW(autoButton, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            SendMessageW(githubButton, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            SendMessageW(cancelButton, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        } else {
            HWND githubButton = nullptr;
            if (state->fetchFailed) {
                githubButton = CreateWindowExW(0, L"BUTTON", L"去 GitHub 查看",
                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                               348, buttonY, 130, 32,
                                               hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kUpdateDialogOpenGithub)),
                                               g_instance, nullptr);
            }
            HWND closeButton = CreateWindowExW(0, L"BUTTON", L"关闭",
                                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | (state->fetchFailed ? 0 : BS_DEFPUSHBUTTON),
                                              state->fetchFailed ? 492 : 466, buttonY, 94, 32,
                                              hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kUpdateDialogCancel)),
                                              g_instance, nullptr);
            if (githubButton) {
                SendMessageW(githubButton, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            }
            SendMessageW(closeButton, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
        return 0;
    }
    auto *state = reinterpret_cast<UpdateWindowState *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps {};
        HDC hdc = BeginPaint(hwnd, &ps);
        Gdiplus::Graphics g(hdc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.Clear(Gdiplus::Color(255, 248, 250, 252));

        RECT rc {};
        GetClientRect(hwnd, &rc);
        Gdiplus::FontFamily family(L"Microsoft YaHei UI");
        Gdiplus::Font titleFont(&family, 22.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font bodyFont(&family, 14.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush titleBrush(Gdiplus::Color(255, 24, 31, 42));
        Gdiplus::SolidBrush bodyBrush(Gdiplus::Color(255, 70, 78, 92));
        Gdiplus::StringFormat centered;
        centered.SetAlignment(Gdiplus::StringAlignmentCenter);
        centered.SetLineAlignment(Gdiplus::StringAlignmentNear);

        if (state) {
            Gdiplus::RectF titleRect(24.0f, 24.0f, static_cast<Gdiplus::REAL>(rc.right - 48), 32.0f);
            g.DrawString(state->title.c_str(), -1, &titleFont, titleRect, &centered, &titleBrush);
            Gdiplus::RectF detailRect(36.0f, 62.0f, static_cast<Gdiplus::REAL>(rc.right - 72), 72.0f);
            g.DrawString(state->detail.c_str(), -1, &bodyFont, detailRect, &centered, &bodyBrush);

            const int gap = 28;
            const int qrSize = 210;
            const int total = qrSize * 2 + gap;
            const int left = std::max(24, (static_cast<int>(rc.right) - total) / 2);
            const int top = 146;
            if (state->qr1) {
                g.DrawImage(state->qr1, left, top, qrSize, qrSize);
            }
            if (state->qr2) {
                g.DrawImage(state->qr2, left + qrSize + gap, top, qrSize, qrSize);
            }
            Gdiplus::RectF hintRect(0.0f, static_cast<Gdiplus::REAL>(top + qrSize + 18),
                                    static_cast<Gdiplus::REAL>(rc.right), 24.0f);
            g.DrawString(L"二维码仅用于自愿支持，不影响任何功能。", -1, &bodyFont, hintRect, &centered, &bodyBrush);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_COMMAND:
        if (state) {
            const int id = LOWORD(wparam);
            if (id == kUpdateDialogAutoInstall || id == kUpdateDialogOpenGithub || id == kUpdateDialogCancel) {
                state->choice = id;
                DestroyWindow(hwnd);
                return 0;
            }
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (state) {
            delete state->qr1;
            delete state->qr2;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int ShowUpdateOptionsDialog(const LatestReleaseInfo &info, bool newer, bool fetchFailed = false) {
    if (!EnsureGdiPlus()) {
        MessageBoxW(nullptr, L"更新页面初始化失败。", L"ImmersiveTopTaskbar 更新", MB_OK | MB_ICONWARNING);
        return kUpdateDialogCancel;
    }

    auto *state = new UpdateWindowState;
    state->newer = newer;
    state->fetchFailed = fetchFailed;
    state->canAutoInstall = newer && !info.installerUrl.empty();
    state->qr1 = LoadPngBitmapResource(IDR_DONATE_QR_1);
    state->qr2 = LoadPngBitmapResource(IDR_DONATE_QR_2);
    if (fetchFailed) {
        state->title = L"暂时无法获取 GitHub 发布信息";
        state->detail = L"可能是网络、代理、GitHub 限流或 Releases API 暂时不可用。\r\n你可以稍后重试，或打开 GitHub 手动查看。";
    } else if (newer) {
        state->title = L"检测到新版本";
        state->detail = L"当前版本：" + std::wstring(kAppVersion) +
                        L"\r\n最新版本：" + (info.tag.empty() ? L"未知" : info.tag);
    } else {
        state->title = L"当前已经是最新版本";
        state->detail = L"当前版本：" + std::wstring(kAppVersion) +
                        L"\r\n最新版本：" + (info.tag.empty() ? std::wstring(kAppVersion) : info.tag);
    }

    constexpr wchar_t kUpdateClass[] = L"ImmersiveTopTaskbarUpdateWindow";
    WNDCLASSW wc {};
    wc.lpfnWndProc = UpdateWndProc;
    wc.hInstance = g_instance;
    wc.lpszClassName = kUpdateClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(g_instance, MAKEINTRESOURCE(IDI_MAIN_ICON));
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        kUpdateClass,
        L"ImmersiveTopTaskbar 更新",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        640,
        550,
        g_window,
        nullptr,
        g_instance,
        state);
    if (!hwnd) {
        delete state->qr1;
        delete state->qr2;
        delete state;
        MessageBoxW(nullptr, L"更新页面创建失败。", L"ImmersiveTopTaskbar 更新", MB_OK | MB_ICONWARNING);
        return kUpdateDialogCancel;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg {};
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    const int choice = state->choice;
    delete state;
    return choice;
}

void StartAutoInstallUpdate(const LatestReleaseInfo &info) {
    if (info.installerUrl.empty()) {
        MessageBoxW(nullptr,
                    L"最新 Release 没有找到安装包附件，将打开 GitHub Releases 页面。",
                    L"ImmersiveTopTaskbar 更新",
                    MB_OK | MB_ICONINFORMATION);
        OpenShellTarget(ReleasesLatestUrl());
        return;
    }

    const std::filesystem::path outputPath = UpdateDownloadPath(info);
    std::wstring downloading = L"即将下载：\r\n" + outputPath.wstring() +
                               L"\r\n\r\n下载完成后会询问是否启动安装包。";
    MessageBoxW(nullptr, downloading.c_str(), L"ImmersiveTopTaskbar 更新", MB_OK | MB_ICONINFORMATION);

    if (!HttpDownloadFile(info.installerUrl, outputPath)) {
        MessageBoxW(nullptr,
                    L"自动下载失败，将打开 GitHub Releases 页面供你手动下载。",
                    L"ImmersiveTopTaskbar 更新",
                    MB_OK | MB_ICONWARNING);
        OpenShellTarget(ReleasesLatestUrl());
        return;
    }

    std::wstring done = L"安装包已下载：\r\n" + outputPath.wstring() +
                        L"\r\n\r\n是否现在启动安装包？当前程序会退出以便安装器覆盖文件。";
    if (MessageBoxW(nullptr, done.c_str(), L"ImmersiveTopTaskbar 更新",
                    MB_YESNO | MB_ICONQUESTION) == IDYES) {
        if (OpenShellTarget(outputPath.wstring())) {
            if (g_window) {
                PostMessageW(g_window, WM_CLOSE, 0, 0);
            }
        } else {
            MessageBoxW(nullptr, L"安装包启动失败，请手动打开下载文件。",
                        L"ImmersiveTopTaskbar 更新", MB_OK | MB_ICONWARNING);
        }
    }
}

void OfferUpdateInstall(const LatestReleaseInfo &info, bool newer) {
    const int choice = ShowUpdateOptionsDialog(info, newer);
    if (choice == kUpdateDialogAutoInstall) {
        StartAutoInstallUpdate(info);
        return;
    }
    if (choice == kUpdateDialogOpenGithub) {
        OpenShellTarget(ReleasesLatestUrl());
        return;
    }
}

void CheckForUpdates(bool interactive = false) {
    std::thread([interactive] {
        if (g_updateCheckRunning.exchange(true)) {
            return;
        }
        LatestReleaseInfo info;
        if (!FetchLatestReleaseInfo(info)) {
            if (interactive) {
                const int choice = ShowUpdateOptionsDialog(info, false, true);
                if (choice == kUpdateDialogOpenGithub) {
                    OpenShellTarget(ReleasesLatestUrl());
                }
            }
            g_updateCheckRunning = false;
            return; // 网络失败 / 仓库不存在：静默跳过。
        }
        int la = 0, lb = 0, lc = 0, ca = 0, cb = 0, cc = 0;
        if (!ParseSemver(info.tag, la, lb, lc) || !ParseSemver(kAppVersion, ca, cb, cc)) {
            g_updateCheckRunning = false;
            return;
        }
        const bool newer = (la > ca) || (la == ca && lb > cb) || (la == ca && lb == cb && lc > cc);
        if (!newer && !interactive) {
            g_updateCheckRunning = false;
            return;
        }
        OfferUpdateInstall(info, newer);
        g_updateCheckRunning = false;
    }).detach();
}

bool EnsureGdiPlus() {
    if (g_gdiplusStarted) {
        return true;
    }
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, nullptr) != Gdiplus::Ok) {
        return false;
    }
    g_gdiplusStarted = true;
    return true;
}

Gdiplus::Bitmap *LoadPngBitmapResource(int resourceId) {
    HRSRC resource = FindResourceW(g_instance, MAKEINTRESOURCEW(resourceId), L"PNG");
    if (!resource) {
        return nullptr;
    }
    HGLOBAL loaded = LoadResource(g_instance, resource);
    if (!loaded) {
        return nullptr;
    }
    const DWORD size = SizeofResource(g_instance, resource);
    const void *data = LockResource(loaded);
    if (!data || size == 0) {
        return nullptr;
    }

    HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!copy) {
        return nullptr;
    }
    void *copyData = GlobalLock(copy);
    if (!copyData) {
        GlobalFree(copy);
        return nullptr;
    }
    std::memcpy(copyData, data, size);
    GlobalUnlock(copy);

    IStream *stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(copy, TRUE, &stream)) || !stream) {
        GlobalFree(copy);
        return nullptr;
    }
    Gdiplus::Bitmap *bitmap = Gdiplus::Bitmap::FromStream(stream);
    stream->Release();
    if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok) {
        delete bitmap;
        return nullptr;
    }
    return bitmap;
}

struct DonationWindowState {
    Gdiplus::Bitmap *qr1 = nullptr;
    Gdiplus::Bitmap *qr2 = nullptr;
};

LRESULT CALLBACK DonationWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_CREATE) {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 0;
    }
    auto *state = reinterpret_cast<DonationWindowState *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps {};
        HDC hdc = BeginPaint(hwnd, &ps);
        Gdiplus::Graphics g(hdc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.Clear(Gdiplus::Color(255, 248, 250, 252));

        RECT rc {};
        GetClientRect(hwnd, &rc);
        Gdiplus::FontFamily titleFamily(L"Microsoft YaHei UI");
        Gdiplus::Font titleFont(&titleFamily, 22.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font bodyFont(&titleFamily, 14.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush titleBrush(Gdiplus::Color(255, 24, 31, 42));
        Gdiplus::SolidBrush bodyBrush(Gdiplus::Color(255, 70, 78, 92));
        Gdiplus::StringFormat centered;
        centered.SetAlignment(Gdiplus::StringAlignmentCenter);

        Gdiplus::RectF titleRect(0.0f, 24.0f, static_cast<Gdiplus::REAL>(rc.right), 32.0f);
        g.DrawString(L"支持 ImmersiveTopTaskbar", -1, &titleFont, titleRect, &centered, &titleBrush);
        Gdiplus::RectF bodyRect(0.0f, 58.0f, static_cast<Gdiplus::REAL>(rc.right), 24.0f);
        g.DrawString(L"喜欢这个小工具的话，可以扫码请作者喝杯咖啡。", -1, &bodyFont, bodyRect, &centered, &bodyBrush);

        const int gap = 34;
        const int qrSize = 260;
        const int total = qrSize * 2 + gap;
        const int left = std::max(24, (static_cast<int>(rc.right) - total) / 2);
        const int top = 106;
        if (state && state->qr1) {
            g.DrawImage(state->qr1, left, top, qrSize, qrSize);
        }
        if (state && state->qr2) {
            g.DrawImage(state->qr2, left + qrSize + gap, top, qrSize, qrSize);
        }

        Gdiplus::RectF hintRect(0.0f, static_cast<Gdiplus::REAL>(top + qrSize + 22),
                                static_cast<Gdiplus::REAL>(rc.right), 24.0f);
        g.DrawString(L"二维码仅用于自愿支持，不影响任何功能。", -1, &bodyFont, hintRect, &centered, &bodyBrush);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (state) {
            delete state->qr1;
            delete state->qr2;
            delete state;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

void ShowDonation() {
    if (!EnsureGdiPlus()) {
        MessageBoxW(nullptr, L"捐赠窗口初始化失败。", L"支持作者", MB_OK | MB_ICONWARNING);
        return;
    }

    auto *state = new DonationWindowState;
    state->qr1 = LoadPngBitmapResource(IDR_DONATE_QR_1);
    state->qr2 = LoadPngBitmapResource(IDR_DONATE_QR_2);
    if (!state->qr1 || !state->qr2) {
        delete state->qr1;
        delete state->qr2;
        delete state;
        MessageBoxW(nullptr, L"没有找到内置捐赠二维码资源。", L"支持作者", MB_OK | MB_ICONWARNING);
        return;
    }

    constexpr wchar_t kDonationClass[] = L"ImmersiveTopTaskbarDonationWindow";
    WNDCLASSW wc {};
    wc.lpfnWndProc = DonationWndProc;
    wc.hInstance = g_instance;
    wc.lpszClassName = kDonationClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(g_instance, MAKEINTRESOURCE(IDI_MAIN_ICON));
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        kDonationClass,
        L"支持作者",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        660,
        470,
        g_window,
        nullptr,
        g_instance,
        state);
    if (!hwnd) {
        delete state->qr1;
        delete state->qr2;
        delete state;
        MessageBoxW(nullptr, L"捐赠窗口创建失败。", L"支持作者", MB_OK | MB_ICONWARNING);
        return;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg {};
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

struct FeedbackWindowState {
    HWND edit = nullptr;
};

std::wstring GetWindowTextValue(HWND hwnd) {
    const int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) {
        return {};
    }
    std::wstring value(static_cast<size_t>(len + 1), L'\0');
    GetWindowTextW(hwnd, value.data(), len + 1);
    value.resize(wcslen(value.c_str()));
    return value;
}

bool SubmitFeedback(HWND hwnd, FeedbackWindowState *state) {
    if (!state || !state->edit) {
        return false;
    }
    const std::wstring text = GetWindowTextValue(state->edit);
    if (!HasVisibleText(text)) {
        MessageBoxW(hwnd, L"先写一点反馈内容吧。", L"意见反馈", MB_OK | MB_ICONINFORMATION);
        SetFocus(state->edit);
        return false;
    }

    bool usesEmail = false;
    const std::wstring target = FeedbackSubmitTarget(text, usesEmail);
    if (!usesEmail) {
        const int choice = MessageBoxW(
            hwnd,
            L"此开源构建还没有配置反馈邮箱，将改为打开 GitHub Issues 页面。\r\n\r\n"
            L"提交前请检查内容，避免带上窗口标题、本地路径或其他隐私信息。",
            L"意见反馈",
            MB_OKCANCEL | MB_ICONINFORMATION);
        if (choice != IDOK) {
            return false;
        }
    } else {
        MessageBoxW(
            hwnd,
            L"将打开你的默认邮件客户端，收件人、标题和内容会自动填好。\r\n\r\n"
            L"请在邮件客户端里确认发送。",
            L"意见反馈",
            MB_OK | MB_ICONINFORMATION);
    }

    if (!OpenShellTarget(target)) {
        MessageBoxW(hwnd, L"没有打开反馈提交入口，请检查默认邮件客户端或浏览器设置。", L"意见反馈",
                    MB_OK | MB_ICONWARNING);
        return false;
    }
    DestroyWindow(hwnd);
    return true;
}

LRESULT CALLBACK FeedbackWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto *state = reinterpret_cast<FeedbackWindowState *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(lparam);
        state = reinterpret_cast<FeedbackWindowState *>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

        HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HWND label = CreateWindowExW(0, L"STATIC",
                                     L"写下你遇到的问题或建议，提交前请避免填写隐私信息。",
                                     WS_CHILD | WS_VISIBLE,
                                     18, 18, 466, 24,
                                     hwnd, nullptr, g_instance, nullptr);
        state->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                          ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                                      18, 48, 466, 218,
                                      hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFeedbackEditId)), g_instance, nullptr);
        HWND submit = CreateWindowExW(0, L"BUTTON", L"提交反馈",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                      294, 284, 92, 30,
                                      hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFeedbackSubmitId)), g_instance, nullptr);
        HWND cancel = CreateWindowExW(0, L"BUTTON", L"取消",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                      394, 284, 90, 30,
                                      hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFeedbackCancelId)), g_instance, nullptr);
        SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(state->edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(submit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(cancel, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(state->edit, EM_SETLIMITTEXT, 3000, 0);
        SetFocus(state->edit);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == kFeedbackSubmitId) {
            SubmitFeedback(hwnd, state);
            return 0;
        }
        if (LOWORD(wparam) == kFeedbackCancelId) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        delete state;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void ShowFeedback(HWND owner) {
    auto *state = new FeedbackWindowState;
    constexpr wchar_t kFeedbackClass[] = L"ImmersiveTopTaskbarFeedbackWindow";
    WNDCLASSW wc {};
    wc.lpfnWndProc = FeedbackWndProc;
    wc.hInstance = g_instance;
    wc.lpszClassName = kFeedbackClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(g_instance, MAKEINTRESOURCE(IDI_MAIN_ICON));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        kFeedbackClass,
        L"意见反馈",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        526,
        370,
        owner,
        nullptr,
        g_instance,
        state);
    if (!hwnd) {
        delete state;
        MessageBoxW(owner, L"反馈窗口创建失败。", L"意见反馈", MB_OK | MB_ICONWARNING);
        return;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
}

void ShowTrayMenu(HWND hwnd) {
    POINT pt {};
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kTrayMenuCheckUpdate, L"检查更新");
    AppendMenuW(menu, MF_STRING, kTrayMenuFeedback, L"意见反馈");
    AppendMenuW(menu, MF_STRING, kTrayMenuAbout, L"关于");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayMenuExit, L"退出");
    SetForegroundWindow(hwnd);
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    if (cmd == kTrayMenuCheckUpdate) {
        CheckForUpdates(true);
    } else if (cmd == kTrayMenuFeedback) {
        ShowFeedback(hwnd);
    } else if (cmd == kTrayMenuAbout) {
        std::wstring about = L"ImmersiveTopTaskbar " + std::wstring(kAppVersion) +
                             L"\r\n\r\n最大化窗口时让顶部任务栏沉浸着色。";
        MessageBoxW(hwnd, about.c_str(), L"关于 ImmersiveTopTaskbar", MB_OK | MB_ICONINFORMATION);
    } else if (cmd == kTrayMenuExit) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == g_taskbarCreatedMessage) {
        RefreshTaskbars();
        RefreshExternalBackends();
        AddTrayIcon(hwnd);
        SetTimer(hwnd, kStateTimer, kStateIntervalMs, nullptr);
        PostMessageW(hwnd, kRefreshMessage, 0, 0);
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        AddTrayIcon(hwnd);
        RefreshTaskbars();
        RefreshExternalBackends();
        SetTimer(hwnd, kStateTimer, kStateIntervalMs, nullptr);
        PostMessageW(hwnd, kRefreshMessage, 0, 0);
        return 0;
    case WM_TIMER:
        if (wparam == kStateTimer) {
            if (!g_taskbarStateUpdating) {
                EvaluateState();
            }
        } else if (wparam == kAnimTimer) {
            if (!g_taskbarStateUpdating) {
                TickAnimation();
            }
        }
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        if ((msg == WM_SETTINGCHANGE && g_shellThemeBroadcasting) || g_taskbarStateUpdating) {
            PostMessageW(hwnd, kRefreshMessage, 0, 0);
            return 0;
        }
        RefreshTaskbars();
        RefreshExternalBackends();
        PostMessageW(hwnd, kRefreshMessage, 0, 0);
        return 0;
    case kRefreshMessage:
        if (g_taskbarStateUpdating) {
            PostMessageW(hwnd, kRefreshMessage, 0, 0);
            return 0;
        }
        EvaluateState();
        return 0;
    case kReassertMessage:
        ReassertTintedTaskbars();
        return 0;
    case kTrayMessage:
        if (LOWORD(lparam) == WM_RBUTTONUP || LOWORD(lparam) == WM_CONTEXTMENU) {
            ShowTrayMenu(hwnd);
        } else if (LOWORD(lparam) == WM_LBUTTONDBLCLK) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kStateTimer);
        KillTimer(hwnd, kAnimTimer);
        RestoreAllTaskbars();
        RemoveTrayIcon(hwnd);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

void InitLogPath() {
    wchar_t localAppData[MAX_PATH] {};
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return;
    }

    std::filesystem::path dir = std::filesystem::path(localAppData) / L"ImmersiveTopTaskbar";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    g_logPath = dir / L"log.txt";
    g_statePath = dir / L"preflight-ok-v1.txt";
    g_shellThemeStatePath = dir / L"shell-theme-original.txt";
    std::ofstream(g_logPath, std::ios::trunc) << "ImmersiveTopTaskbar log\n";
}

void EnablePerMonitorDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        return;
    }

    auto setContext = reinterpret_cast<SetProcessDpiAwarenessContextProc>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (setContext) {
        setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
}

void InstallHooks() {
    g_foregroundHook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND,
        EVENT_SYSTEM_FOREGROUND,
        nullptr,
        WinEventProc,
        0,
        0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    g_locationHook = SetWinEventHook(
        EVENT_OBJECT_LOCATIONCHANGE,
        EVENT_OBJECT_LOCATIONCHANGE,
        nullptr,
        WinEventProc,
        0,
        0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    g_minimizeHook = SetWinEventHook(
        EVENT_SYSTEM_MINIMIZESTART,
        EVENT_SYSTEM_MINIMIZEEND,
        nullptr,
        WinEventProc,
        0,
        0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    g_moveSizeHook = SetWinEventHook(
        EVENT_SYSTEM_MOVESIZESTART,
        EVENT_SYSTEM_MOVESIZEEND,
        nullptr,
        WinEventProc,
        0,
        0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, g_instance, 0);
    if (!g_mouseHook) {
        Log("SetWindowsHookExW(WH_MOUSE_LL) failed gle=" + std::to_string(GetLastError()));
    }

    // Self-check: if any interaction hook is dead, the settle protection for
    // taskbar clicks and window drags silently never fires. Make it visible.
    std::ostringstream ss;
    ss << "[HOOK] install: foreground=" << (g_foregroundHook ? "ok" : "FAIL")
       << " location=" << (g_locationHook ? "ok" : "FAIL")
       << " minimize=" << (g_minimizeHook ? "ok" : "FAIL")
       << " moveSize=" << (g_moveSizeHook ? "ok" : "FAIL")
       << " mouseLL=" << (g_mouseHook ? "ok" : "FAIL");
    Log(ss.str());
}

void UninstallHooks() {
    if (g_foregroundHook) {
        UnhookWinEvent(g_foregroundHook);
        g_foregroundHook = nullptr;
    }
    if (g_locationHook) {
        UnhookWinEvent(g_locationHook);
        g_locationHook = nullptr;
    }
    if (g_minimizeHook) {
        UnhookWinEvent(g_minimizeHook);
        g_minimizeHook = nullptr;
    }
    if (g_moveSizeHook) {
        UnhookWinEvent(g_moveSizeHook);
        g_moveSizeHook = nullptr;
    }
    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }
    g_moveSizeWindow = nullptr;
    g_moveSizeTransparentMode = false;
    g_taskbarInteractionUntil = 0;
}

bool HasCommandLineFlag(const wchar_t *flag) {
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return false;
    }

    bool found = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], flag) == 0) {
            found = true;
            break;
        }
    }
    LocalFree(argv);
    return found;
}

void LoadCalibrationOptions() {
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return;
    }

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg.rfind(L"--brightness=", 0) == 0) {
            g_colorBrightness = std::clamp(std::wcstod(arg.c_str() + 13, nullptr), 0.75, 1.35);
        } else if (arg.rfind(L"--saturation=", 0) == 0) {
            g_colorSaturation = std::clamp(std::wcstod(arg.c_str() + 13, nullptr), 0.0, 1.25);
        } else if (arg.rfind(L"--offset=", 0) == 0) {
            g_colorOffset = ClampInt(static_cast<int>(std::wcstol(arg.c_str() + 9, nullptr, 10)), -32, 32);
        }
    }
    LocalFree(argv);

    std::ostringstream ss;
    ss << "color calibration brightness=" << g_colorBrightness
       << " saturation=" << g_colorSaturation
       << " offset=" << g_colorOffset;
    Log(ss.str());
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_instance = instance;
    EnablePerMonitorDpiAwareness();
    InitLogPath();
    SetUnhandledExceptionFilter(TopLevelExceptionRestoreFilter);
    CaptureShellTheme();
    LoadCalibrationOptions();
    HRESULT cohr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_comInitialized = SUCCEEDED(cohr);
    Log("CoInitializeEx hr=" + HexHr(cohr));

    if (HasCommandLineFlag(L"--tap-probe")) {
        const int result = RunExplorerTapProbe();
        if (g_comInitialized) {
            CoUninitialize();
            g_comInitialized = false;
        }
        return result;
    }

    if (HasCommandLineFlag(L"--restore-ttb")) {
        g_ttbForceRefreshMessage = RegisterWindowMessageW(L"TTB_ForceRefreshTaskbar");
        RestoreTranslucentTbMaximizedAppearance();
        ForceRestoreShellThemeNow("restore-ttb");
        if (g_comInitialized) {
            CoUninitialize();
            g_comInitialized = false;
        }
        return 0;
    }

    if (HasCommandLineFlag(L"--quit")) {
        if (HWND existing = FindWindowW(L"ImmersiveTopTaskbarHiddenWindow", nullptr)) {
            PostMessageW(existing, WM_CLOSE, 0, 0);
        } else {
            g_ttbForceRefreshMessage = RegisterWindowMessageW(L"TTB_ForceRefreshTaskbar");
            RestoreTranslucentTbMaximizedAppearance();
            ForceRestoreShellThemeNow("quit-no-window");
        }
        if (g_comInitialized) {
            CoUninitialize();
            g_comInitialized = false;
        }
        return 0;
    }

    g_singleInstance = CreateMutexW(nullptr, TRUE, L"Local\\ImmersiveTopTaskbarSingleInstance");
    if (g_singleInstance && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(L"ImmersiveTopTaskbarHiddenWindow", nullptr)) {
            PostMessageW(existing, kRefreshMessage, 0, 0);
        }
        return 0;
    }

    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    g_ttbApplyColorPreviewMessage = RegisterWindowMessageW(L"TTB_ApplyColorPreview");
    g_ttbForceRefreshMessage = RegisterWindowMessageW(L"TTB_ForceRefreshTaskbar");

    const bool preflightAlreadyPassed = HasPassedPreflightOnce();

    // Show the usage notice only before this device has completed a passing
    // environment check. Later launches still check silently and show failures.
    if (!preflightAlreadyPassed && !ShowStartupNotice()) {
        Log("startup notice declined by user");
        if (g_comInitialized) {
            CoUninitialize();
            g_comInitialized = false;
        }
        return 0;
    }

    // Refuse to run without TranslucentTB / ExplorerTAP / a top taskbar instead
    // of silently sitting idle. Reports every prerequisite with a pass/fail glyph.
    if (!RunPreflightChecks(!preflightAlreadyPassed)) {
        if (g_comInitialized) {
            CoUninitialize();
            g_comInitialized = false;
        }
        return 1;
    }
    if (!preflightAlreadyPassed) {
        MarkPreflightPassed();
    }

    // A forced termination can leave the temporary opaque desktop baseline in
    // settings.json. Recover the saved sections before creating a fresh backup
    // for this process. This runs only after single-instance ownership succeeds.
    RestoreTranslucentTbMaximizedAppearance();
    RegisterExplorerTapProcessDeathRestore();

    WNDCLASSEXW wc {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"ImmersiveTopTaskbarHiddenWindow";
    wc.hIcon = LoadIconW(g_instance, MAKEINTRESOURCE(IDI_MAIN_ICON));

    if (!RegisterClassExW(&wc)) {
        return 1;
    }

    WNDCLASSEXW seamClass {};
    seamClass.cbSize = sizeof(seamClass);
    seamClass.lpfnWndProc = SeamCoverWndProc;
    seamClass.hInstance = instance;
    seamClass.lpszClassName = L"ImmersiveTopTaskbarSeamCover";
    if (!RegisterClassExW(&seamClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return 1;
    }

    g_window = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"Immersive Top Taskbar",
        WS_OVERLAPPED,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (!g_window) {
        return 1;
    }

    InstallHooks();

    // 启动后异步检查一次更新（后台线程，仅在有新版本时弹提示）。
    CheckForUpdates();

    MSG msg {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UninstallHooks();
    RestoreAllTaskbars();
    if (g_ttbAppearanceService) {
        g_ttbAppearanceService->Release();
        g_ttbAppearanceService = nullptr;
    }
    if (g_singleInstance) {
        CloseHandle(g_singleInstance);
        g_singleInstance = nullptr;
    }
    if (g_explorerTapDll) {
        FreeLibrary(g_explorerTapDll);
        g_explorerTapDll = nullptr;
        g_injectExplorerTap = nullptr;
    }
    if (g_comInitialized) {
        CoUninitialize();
        g_comInitialized = false;
    }
    return 0;
}
