// =====================================================================
//  tiling_probe -- can ordinary third-party windows be moved as a group?
//
//  This probe deliberately owns no production state. It lists the windows
//  IslandBar's normal task filter would expose, optionally moves the first N
//  into a two-column layout, verifies the rectangles it can observe, and
//  restores their WINDOWPLACEMENT values.
//
//      tiling_probe --list
//      tiling_probe --move-existing 5
//      tiling_probe --move-existing 1 --title ChatGPT
//
//  The title option exists because every application family reacts differently
//  to an external layout. Run the probe against Chromium, Qt, WPF and native
//  Win32 windows separately; do not treat five instances of one family as five
//  independent answers.
// =====================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <objbase.h>
#include <dwmapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "dwmapi.lib")

namespace {

struct Candidate {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    bool elevated = false;
    std::wstring title;
    RECT rect = {};
    WINDOWPLACEMENT placement = {};
};

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

bool IsCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
           cloaked != 0;
}

bool IsElevated(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return true;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return true;
    HANDLE token = nullptr;
    bool elevated = true;
    if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION value = {};
        DWORD size = 0;
        if (GetTokenInformation(token, TokenElevation, &value, sizeof(value), &size)) {
            elevated = value.TokenIsElevated != 0;
        }
        CloseHandle(token);
    }
    CloseHandle(process);
    return elevated;
}

struct EnumContext {
    DWORD self = 0;
    const wchar_t* titleFilter = nullptr;
    std::vector<Candidate>* output = nullptr;
};

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM param) {
    auto* context = reinterpret_cast<EnumContext*>(param);
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || IsZoomed(hwnd) || IsCloaked(hwnd) ||
        IsShellWindow(hwnd)) {
        return TRUE;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid || pid == context->self) return TRUE;

    wchar_t title[512] = {};
    const int titleLength = GetWindowTextW(hwnd, title, 512);
    if (titleLength <= 0) return TRUE;
    if (context->titleFilter && *context->titleFilter &&
        !wcsstr(title, context->titleFilter)) {
        return TRUE;
    }

    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    const bool appWindow = (exStyle & WS_EX_APPWINDOW) != 0;
    if (((exStyle & WS_EX_TOOLWINDOW) != 0 || GetWindow(hwnd, GW_OWNER) != nullptr) &&
        !appWindow) {
        return TRUE;
    }
    Candidate candidate;
    candidate.hwnd = hwnd;
    candidate.pid = pid;
    candidate.title = title;
    candidate.elevated = IsElevated(hwnd);
    candidate.placement.length = sizeof(candidate.placement);
    if (!GetWindowRect(hwnd, &candidate.rect) || !GetWindowPlacement(hwnd, &candidate.placement)) {
        return TRUE;
    }
    if (candidate.rect.right <= candidate.rect.left || candidate.rect.bottom <= candidate.rect.top) {
        return TRUE;
    }
    context->output->push_back(std::move(candidate));
    return TRUE;
}

std::vector<Candidate> ListCandidates(const wchar_t* titleFilter) {
    EnumContext context;
    context.self = GetCurrentProcessId();
    context.titleFilter = titleFilter;
    std::vector<Candidate> candidates;
    context.output = &candidates;
    EnumWindows(&EnumProc, reinterpret_cast<LPARAM>(&context));
    return candidates;
}

bool SameRect(const RECT& a, const RECT& b, LONG tolerance = 2) {
    return std::abs(a.left - b.left) <= tolerance &&
           std::abs(a.top - b.top) <= tolerance &&
           std::abs(a.right - b.right) <= tolerance &&
           std::abs(a.bottom - b.bottom) <= tolerance;
}

