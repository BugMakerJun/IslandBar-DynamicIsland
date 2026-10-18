// =====================================================================
//  Monitor.cpp
// =====================================================================
#include "platform/Monitor.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

#include <cwchar>
#include <vector>

namespace island {
namespace {

// The shell's own taskbar, and the only window this file looks at by class.
constexpr const wchar_t* kTrayClass = L"Shell_TrayWnd";

// GetDpiForMonitor is Windows 8.1+. Fall back to the device caps so the bar
// still lays out sensibly on a machine that lacks it, instead of assuming 96.
UINT QueryDpi(HMONITOR monitor, MONITOR_DPI_TYPE type) {
    UINT dpiX = 96;
    UINT dpiY = 96;
    if (SUCCEEDED(GetDpiForMonitor(monitor, type, &dpiX, &dpiY))) {
        return dpiX ? dpiX : 96;
    }

    // Fallback: derive from the GDI device caps of the monitor's virtual screen.
    HDC screen = GetDC(nullptr);
    if (screen) {
        const int gdiDpi = GetDeviceCaps(screen, LOGPIXELSX);
        ReleaseDC(nullptr, screen);
        if (gdiDpi > 0) return static_cast<UINT>(gdiDpi);
    }
    return 96;
}

// A system metric for a given monitor's DPI. GetSystemMetricsForDpi is Windows 10 1607+, and the
// difference matters on a mixed-DPI desktop: the plain call reports the value for one DPI, so a
// maximized window on a 150% monitor would be compared against 100% borders and never match.
LONG MetricForDpi(int index, UINT dpi) {
    using Fn = int(WINAPI*)(int, UINT);
    static Fn fn = reinterpret_cast<Fn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi"));
    if (fn) return fn(index, dpi);
    return GetSystemMetrics(index);
}

} // namespace

MonitorGeometry QueryMonitor(HMONITOR monitor) {
    if (!monitor) {
        monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    }

    MonitorGeometry geometry;
    geometry.monitor = monitor;

    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) {
        geometry.screen  = info.rcMonitor;
        geometry.work    = info.rcWork;
        geometry.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    } else {
        // No information is better than a zero rect: fall back to the desktop.
        geometry.screen.left   = 0;
        geometry.screen.top    = 0;
        geometry.screen.right  = GetSystemMetrics(SM_CXSCREEN);
        geometry.screen.bottom = GetSystemMetrics(SM_CYSCREEN);
        geometry.work          = geometry.screen;
    }

    geometry.dpi  = QueryDpi(monitor, MDT_EFFECTIVE_DPI);
    geometry.dpiY = geometry.dpi;

    return geometry;
}

bool FullScreenAppInFront(const RECT& monitor) {
    // THE SHELL'S OWN ANSWER FIRST, because the geometry below cannot see an APPLICATION's full screen.
    //
    // A game's exclusive full screen is a window that covers the monitor and is not maximised, which is
    // what this function has always tested -- and it is why the bar stood down only "when playing a game
    // full-screen". A video in a browser, or a player that goes full screen by hiding its own chrome, is
    // still a MAXIMISED window, and the maximised test below deliberately answers "no" for those. The
    // shell hides its own taskbar in exactly those cases and publishes the answer here: QUNS_BUSY for a
    // general full-screen application, QUNS_RUNNING_D3D_FULL_SCREEN for an exclusive one. The geometry
    // below stays as the fallback for a borderless full screen the shell does not consider one.
    QUERY_USER_NOTIFICATION_STATE state = QUNS_ACCEPTS_NOTIFICATIONS;
    if (SUCCEEDED(SHQueryUserNotificationState(&state))) {
        if (state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_BUSY) return true;
    }

    const HWND foreground = GetForegroundWindow();
    if (!foreground) return false;

    // Our own windows do not count, and the pid is enough: the bar is one process and none of its windows
    // is an application the user is playing. (It is also why this needs no handle passed in, which keeps
    // the answer a function of the monitor alone.)
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    if (pid == GetCurrentProcessId()) return false;

    if (!IsWindowVisible(foreground) || IsIconic(foreground)) return false;

    wchar_t className[64] = {};
    GetClassNameW(foreground, className, 64);
    if (::_wcsicmp(className, L"Progman") == 0 || ::_wcsicmp(className, L"WorkerW") == 0 ||
        ::_wcsicmp(className, L"Shell_TrayWnd") == 0 ||
        ::_wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0) {
        return false;
    }

    // A window DWM has cloaked is not on screen: an application on another virtual desktop covers the
    // monitor and nobody is looking at it.
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(foreground, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
        cloaked != 0) {
        return false;
    }

    RECT rect = {};
    if (!GetWindowRect(foreground, &rect)) return false;
    if (!WindowCoversMonitor(rect, monitor)) return false;

    // A MAXIMIZED WINDOW IS NOT A FULL-SCREEN APPLICATION, and this test used to be free: a maximized
    // window stops at the work area, which is inset by the stack's band, so it could never cover the
    // monitor. Since the bar releases that reservation so a maximized window can reach the top of the
    // screen (NativeTaskbar::ReleaseWorkArea), a maximized window covers the monitor too and the
    // geometry alone no longer separates the two.
    //
    // What does separate them is the invisible resize border: a maximized window is the work area
    // inflated by it, and a full-screen application is not. IsZoomed is asked as well, so this can only
    // ever turn a "yes" into a "no" for a window Windows itself calls maximized.
    if (IsZoomed(foreground)) {
        const HMONITOR owner = MonitorFromRect(&monitor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info = {};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(owner, &info) &&
            LooksLikeWindowsMaximizedRect(rect, info.rcWork, QueryDpi(owner, MDT_EFFECTIVE_DPI))) {
            return false;
        }
    }

    return true;
}

