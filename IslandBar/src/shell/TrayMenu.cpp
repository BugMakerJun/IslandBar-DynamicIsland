// =====================================================================
//  TrayMenu.cpp -- WHAT A TRAY CELL DOES, AND WHAT ITS RIGHT-CLICK MENU OFFERS
//
//  These functions used to live inside main.cpp's anonymous namespace, next to the frame loop, the
//  command-line handlers and the composition wiring. They belong to the tray: they say what a cell means
//  (press it, or draw the menu the bar offers for it), and they are called from exactly two places -- the
//  frame loop when a cell is clicked, and the --tray-* commands when one is pressed from a script.
//
//  WHY THE MOVE. main.cpp is where the program is ASSEMBLED, and a reader had to walk past 1,300 lines of
//  command-line handlers and 230 lines of tray-menu rules to find the frame loop. The rule this file is
//  built on is the one the menu states in its own comment: this is the fallback for an unavailable
//  application menu, so every row must be something IslandBar can carry out itself.
//
//  Nothing here is new code: it is the same text, moved, with island:: qualification added at the call sites.
// =====================================================================
#include <windows.h>
#include <shellapi.h>   // ShellExecuteW: "open file location" goes through Explorer's own verb
#include <tlhelp32.h>   // the process snapshot that "end the application" walks
#include <cstdio>
#include <cwchar>
#include <cwctype>     // towlower: comparing an executable's name with a window's process image
#include <memory>
#include <new>
#include <objbase.h>   // CoInitializeEx: a press talks to COM, and WIN32_LEAN_AND_MEAN leaves it out of windows.h

#include "core/Log.h"
#include "domain/CardLayout.h"   // TrayMenuFacts / TrayMenuAction: the rows and their labels
#include "platform/WindowList.h"
#include "shell/TrayIcons.h"
#include "shell/TrayMenu.h"
#include "shell/TrayPress.h"