int MoveAndRestore(std::vector<Candidate> candidates, int count) {
    count = std::min<int>(count, static_cast<int>(candidates.size()));
    if (count <= 0) {
        std::printf("no candidates\n");
        return 1;
    }

    RECT work = {};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) return 2;
    const LONG workWidth = work.right - work.left;
    const LONG workHeight = work.bottom - work.top;
    const LONG gap = 12;
    const LONG left = work.left + 32;
    const LONG top = work.top + 32;
    const LONG width = std::max<LONG>(240, (workWidth - 96 - gap) / 2);
    const LONG height = std::max<LONG>(180, workHeight - 96);

    const unsigned long long started = GetTickCount64();
    bool batch = true;
    std::vector<RECT> targets(static_cast<size_t>(count));
    int added = 0;
    for (int i = 0; i < count; ++i) {
        const LONG column = i % 2;
        const LONG row = i / 2;
        targets[static_cast<size_t>(i)] = {
            left + column * (width + gap),
            top + row * std::min<LONG>(height + gap, 80),
            left + column * (width + gap) + width,
            top + row * std::min<LONG>(height + gap, 80) + height,
        };
        const BOOL moved = SetWindowPos(
            candidates[static_cast<size_t>(i)].hwnd, nullptr,
            targets[static_cast<size_t>(i)].left, targets[static_cast<size_t>(i)].top,
            width, height,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                SWP_NOSENDCHANGING);
        if (!moved) batch = false;
        if (moved) ++added;
    }

    std::printf("move: requested=%d committed=%d added=%d duration_ms=%llu\n",
                count, batch ? 1 : 0, added,
                static_cast<unsigned long long>(GetTickCount64() - started));
    Sleep(300);

    int applied = 0;
    for (int i = 0; i < count; ++i) {
        RECT actual = {};
        const bool ok = GetWindowRect(candidates[static_cast<size_t>(i)].hwnd, &actual) &&
                        SameRect(actual, targets[static_cast<size_t>(i)]);
        if (ok) ++applied;
        std::printf("verify %d: %s target=(%ld,%ld,%ld,%ld) actual=(%ld,%ld,%ld,%ld)\n",
                    i, ok ? "ok" : "FAIL",
                    targets[static_cast<size_t>(i)].left, targets[static_cast<size_t>(i)].top,
                    targets[static_cast<size_t>(i)].right, targets[static_cast<size_t>(i)].bottom,
                    actual.left, actual.top, actual.right, actual.bottom);
    }

    int restored = 0;
    for (int i = 0; i < count; ++i) {
        Candidate& candidate = candidates[static_cast<size_t>(i)];
        const BOOL ok = SetWindowPlacement(candidate.hwnd, &candidate.placement);
        RECT actual = {};
        const bool verified = ok && GetWindowRect(candidate.hwnd, &actual) &&
                              SameRect(actual, candidate.rect, 4);
        if (verified) ++restored;
        std::printf("restore %d: %s original=(%ld,%ld,%ld,%ld) actual=(%ld,%ld,%ld,%ld)\n",
                    i, verified ? "ok" : "FAIL",
                    candidate.rect.left, candidate.rect.top,
                    candidate.rect.right, candidate.rect.bottom,
                    actual.left, actual.top, actual.right, actual.bottom);
    }

    std::printf("RESULT applied=%d/%d restored=%d/%d\n",
                applied, count, restored, count);
    return applied == count && restored == count ? 0 : 3;
}

int RunSelfWindows(int count) {
    count = std::clamp(count, 1, 5);
    std::vector<Candidate> windows;
    windows.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        wchar_t title[64] = {};
        std::swprintf(title, 64, L"IslandBar tiling probe %d", i + 1);
        HWND hwnd = CreateWindowExW(
            0, L"STATIC", title, WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            120 + i * 48, 120 + i * 36, 520, 360, nullptr, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        if (!hwnd) continue;
        Candidate candidate;
        candidate.hwnd = hwnd;
        candidate.pid = GetCurrentProcessId();
        candidate.title = title;
        candidate.placement.length = sizeof(candidate.placement);
        if (!GetWindowRect(hwnd, &candidate.rect) ||
            !GetWindowPlacement(hwnd, &candidate.placement)) {
            DestroyWindow(hwnd);
            continue;
        }
        windows.push_back(std::move(candidate));
    }
    Sleep(100);
    const int result = MoveAndRestore(windows, static_cast<int>(windows.size()));
    for (const Candidate& candidate : windows) DestroyWindow(candidate.hwnd);
    return result;
}

} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    const wchar_t* titleFilter = nullptr;
    int moveCount = 0;
    int selfWindows = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--move-existing" && i + 1 < argc) {
            moveCount = std::atoi(argv[++i]);
        } else if (arg == "--title" && i + 1 < argc) {
            static std::wstring filter;
            const int length = MultiByteToWideChar(CP_UTF8, 0, argv[++i], -1, nullptr, 0);
            if (length > 0) {
                filter.resize(static_cast<size_t>(length));
                MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, filter.data(), length);
                if (!filter.empty() && filter.back() == L'\0') filter.pop_back();
                titleFilter = filter.c_str();
            }
        } else if (arg == "--self-windows" && i + 1 < argc) {
            selfWindows = std::atoi(argv[++i]);
        }
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (selfWindows > 0) {
        const int result = RunSelfWindows(selfWindows);
        CoUninitialize();
        return result;
    }
    std::vector<Candidate> candidates = ListCandidates(titleFilter);
    for (size_t i = 0; i < candidates.size(); ++i) {
        const Candidate& candidate = candidates[i];
        std::wprintf(L"%zu hwnd=%p pid=%lu elevated=%d rect=(%ld,%ld,%ld,%ld) %ls\n",
                     i, reinterpret_cast<void*>(candidate.hwnd), candidate.pid,
                     candidate.elevated ? 1 : 0,
                     candidate.rect.left, candidate.rect.top,
                     candidate.rect.right, candidate.rect.bottom,
                     candidate.title.c_str());
    }
    int result = 0;
    if (moveCount > 0) result = MoveAndRestore(candidates, moveCount);
    CoUninitialize();
    return result;
}