LONG ShellTopBandThicknessPx(HMONITOR monitor) {
    const HWND tray = FindWindowW(kTrayClass, nullptr);
    if (!tray) return 0;

    RECT rect = {};
    if (!GetWindowRect(tray, &rect)) return 0;

    const HMONITOR owner = MonitorFromWindow(tray, MONITOR_DEFAULTTONEAREST);
    if (!monitor || owner != monitor) return 0;

    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(owner, &info)) return 0;

    const LONG width = rect.right - rect.left;
    const LONG height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return 0;

    // Wider than it is tall (so it is a horizontal bar) AND starting at or above the top of the display.
    // Both are needed: one test alone cannot tell a top-docked taskbar from a bottom-docked one, and the
    // answer here is specifically the band the island hangs in.
    if (width < height) return 0;
    if (rect.top > info.rcMonitor.top) return 0;

    const LONG thickness = rect.bottom - info.rcMonitor.top;
    return thickness > 0 ? thickness : 0;
}

BorderInflation MaximizedBorderPx(UINT dpi) {
    BorderInflation border;
    border.x = MetricForDpi(SM_CXSIZEFRAME, dpi) + MetricForDpi(SM_CXPADDEDBORDER, dpi);
    border.y = MetricForDpi(SM_CYSIZEFRAME, dpi) + MetricForDpi(SM_CXPADDEDBORDER, dpi);
    return border;
}

namespace {

struct NudgePass {
    const RECT* monitorScreen;
    const RECT* workBefore;
    const RECT* workAfter;
    DWORD selfPid;
    int moved;
};

BOOL CALLBACK NudgeProc(HWND hwnd, LPARAM param) {
    auto* pass = reinterpret_cast<NudgePass*>(param);

    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
    if (!IsZoomed(hwnd)) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid || pid == pass->selfPid) return TRUE;

    // Not on screen at all: another virtual desktop, or a UWP application that has no window of its
    // own. Neither is something to move.
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
        cloaked != 0) {
        return TRUE;
    }

    const HMONITOR owner = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(owner, &info)) return TRUE;

    // Only the monitor whose strip was released: the other displays' work areas were not touched, so a
    // window on one of them is not in the state this is about.
    if (!SameRect(info.rcMonitor, *pass->monitorScreen)) return TRUE;

    RECT rect = {};
    if (!GetWindowRect(hwnd, &rect)) return TRUE;

    const UINT dpi = QueryDpi(owner, MDT_EFFECTIVE_DPI);
    const BorderInflation border = MaximizedBorderPx(dpi);

    // EXACTLY the maximized rectangle of the OLD work area, nothing else. A window that was maximized
    // for some other reason -- a custom frame, a snapped window, an application that sizes itself --
    // does not match and is left alone.
    if (!SameRect(rect, InflateRect(*pass->workBefore, border.x, border.y))) return TRUE;

    const RECT target = InflateRect(*pass->workAfter, border.x, border.y);
    // SWP_NOACTIVATE: measured, the foreground window does not change. SWP_NOSENDCHANGING: the
    // application cannot veto the rectangle, which is the whole point -- measured, an already-maximized
    // window does not respond to a repeated SW_MAXIMIZE either, so there is no gentler way to move it.
    if (SetWindowPos(hwnd, nullptr, target.left, target.top, target.right - target.left,
                     target.bottom - target.top,
                     SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSENDCHANGING)) {
        ++pass->moved;
    }
    return TRUE;
}

} // namespace

int NudgeStaleMaximizedWindows(const RECT& monitorScreen, const RECT& workBefore,
                               const RECT& workAfter) {
    if (SameRect(workBefore, workAfter)) return 0;

    NudgePass pass{&monitorScreen, &workBefore, &workAfter, GetCurrentProcessId(), 0};
    EnumWindows(&NudgeProc, reinterpret_cast<LPARAM>(&pass));
    return pass.moved;
}

} // namespace island
