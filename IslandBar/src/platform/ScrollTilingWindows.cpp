#include "platform/ScrollTilingWindows.h"

#include <dwmapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")

namespace island {
namespace {

bool ProcessCreationTime(DWORD processId, ULONGLONG* output) {
    if (!output || processId == 0) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return false;
    FILETIME created = {};
    FILETIME exited = {};
    FILETIME kernel = {};
    FILETIME user = {};
    const bool ok = GetProcessTimes(process, &created, &exited, &kernel, &user) != FALSE;
    CloseHandle(process);
    if (!ok) return false;
    ULARGE_INTEGER value = {};
    value.LowPart = created.dwLowDateTime;
    value.HighPart = created.dwHighDateTime;
    *output = value.QuadPart;
    return true;
}

bool ProcessIsElevated(DWORD processId) {
    if (processId == 0) return true;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return true;
    HANDLE token = nullptr;
    bool elevated = true;
    if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elevation = {};
        DWORD bytes = 0;
        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &bytes)) {
            elevated = elevation.TokenIsElevated != 0;
        }
        CloseHandle(token);
    }
    CloseHandle(process);
    return elevated;
}

bool IsCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
           cloaked != 0;
}

bool IsShellWindow(HWND hwnd) {
    wchar_t name[128] = {};
    if (!GetClassNameW(hwnd, name, 128)) return false;
    static const wchar_t* const classes[] = {
        L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Progman", L"WorkerW",
        L"ApplicationManager_DesktopShellWindow", L"MultitaskingViewFrame",
        L"ForegroundStaging", L"XamlExplorerHostIslandWindow", L"TaskListThumbnailWnd",
    };
    for (const wchar_t* value : classes) {
        if (_wcsicmp(name, value) == 0) return true;
    }
    return false;
}

bool WindowCoversMonitor(HWND hwnd, HMONITOR monitor) {
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    RECT rect = {};
    return GetMonitorInfoW(monitor, &info) && GetWindowRect(hwnd, &rect) &&
           rect.left <= info.rcMonitor.left && rect.top <= info.rcMonitor.top &&
           rect.right >= info.rcMonitor.right && rect.bottom >= info.rcMonitor.bottom;
}

bool SameRect(const RECT& a, const RECT& b, int tolerance) {
    return std::abs(a.left - b.left) <= tolerance &&
           std::abs(a.top - b.top) <= tolerance &&
           std::abs(a.right - b.right) <= tolerance &&
           std::abs(a.bottom - b.bottom) <= tolerance;
}

} // namespace

std::vector<ScrollTilingWindowRecord> ScrollTilingWindows::Collect(
    const WindowList& windows, HMONITOR targetMonitor, int maximum,
    std::wstring* summary) {
    std::vector<ScrollTilingWindowRecord> records;
    if (maximum <= 0 || !targetMonitor) {
        if (summary) summary->clear();
        return records;
    }
    maximum = std::min(maximum, kScrollTilingMaxColumns);

    int seen = 0;
    int otherMonitor = 0;
    int specialState = 0;
    int elevated = 0;
    int identityFailed = 0;
    int otherDesktop = 0;

    for (int i = 0; i < windows.Count() && static_cast<int>(records.size()) < maximum; ++i) {
        ++seen;
        const TaskEntry& entry = windows.At(i);
        HWND hwnd = entry.handle;
        if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd) ||
            IsZoomed(hwnd) || IsCloaked(hwnd) || IsShellWindow(hwnd)) {
            ++specialState;
            continue;
        }

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == 0 || pid != entry.processId || pid == GetCurrentProcessId()) {
            ++identityFailed;
            continue;
        }
        if (ProcessIsElevated(pid)) {
            ++elevated;
            continue;
        }
        if (!IsOnCurrentVirtualDesktop(hwnd)) {
            ++otherDesktop;
            continue;
        }
        if (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) != targetMonitor) {
            ++otherMonitor;
            continue;
        }
        if (WindowCoversMonitor(hwnd, targetMonitor)) {
            ++specialState;
            continue;
        }

        ScrollTilingWindowRecord record;
        record.hwnd = hwnd;
        record.processId = pid;
        record.originalPlacement.length = sizeof(record.originalPlacement);
        if (!ProcessCreationTime(pid, &record.processCreationTime) ||
            !GetWindowPlacement(hwnd, &record.originalPlacement) ||
            !GetWindowRect(hwnd, &record.originalRect)) {
            ++identityFailed;
            continue;
        }
        if (record.originalPlacement.showCmd == SW_SHOWMAXIMIZED ||
            record.originalPlacement.showCmd == SW_SHOWMINIMIZED ||
            record.originalRect.right <= record.originalRect.left ||
            record.originalRect.bottom <= record.originalRect.top) {
            ++specialState;
            continue;
        }
        records.push_back(record);
    }

    if (summary) {
        wchar_t text[320] = {};
        std::swprintf(text, 320,
                      L"seen=%d eligible=%zu other-monitor=%d special=%d elevated=%d "
                      L"identity=%d desktop=%d",
                      seen, records.size(), otherMonitor, specialState, elevated,
                      identityFailed, otherDesktop);
        *summary = text;
    }
    return records;
}