namespace island {

// The cell a press could not deal with, for the frame loop to open the bar's own menu over (see TrayMenu.h).
std::atomic<int> trayMenuRequestCell{-1};
std::atomic<int> trayPressDismissCell{-1};

namespace {

struct TrayPressJob {
    island::TrayPressRequest request;
    bool rightClick = false;
    int cell = -1;
};

WorkerThread g_trayPressWorker;

unsigned long __stdcall TrayPressThreadEntry(void* context) {
    std::unique_ptr<TrayPressJob> job(static_cast<TrayPressJob*>(context));
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const TrayPressOutcome outcome = PressTrayIcon(job->request, job->rightClick);
    LogAndPrint(L"  tray press: '%ls' -> %ls via %ls, menu_evidence=%ls, press %lld ms\n",
                job->request.name, TrayPressResultName(outcome.result), outcome.route,
                outcome.menuEvidence, outcome.ms);
    if (outcome.result == TrayPressResult::Pressed) {
        LogAndPrint(L"  tray press: delivered to %ls -- %ls\n", outcome.target, outcome.answer);
    }
    if (outcome.result == TrayPressResult::NoEntry ||
        outcome.result == TrayPressResult::NoWindow ||
        outcome.result == TrayPressResult::Refused) {
        trayMenuRequestCell.store(job->cell);
    } else if (outcome.result == TrayPressResult::Pressed && !job->rightClick) {
        trayPressDismissCell.store(job->cell);
    }
    std::fflush(stdout);
    if (SUCCEEDED(comHr)) CoUninitialize();
    return 0;
}

} // namespace

// One icon's press request, copied out of the list: a press runs on a worker thread and the list refreshes
// under it (see TrayPressRequest).
island::TrayPressRequest MakeTrayPressRequest(const island::TrayIcon& icon, const wchar_t* exePath) {
    island::TrayPressRequest request;
    request.uid = icon.uid;
    request.hasUid = icon.hasUid;
    request.hasGuid = icon.hasGuid;
    request.guid = icon.guid;
    wcsncpy_s(request.exePath, 260, exePath ? exePath : L"", _TRUNCATE);
    wcsncpy_s(request.name, 96, icon.name, _TRUNCATE);
    return request;
}

// An icon's name with its line breaks flattened, for a log line.
//
// Five of the sixteen names on this machine are multi-line tooltips ("Steam - <something>", "NVIDIA
// <driver>", the QQ Music one), and printing them raw put the REST of the log line on a line of its own --
// which reads exactly like a missing answer. Measured while reading the first run of --tray-surface.
const wchar_t* FlatName(const wchar_t* name, wchar_t* buffer, size_t capacity) {
    size_t out = 0;
    for (const wchar_t* p = name; p && *p && out + 1 < capacity; ++p) {
        const wchar_t c = *p;
        if (c == L'\r') continue;
        buffer[out++] = (c == L'\n' || c == L'\t') ? L' ' : c;
    }
    buffer[out] = L'\0';
    return buffer;
}

// Hands one icon to a worker thread: the press shows the shell's tray for about a fifth of a second and
// walks its overflow, which is not something the bar's own frame loop can spend time on. The request is a
// copy, because the list refreshes under it.
// The bare executable name out of a path ("Weixin.exe" from a full path), for comparing what the registry
// recorded with the process behind a window.
const wchar_t* BaseNameOf(const wchar_t* path) {
    if (!path) return L"";
    const wchar_t* base = std::wcsrchr(path, L'\\');
    return base ? base + 1 : path;
}

// Whether two bare names are the same image. Case-insensitive: the registry records what the application
// wrote, and Windows does not care about the case of an executable's name.
bool SameImage(const wchar_t* a, const wchar_t* b) {
    if (!a || !b || a[0] == L'\0' || b[0] == L'\0') return false;
    while (*a && *b) {
        if (std::towlower(*a) != std::towlower(*b)) return false;
        ++a;
        ++b;
    }
    return *a == L'\0' && *b == L'\0';
}

// Whether any process is running the registered image. A tray-only application can have no
// task window, so "no icon-row window" is not the same question as "not running".
bool ProcessImageRunning(const wchar_t* image) {
    if (!image || image[0] == L'\0') return false;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    bool running = false;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (SameImage(entry.szExeFile, image)) {
                running = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return running;
}

bool IsLaunchableFile(const wchar_t* path) {
    if (!path || path[0] == L'\0') return false;
    wchar_t expanded[MAX_PATH] = {};
    const DWORD length = ExpandEnvironmentStringsW(path, expanded, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return false;
    const DWORD attributes = GetFileAttributesW(expanded);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// Ends every process of that image, and returns how many were ended.
//
// THE FORCEFUL PATH, and the only one that can promise what the entry says. A tray application's window
// closes INTO the tray rather than quitting -- WeChat and QQ Music both do -- so "ask it to close" cannot
// deliver "the application is gone". This is what Task Manager's 结束任务 does, which is why it is offered
// only behind a second press (see TrayMenuArmedLabel) and why its label says 结束 rather than 关闭: the
// user is told which of the two they are asking for.
int TerminateProcessesOfImage(const wchar_t* image) {
    if (!image || image[0] == L'\0') return 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    int ended = 0;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            // `szExeFile` is the bare name, which is the same thing the tray's record is compared by.
            if (!SameImage(entry.szExeFile, image)) continue;
            HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, entry.th32ProcessID);
            if (!process) continue;
            if (TerminateProcess(process, 0)) ++ended;
            CloseHandle(process);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return ended;
}

// The first window of that image in the bar's OWN task list -- the same list the icon row draws, so "show
// the window" acts on the window whose icon the user would have clicked.
HWND WindowOfImageInList(island::WindowList& windows, const wchar_t* image) {
    if (!image || image[0] == L'\0') return nullptr;
    for (int i = 0; i < windows.Count(); ++i) {
        const island::TaskEntry& entry = windows.At(i);
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.processId);
        if (!process) continue;
        wchar_t path[MAX_PATH] = {};
        DWORD length = MAX_PATH;
        const bool havePath = QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
        CloseHandle(process);
        if (havePath && SameImage(BaseNameOf(path), image)) return entry.handle;
    }
    return nullptr;
}

const wchar_t* TrayLaunchResultName(TrayLaunchResult result) {
    switch (result) {
        case TrayLaunchResult::Activated: return L"activate";
        case TrayLaunchResult::Launched: return L"launch";
        case TrayLaunchResult::NoTarget: return L"no-target";
        case TrayLaunchResult::Failed: return L"failed";
    }
    return L"?";
}

TrayLaunchResult LaunchOrActivateTrayApplication(const TrayIcons& icons, WindowList& windows, int cell) {
    if (cell < 0 || cell >= icons.Count()) return TrayLaunchResult::NoTarget;

    const wchar_t* path = icons.ExePathAt(cell);
    if (!path || path[0] == L'\0') return TrayLaunchResult::NoTarget;

    const wchar_t* image = BaseNameOf(path);
    HWND window = WindowOfImageInList(windows, image);
    if (window && windows.Activate(window)) return TrayLaunchResult::Activated;

    // No normal task window means "let the application handle a second launch". ShellExecuteW is
    // what the Start card already uses, and single-instance applications commonly restore their
    // main window in response to it. Searching further down the HWND hierarchy would start
    // activating helper, message-only and hidden utility windows.
    if (!IsLaunchableFile(path)) return TrayLaunchResult::NoTarget;

    wchar_t expanded[MAX_PATH] = {};
    const DWORD length = ExpandEnvironmentStringsW(path, expanded, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return TrayLaunchResult::NoTarget;
    const HINSTANCE opened = ShellExecuteW(nullptr, L"open", expanded, nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(opened) > 32 ? TrayLaunchResult::Launched
                                                 : TrayLaunchResult::Failed;
}

void TrayMenuFactsFor(const island::TrayIcons& icons, island::WindowList& windows, int cell,
                      island::TrayMenuFacts* out) {
    if (!out) return;
    *out = island::TrayMenuFacts{};
    if (cell < 0 || cell >= icons.Count()) return;
    const wchar_t* path = icons.ExePathAt(cell);
    out->hasExePath = path && path[0] != L'\0';
    if (!out->hasExePath) return;
    const wchar_t* image = BaseNameOf(path);
    out->hasWindow = WindowOfImageInList(windows, image) != nullptr;
    out->processRunning = ProcessImageRunning(image);
    out->hasLaunchTarget = IsLaunchableFile(path);
}

bool RunTrayMenuAction(const island::TrayIcons& icons, island::WindowList& windows, int cell,
                       island::TrayMenuAction action) {
    if (cell < 0 || cell >= icons.Count()) return false;
    const wchar_t* path = icons.ExePathAt(cell);
    switch (action) {
        case island::TrayMenuAction::LaunchApplication: {
            const TrayLaunchResult result = LaunchOrActivateTrayApplication(icons, windows, cell);
            island::LogAndPrint(L"  tray menu: launch/activate '%ls' result=%ls\n", BaseNameOf(path),
                                TrayLaunchResultName(result));
            return result == TrayLaunchResult::Activated || result == TrayLaunchResult::Launched;
        }
        case island::TrayMenuAction::OpenLocation: {
            if (!path || path[0] == L'\0') return false;
            // Through Explorer's own "open file location", which is the same gesture the shell's menus
            // offer. Quoted, because a path with spaces in it is the normal case.
            wchar_t argument[512] = {};
            std::swprintf(argument, 512, L"/select,\"%ls\"", path);
            const HINSTANCE result = ShellExecuteW(nullptr, L"open", L"explorer.exe", argument, nullptr,
                                                   SW_SHOWNORMAL);
            island::LogAndPrint(L"  tray menu: opened the folder of '%ls' (result %lld)\n", path,
                                static_cast<long long>(reinterpret_cast<INT_PTR>(result)));
            std::fflush(stdout);
            return reinterpret_cast<INT_PTR>(result) > 32;
        }
        case island::TrayMenuAction::QuitApplication: {
            const int ended = TerminateProcessesOfImage(BaseNameOf(path));
            island::LogAndPrint(L"  tray menu: ended %d process(es) of '%ls'\n", ended,
                                BaseNameOf(path));
            std::fflush(stdout);
            return ended > 0;
        }
    }
    return false;
}

void StartTrayPress(const island::TrayIcons& icons, int cell, bool rightClick) {
    if (cell < 0 || cell >= icons.Count()) return;
    const island::TrayPressRequest request =
        MakeTrayPressRequest(icons.At(cell), icons.ExePathAt(cell));
    wchar_t flat[128] = {};
    // WIDE, every part of it: the icon's name can be Chinese (" 微信"), and a narrow format string with a
    // `%ls` for it empties the whole line (see Log.h -- the same trap that swallowed this press's result
    // line for a whole round). The narrow spelling that used to be here also passed a `%s` for the
    // "(right click)" suffix, which the wide engine cannot convert either.
    island::LogAndPrint(L"  tray press: cell %d '%ls'%ls\n", cell,
                        FlatName(icons.At(cell).name, flat, 128),
                        rightClick ? L" (right click)" : L"");
    std::fflush(stdout);
    auto* job = new (std::nothrow) TrayPressJob{request, rightClick, cell};
    if (!job) return;
    if (!g_trayPressWorker.StartUnmanaged(&TrayPressThreadEntry, job)) {
        delete job;
        island::LogAndPrint(L"  tray press: another press is still in flight; '%ls' was not sent\n",
                            request.name);
    }
}

WorkerThread::StopResult StopTrayPressWorker() {
    return g_trayPressWorker.Stop();
}

} // namespace island