bool ScrollTilingWindows::Capture(HWND hwnd, HMONITOR targetMonitor,
                                  ScrollTilingWindowRecord* record,
                                  std::wstring* reason) {
    if (reason) reason->clear();
    if (!record || !hwnd || !targetMonitor || !IsWindow(hwnd)) {
        if (reason) *reason = L"window does not exist";
        return false;
    }
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || IsZoomed(hwnd) ||
        IsCloaked(hwnd) || IsShellWindow(hwnd)) {
        if (reason) *reason = L"window state is not eligible";
        return false;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid || pid == GetCurrentProcessId()) {
        if (reason) *reason = L"window belongs to IslandBar";
        return false;
    }
    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    const bool appWindow = (exStyle & WS_EX_APPWINDOW) != 0;
    if (GetWindowTextLengthW(hwnd) <= 0 && !appWindow) {
        if (reason) *reason = L"untitled helper window";
        return false;
    }
    if (((exStyle & WS_EX_TOOLWINDOW) != 0 || GetWindow(hwnd, GW_OWNER) != nullptr) &&
        !appWindow) {
        if (reason) *reason = L"window is owned or a tool window";
        return false;
    }
    if (ProcessIsElevated(pid)) {
        if (reason) *reason = L"window is elevated";
        return false;
    }
    if (!IsOnCurrentVirtualDesktop(hwnd)) {
        if (reason) *reason = L"window is on another virtual desktop";
        return false;
    }
    if (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) != targetMonitor) {
        if (reason) *reason = L"window is on another monitor";
        return false;
    }
    if (WindowCoversMonitor(hwnd, targetMonitor)) {
        if (reason) *reason = L"window covers the monitor";
        return false;
    }

    ScrollTilingWindowRecord captured;
    captured.hwnd = hwnd;
    captured.processId = pid;
    captured.originalPlacement.length = sizeof(captured.originalPlacement);
    if (!ProcessCreationTime(pid, &captured.processCreationTime) ||
        !GetWindowPlacement(hwnd, &captured.originalPlacement) ||
        !GetWindowRect(hwnd, &captured.originalRect)) {
        if (reason) *reason = L"window identity or geometry is unavailable";
        return false;
    }
    if (captured.originalPlacement.showCmd == SW_SHOWMAXIMIZED ||
        captured.originalPlacement.showCmd == SW_SHOWMINIMIZED ||
        captured.originalRect.right <= captured.originalRect.left ||
        captured.originalRect.bottom <= captured.originalRect.top) {
        if (reason) *reason = L"window has a special size state";
        return false;
    }
    *record = captured;
    return true;
}

bool ScrollTilingWindows::StillSameWindow(const ScrollTilingWindowRecord& record) {
    if (!record.hwnd || !IsWindow(record.hwnd)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(record.hwnd, &pid);
    if (pid == 0 || pid != record.processId) return false;
    ULONGLONG created = 0;
    return ProcessCreationTime(pid, &created) && created == record.processCreationTime;
}

bool ScrollTilingWindows::IsSafeForLayout(const ScrollTilingWindowRecord& record) {
    if (!StillSameWindow(record) || !IsWindowVisible(record.hwnd) ||
        IsIconic(record.hwnd) || IsZoomed(record.hwnd) || IsCloaked(record.hwnd)) {
        return false;
    }
    WINDOWPLACEMENT placement = {};
    placement.length = sizeof(placement);
    return GetWindowPlacement(record.hwnd, &placement) &&
           placement.showCmd != SW_SHOWMAXIMIZED &&
           placement.showCmd != SW_SHOWMINIMIZED &&
           IsOnCurrentVirtualDesktop(record.hwnd);
}

bool ScrollTilingWindows::IsOnCurrentVirtualDesktop(HWND hwnd) {
    if (!hwnd) return false;
    IVirtualDesktopManager* manager = nullptr;
    const HRESULT created = CoCreateInstance(
        CLSID_VirtualDesktopManager, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&manager));
    if (FAILED(created) || !manager) return true;
    BOOL current = TRUE;
    const HRESULT queried = manager->IsWindowOnCurrentVirtualDesktop(hwnd, &current);
    manager->Release();
    return FAILED(queried) ? true : current != FALSE;
}

bool ScrollTilingWindows::CurrentRectMatches(HWND hwnd, const RECT& expected,
                                             int tolerancePx, RECT* actual) {
    RECT current = {};
    if (!hwnd || !GetWindowRect(hwnd, &current)) return false;
    if (actual) *actual = current;
    return SameRect(current, expected, tolerancePx);
}

bool ScrollTilingWindows::Apply(std::span<const ScrollTilingPlacement> placements,
                                bool verify,
                                ScrollTilingApplyResult* result) {
    if (result) {
        result->requested = 0;
        result->accepted = 0;
        result->verified = 0;
        result->failed = 0;
        result->durationMs = 0;
        result->placedMask = 0;
        result->verifiedMask = 0;
    }
    if (placements.empty()) {
        return true;
    }
    if (placements.size() > static_cast<size_t>(kScrollTilingMaxColumns)) {
        return false;
    }

    const unsigned long long started = GetTickCount64();
    if (result) result->requested = static_cast<int>(placements.size());

    for (size_t i = 0; i < placements.size(); ++i) {
        const ScrollTilingPlacement& placement = placements[i];
        if (placement.rect.Empty() || !placement.hwnd) continue;
        const BOOL moved = SetWindowPos(
            placement.hwnd, nullptr, placement.rect.left, placement.rect.top,
            placement.rect.Width(), placement.rect.Height(),
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                SWP_NOSENDCHANGING);
        if (moved && result) {
            ++result->accepted;
            result->placedMask |= std::uint64_t{1} << i;
        }
    }

    if (verify) {
        for (size_t i = 0; i < placements.size(); ++i) {
            RECT expected = {
                placements[i].rect.left,
                placements[i].rect.top,
                placements[i].rect.right,
                placements[i].rect.bottom,
            };
            const bool matched =
                CurrentRectMatches(placements[i].hwnd, expected, 1);
            if (result) {
                if (matched) {
                    result->verifiedMask |= std::uint64_t{1} << i;
                    ++result->verified;
                } else {
                    ++result->failed;
                }
            }
        }
    } else if (result) {
        result->failed =
            result->requested - result->accepted;
    }
    if (result) result->durationMs = GetTickCount64() - started;
    return result == nullptr ||
           (verify ? result->failed == 0
                   : result->accepted == result->requested);
}

ScrollTilingRestoreResult ScrollTilingWindows::Restore(
    const ScrollTilingWindowRecord& record) {
    if (!StillSameWindow(record)) return ScrollTilingRestoreResult::InvalidIdentity;
    RECT current = {};
    if (!GetWindowRect(record.hwnd, &current)) {
        return ScrollTilingRestoreResult::InvalidIdentity;
    }
    const bool atTarget = SameRect(current, record.lastAppliedRect, 1);
    const int previousWidth =
        record.previousAppliedRect.right - record.previousAppliedRect.left;
    const int targetWidth =
        record.lastAppliedRect.right - record.lastAppliedRect.left;
    const int currentWidth = current.right - current.left;
    const bool inTransition =
        std::abs(previousWidth - targetWidth) <= 1 &&
        std::abs(currentWidth - targetWidth) <= 1 &&
        std::abs(current.top - record.lastAppliedRect.top) <= 1 &&
        std::abs(current.bottom - record.lastAppliedRect.bottom) <= 1 &&
        current.left >= std::min(record.previousAppliedRect.left,
                                 record.lastAppliedRect.left) - 1 &&
        current.left <= std::max(record.previousAppliedRect.left,
                                 record.lastAppliedRect.left) + 1;
    if (!atTarget && !inTransition) {
        return ScrollTilingRestoreResult::ExternallyMoved;
    }
    return RestoreOriginal(record) ? ScrollTilingRestoreResult::Restored
                                   : ScrollTilingRestoreResult::Failed;
}

bool ScrollTilingWindows::RestoreOriginal(
    const ScrollTilingWindowRecord& record) {
    if (!StillSameWindow(record)) return false;
    WINDOWPLACEMENT original = record.originalPlacement;
    original.length = sizeof(original);
    return SetWindowPlacement(record.hwnd, &original) &&
           CurrentRectMatches(record.hwnd, record.originalRect, 2);
}

} // namespace island
