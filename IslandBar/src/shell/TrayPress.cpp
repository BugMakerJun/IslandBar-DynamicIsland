// =====================================================================
//  TrayPress.cpp -- see the header for the route and for what it is built on.
// =====================================================================
#include "shell/TrayPress.h"
#include "domain/TrayEncoding.h"   // the only place a click's two words are placed (see its header)

#include "shell/TrayUia.h"   // the fallback for the icons no message can ever reach

#include "core/Log.h"

#include <shellapi.h>
#include <tlhelp32.h>
// The COM declarations: the request this file is handed carries the icon's GUID, and with WIN32_LEAN_AND_MEAN
// windows.h does not declare one.
#include <objbase.h>

#include <algorithm>
#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace island {

// THE UI AUTOMATION FALLBACK IS OPT-IN. It can reach otherwise unpressable icons, but only by making the
// native taskbar visible for the lookup. The user's requirement is the opposite: an unlearned cell must open
// this bar's own menu without flashing Windows' taskbar. `ISLANDBAR_TRAY_UIA=1` restores the old coverage for
// diagnostics or a machine that accepts that flash; `ISLANDBAR_NO_TRAY_UIA=1` remains an explicit veto.
bool TrayUiaFallbackEnabled() {
    static const bool enabled = [] {
        wchar_t off[8] = {};
        if (GetEnvironmentVariableW(L"ISLANDBAR_NO_TRAY_UIA", off, 8) > 0 && off[0] == L'1') return false;
        wchar_t on[8] = {};
        return GetEnvironmentVariableW(L"ISLANDBAR_TRAY_UIA", on, 8) > 0 && on[0] == L'1';
    }();
    return enabled;
}

namespace {

// NOTHING ELSE LIVES AT THIS SCOPE ANY MORE, and that is worth stating: this file used to hold the taskbar a
// press borrowed (hidden-but-present, with its extended style) and the shell's overflow panel left invisible
// for the length of a right click, plus a watchdog handover to the frame loop that put both back. All of it
// existed to make a REAL CLICK possible, and the real click is gone -- see the top of TrayPress.h.
long long MillisSince(const ULONGLONG start) { return static_cast<long long>(GetTickCount64() - start); }

// The pids of every process whose image is `exePath`'s file name, case-insensitively.
//
// Toolhelp and a name comparison rather than OpenProcess per pid: the same choice TrayIcons makes, for
// the same reason -- this is a handful of milliseconds on a machine with hundreds of processes.
std::vector<DWORD> PidsForImage(const wchar_t* exePath) {
    std::vector<DWORD> pids;
    if (!exePath || !*exePath) return pids;
    const wchar_t* slash = std::wcsrchr(exePath, L'\\');
    std::wstring wanted = slash ? std::wstring(slash + 1) : std::wstring(exePath);
    std::transform(wanted.begin(), wanted.end(), wanted.begin(), ::towlower);

    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            std::wstring name = entry.szExeFile;
            std::transform(name.begin(), name.end(), name.begin(), ::towlower);
            if (name == wanted) pids.push_back(entry.th32ProcessID);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return pids;
}

// The windows of the processes in `pids`: every top-level one and every message-only one.
//
// Both, because a tray icon's owner is usually a window nobody ever sees: WeChat's is
// `Qt51514WxTrayIconMessageWindowClass`, QQ Music's is a `TXGuiFoundation` window, and others are
// message-only windows, which EnumWindows does not report at all (they are children of HWND_MESSAGE).
struct WindowSearch {
    const std::vector<DWORD>* pids = nullptr;
    std::vector<HWND> windows;
};

BOOL CALLBACK CollectWindow(HWND hwnd, LPARAM param) {
    WindowSearch* search = reinterpret_cast<WindowSearch*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (std::find(search->pids->begin(), search->pids->end(), pid) != search->pids->end()) {
        search->windows.push_back(hwnd);
    }
    return TRUE;
}

std::vector<HWND> WindowsOf(const std::vector<DWORD>& pids) {
    std::vector<HWND> windows;
    if (pids.empty()) return windows;
    WindowSearch search;
    search.pids = &pids;
    EnumWindows(&CollectWindow, reinterpret_cast<LPARAM>(&search));
    // Message-only windows live under a special parent handle, and are reached from it rather than from
    // the desktop: FindWindowExW(HWND_MESSAGE, ...) walks that separate list.
    HWND child = nullptr;
    while ((child = FindWindowExW(HWND_MESSAGE, child, nullptr, nullptr)) != nullptr) {
        DWORD pid = 0;
        GetWindowThreadProcessId(child, &pid);
        if (std::find(pids.begin(), pids.end(), pid) != pids.end()) search.windows.push_back(child);
    }
    return search.windows;
}

// What the press knows about the desktop before it clicks, and what it measures the click against.
//
// MEASURED, 2026-10-15, because the plan this replaced was built on a wrong assumption about what a tray
// menu IS on this machine: right-clicking WeGame's icon brings up a window of WeGame's OWN process (class
// `TWINCONTROL`, 250x280 at 2136,100 -- the click was at 2394,109, and the menu's top-right corner sits
// ten pixels from it). It is NOT a `#32768` Win32 popup menu; it is NOT in the UI Automation tree (asked
// the whole desktop for ControlType=Menu: zero, while the menu was painted on the screen); and it does not
// answer `WindowFromPoint` either (asked at the middle of it while it was painted: the desktop's
// `SysListView32`). So a search by class name, by automation, or by hit testing cannot see a tray menu
// here -- the only thing that can is the APPLICATION'S OWN WINDOW LIST, asked twice.
struct WindowWatch {
    std::vector<DWORD> pids;          // the application's processes
    std::vector<HWND> appWindows;     // every window of theirs, message-only ones included
    std::vector<HWND> appVisible;     // ... and which of those were already on screen
};

WindowWatch TakeWindowWatch(const TrayPressRequest& request) {
    WindowWatch watch;
    watch.pids = PidsForImage(request.exePath);
    watch.appWindows = WindowsOf(watch.pids);
    for (HWND window : watch.appWindows) {
        if (IsWindowVisible(window)) watch.appVisible.push_back(window);
    }
    return watch;
}

// One look: has anything of the application's come up since the snapshot? The window, its class and its
// rectangle are filled in when it has, and `out` describes it in words for the log.
//
// ... AND ONLY THE APPLICATION'S OWN WINDOWS COUNT. A first version also accepted "any new top-level window
// of another process, or one wearing the popup-menu class `#32768`" -- and that version was measured
// reporting a shell TOOLTIP as the application's answer: a left press on WeGame's icon produced
// `Xaml_WindowedPopupClass ... 130x41`, which is the icon's own tooltip, while WeGame itself did nothing.
// A log line that says the application answered when it did not is worse than one that says nothing, so the
// question is now asked of the application and of nobody else. If a future shell draws a tray menu on the
// application's behalf, this will say "nothing came up" about a menu that IS up, and that is the known and
// documented limit of the criterion.
bool AnyWindowCameUp(const WindowWatch& watch, wchar_t* out, size_t capacity, HWND* handle, RECT* rectOut) {
    const auto describe = [out, capacity](const wchar_t* cls, const RECT& rect, const wchar_t* whose) {
        if (out && capacity > 0) {
            std::swprintf(out, capacity, L"its own '%ls' at %ld,%ld %ldx%ld (%ls)", cls, rect.left,
                          rect.top, rect.right - rect.left, rect.bottom - rect.top, whose);
        }
    };
    for (HWND window : WindowsOf(watch.pids)) {
        if (!IsWindowVisible(window)) continue;
        if (std::find(watch.appVisible.begin(), watch.appVisible.end(), window) != watch.appVisible.end()) {
            continue;
        }
        RECT rect = {};
        if (!GetWindowRect(window, &rect)) continue;
        if (rect.right <= rect.left || rect.bottom <= rect.top) continue;
        wchar_t cls[96] = {};
        GetClassNameW(window, cls, 96);
        describe(cls, rect, L"a window of its own that was not on screen before");
        if (handle) *handle = window;
        if (rectOut) *rectOut = rect;
        return true;
    }
    return false;
}

// ... AND A WINDOW THAT WENT AWAY IS AN ANSWER TOO, which the first version of the watch could not see.
//
// Measured on clash-verge (2026-10-15): its tray icon TOGGLES its window, so a press that takes the window
// off the screen looked to this file exactly like a press that was ignored -- "no new window, and the
// foreground did not move to it" -- and the verdict that got written into the cache was "this application
// ignores its own callback", which is the one line in that file that permanently disables the invisible
// route for an application. The press then fell back to a real click, which put the window back, so every
// test looked like a success while the mechanism being tested kept being declared broken.
//
// The comparison is against the snapshot taken BEFORE the press: any of the application's own visible
// top-level windows that is no longer visible means it acted.
bool AnyWindowWentAway(const WindowWatch& watch, wchar_t* out, size_t capacity) {
    for (HWND window : watch.appVisible) {
        if (IsWindow(window) && IsWindowVisible(window)) continue;
        wchar_t cls[96] = {};
        GetClassNameW(window, cls, 96);
        if (out && capacity > 0) {
            std::swprintf(out, capacity, L"its own '%ls' was on screen before the press and is not now",
                          cls);
        }
        return true;
    }
    return false;
}

// Did the application do anything? The rule this exists for is the review's: a clean HRESULT is not an
// answer -- "the click never arrived" and "the click arrived and was ignored" look identical from here, and
// only the application's own windows can tell them apart.
//
// TWO QUESTIONS, both observable from outside any application and neither fakeable by a press that
// "succeeded": a window of the application's that came up, or the foreground moving to one of the
// application's windows. The budget is 1.5 s rather than the 200 ms the plan asked for, and the reason is
// measured: WeGame is a launcher, and the first run of this watch said "nothing" 200 ms after a click that
// a longer look would have seen -- the pointer is already back where the user left it by then, so the wait
// costs nobody anything.
bool WatchApplicationForAnswer(const WindowWatch& watch, HWND foregroundBefore, wchar_t* out,
                               size_t capacity, long long budgetMs) {
    const auto say = [out, capacity](const wchar_t* text) {
        if (out && capacity > 0) wcsncpy_s(out, capacity, text, _TRUNCATE);
    };
    say(L"nothing of its own appeared");
    if (watch.pids.empty()) {
        say(L"none of its processes is running");
        return false;
    }
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        if (AnyWindowCameUp(watch, out, capacity, nullptr, nullptr)) return true;
        if (AnyWindowWentAway(watch, out, capacity)) return true;
        const HWND foreground = GetForegroundWindow();
        if (foreground && foreground != foregroundBefore) {
            DWORD pid = 0;
            GetWindowThreadProcessId(foreground, &pid);
            if (std::find(watch.pids.begin(), watch.pids.end(), pid) != watch.pids.end()) {
                wchar_t cls[96] = {};
                GetClassNameW(foreground, cls, 96);
                if (out && capacity > 0) {
                    std::swprintf(out, capacity, L"the foreground window is now its '%ls'", cls);
                }
                return true;
            }
        }
        if (MillisSince(start) >= budgetMs) {
            if (out && capacity > 0) {
                std::swprintf(out, capacity, L"nothing of its own appeared, and the foreground did not move "
                                             L"to it, in %lld ms", budgetMs);
            }
            return false;
        }
        Sleep(30);
    }
}

// THE APPLICATION'S MAIN WINDOW, as Windows defines one in the absence of an API that names it: a visible
// top-level window of the application's that has no owner and is not a tool window, the largest of those.
HWND MainWindowOf(const std::vector<DWORD>& pids) {
    HWND best = nullptr;
    long long bestArea = 0;
    for (HWND window : WindowsOf(pids)) {
        if (!IsWindowVisible(window)) continue;
        if (GetWindow(window, GW_OWNER) != nullptr) continue;
        const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
        const LONG_PTR extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
        if ((extended & WS_EX_TOOLWINDOW) != 0) continue;
        // Something the user could recognise as "the application's window": it has a title bar or a resize
        // border. Without this the largest window of a tray application is often an invisible-though-visible
        // helper that fills the screen.
        if ((style & WS_CAPTION) == 0 && (style & WS_THICKFRAME) == 0) continue;
        RECT rect = {};
        if (!GetWindowRect(window, &rect)) continue;
        const long long area = static_cast<long long>(rect.right - rect.left) * (rect.bottom - rect.top);
        if (area > bestArea) {
            bestArea = area;
            best = window;
        }
    }
    return best;
}

// BRINGING THE APPLICATION FORWARD WITHOUT A CLICK -- the other half of "press a tray icon", and the half a
// posted message could not do on its own.
//
// Measured (HANDOFF 8.105), and this is the reason the message route was right-click-only for a day: five
// different notification codes were posted to WeChat's own tray window, every one of them was delivered,
// every one was dequeued by the application, and its main window did not come to the front once. The cause
// is a rule rather than a bug -- a process that is not in the foreground may not take the foreground, and an
// application restoring its window from a background callback runs straight into it. A real click satisfies
// the rule, which is exactly why the pointer-moving route always worked.
//
// Two calls make the application's own attempt work instead of overriding it: the right to take the
// foreground is granted to the application BEFORE the message is posted (see SendTrayCallback), and if the
// application still does not come forward, ITS OWN main window is brought forward here -- which this process
// is allowed to do, because the click that started this press left OUR window in the foreground.
bool BringApplicationForward(const WindowWatch& watch, wchar_t* out, size_t capacity) {
    const auto say = [out, capacity](const wchar_t* text) {
        if (out && capacity > 0) wcsncpy_s(out, capacity, text, _TRUNCATE);
    };
    const HWND window = MainWindowOf(watch.pids);
    if (!window) {
        say(L"it has no window of its own to bring forward");
        return false;
    }
    if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
    if (!SetForegroundWindow(window)) {
        say(L"Windows refused to bring its own window to the front");
        return false;
    }
    wchar_t cls[96] = {};
    GetClassNameW(window, cls, 96);
    if (out && capacity > 0) {
        std::swprintf(out, capacity, L"brought its own '%ls' to the front", cls);
    }
    return true;
}

} // namespace

// ---- the posted-callback route, learned once per application (2026-10-15 late) ------------------------
//
// MEASURED, and this is the whole reason it exists: an application's tray icon can be pressed WITHOUT
// touching the pointer, by posting the application's own callback message to its own tray window. On WeChat
// that put its tray menu -- 退出微信 and all -- at the coordinates this bar passed, with the pointer sitting
// still, and nothing injected anywhere (HANDOFF 8.104; the screenshot is captures/wx-A1.png).
//
// THE LAYOUT IS THE PART THAT COSTS (NOTIFYICONDATA.uCallbackMessage):
//
//   v4 (modern):  LOWORD(lParam) = the notification, HIWORD(lParam) = the icon's uid,
//                 and the ANCHOR POINT is in wParam (low word x, high word y).
//   v3 (older):   wParam = uid, lParam = the mouse message -- WM_LBUTTONUP for a left click, WM_RBUTTONUP
//                 for a right one. NOT NIN_SELECT: measured 2026-10-15, posting NIN_SELECT to clash-verge
//                 and to Steam (neither ever calls NIM_SETVERSION, so both are v3) was delivered, was
//                 dequeued by the application, and produced nothing at all, while the shell's own click on
//                 the very same icon opens the window -- because the shell sends the mouse message. The
//                 silence was then read as "this application ignores its callback" and written into the
//                 cache, which is a verdict that may only be given to a correct message.
//
// The first two attempts used the v3-looking spelling with coordinates bolted into lParam's high word; both
// were delivered, both were retrieved by the application, and both were ignored. This is not guessable.
//
// WHAT IS CACHED, AND WHY A FILE. Per application: the message number, the class of the window it has to be
// posted to, and the icon's uid. The message number cannot be looked up anywhere (the shell's registry entry
// carries UID, IconGuid, path, tooltip and icon, and no callback message) and cannot be observed from this
// side either (the shell delivers it without it passing through any queue this process can see), so it is
// LEARNED once with a short Win32k trace -- scripts/probe-tray-callback-etw.ps1 -- and written here:
//
//     %USERPROFILE%\IslandBar\tray_callbacks.txt
//     # exe|message|window class|uid
//     Weixin.exe|0x8385|Qt51514WxTrayIconMessageWindowClass|0
//
// An application with no entry opens the bar's own menu. The optional UI Automation experiment can still be
// enabled explicitly, but it is not part of the default press path.
struct TrayCallback {
    wchar_t exe[64] = {};
    unsigned message = 0;
    wchar_t windowClass[128] = {};
    unsigned uid = 0;
    // WHICH RECIPE A LEFT PRESS TAKES, once one has been measured to work for this application. THREE
    // RECIPES ARE POSSIBLE and each one is a complete spelling -- the notification, whether the matching
    // mouse DOWN goes first, and WHICH PACKING. `Unknown` means "try the documented one, then the measured
    // one"; see the note where the attempts are built, and note that the packing is part of the recipe
    // rather than a consequence of the version: measured, an application that never calls NIM_SETVERSION
    // and an application that claims version 4 can want the same raw mouse messages, and the second attempt
    // of a press has to be allowed to disagree with the version field.
    enum class LeftSpelling { Unknown, Select, DownThenUp3, DownThenUp4 };
    LeftSpelling leftSpelling = LeftSpelling::Unknown;
    // WHICH PRESS THIS ENTRY IS ALLOWED TO CARRY. `right` was written by the learning pass for a while, back
    // when a left press had to be a real click (the foreground rule, HANDOFF 8.105: an application restoring
    // its own window from a background callback is refused the foreground). The measured pair of sent mouse
    // messages (8.109) removed that reason, so the learning pass writes `both` again -- and this flag stays
    // because a hand-written line may still say `right`, and because an application that ignores a left press
    // but answers a right one is a real thing to have found.
    bool rightClickOnly = false;
    // THE VERSION THE APPLICATION REGISTERED WITH, and it decides how the callback is packed. v4 (4) carries
    // the anchor in wParam and the notification plus the uid in lParam; v3 (and 0, which means "never said")
    // carries the uid in wParam and the mouse message in lParam, with the anchor left to the current cursor --
    // which is the cell the user just pressed, so the menu still lands where it should.
    unsigned version = 4;
    // THE APPLICATION DID NOTHING OBSERVABLE when its callback was delivered, so a press of this icon from
    // now on reports that instead of spending a second and a half finding it out again. Learned the same way
    // as everything else here: by trying it (see the note in SendTrayCallback). The cache spells it `none`.
    bool ignoresCallback = false;
    // THE GUID THE APPLICATION NAMED THIS ICON WITH, when it did (the cache's eighth field). Compared as
    // text: the registry's own GUID string and the one the module reported come from the same API.
    wchar_t guid[64] = {};
};

std::wstring BaseNameLower(const wchar_t* path) {
    if (!path || !*path) return std::wstring();
    const wchar_t* slash = std::wcsrchr(path, L'\\');
    std::wstring name = slash ? std::wstring(slash + 1) : std::wstring(path);
    std::transform(name.begin(), name.end(), name.begin(), ::towlower);
    return name;
}

// The learned entries, read once per press. A missing file is not an error: it means "nothing learned yet",
// which is the state every application starts in.
std::vector<TrayCallback> ReadTrayCallbacks() {
    std::vector<TrayCallback> entries;
    wchar_t home[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return entries;
    std::wstring file = std::wstring(home) + L"\\IslandBar\\tray_callbacks.txt";

    FILE* stream = nullptr;
    if (_wfopen_s(&stream, file.c_str(), L"r, ccs=UTF-8") != 0 || !stream) return entries;
    wchar_t line[512] = {};
    while (std::fgetws(line, 512, stream)) {
        std::wstring text = line;
        const size_t comment = text.find(L'#');
        if (comment != std::wstring::npos) text = text.substr(0, comment);
        std::vector<std::wstring> fields;
        size_t start = 0;
        for (;;) {
            const size_t bar = text.find(L'|', start);
            std::wstring field = text.substr(start, bar == std::wstring::npos ? std::wstring::npos
                                                                              : bar - start);
            while (!field.empty() && (field.back() == L'\r' || field.back() == L'\n' ||
                                      field.back() == L' ' || field.back() == L'\t')) {
                field.pop_back();
            }
            while (!field.empty() && (field.front() == L' ' || field.front() == L'\t')) field.erase(0, 1);
            fields.push_back(field);
            if (bar == std::wstring::npos) break;
            start = bar + 1;
        }
        if (fields.size() < 3 || fields[0].empty() || fields[1].empty()) continue;
        TrayCallback entry;
        std::transform(fields[0].begin(), fields[0].end(), entry.exe, ::towlower);
        const bool hex = fields[1].size() > 2 && fields[1][0] == L'0' &&
                         (fields[1][1] == L'x' || fields[1][1] == L'X');
        entry.message = static_cast<unsigned>(
            std::wcstoul(fields[1].c_str() + (hex ? 2 : 0), nullptr, hex ? 16 : 10));
        wcsncpy_s(entry.windowClass, fields[2].c_str(), _TRUNCATE);
        if (fields.size() > 3) entry.uid = static_cast<unsigned>(std::wcstoul(fields[3].c_str(), nullptr, 10));
        if (fields.size() > 4) {
            entry.rightClickOnly = _wcsicmp(fields[4].c_str(), L"right") == 0;
            entry.ignoresCallback = _wcsicmp(fields[4].c_str(), L"none") == 0 ||
                                    _wcsicmp(fields[4].c_str(), L"click") == 0;   // `click`: written by the
                                                                                 // build that still had a real
                                                                                 // click to fall back to
        }
        // The learned files carry the registered version from HANDOFF 8.107 on; a line written before that
        // says nothing, and the default is what such a line was written for.
        if (fields.size() > 5) {
            const unsigned parsed = static_cast<unsigned>(std::wcstoul(fields[5].c_str(), nullptr, 10));
            entry.version = (parsed == 0 || parsed == 3 || parsed == 4) ? parsed : 4;
        }
        // THE REST BY SHAPE, NOT BY POSITION: the recipe is a word and the GUID is a braced token, and which
        // field each one lands in depends on which of them the line carries at all (see TrayLearn.cpp, where
        // the same mistake made a GUID icon look unpressable while its entry sat in the file).
        for (size_t field = 6; field < fields.size(); ++field) {
            const std::wstring& value = fields[field];
            if (value.empty()) continue;
            if (value.front() == L'{') {
                wcsncpy_s(entry.guid, value.c_str(), _TRUNCATE);
            } else if (_wcsicmp(value.c_str(), L"select") == 0) {
                entry.leftSpelling = TrayCallback::LeftSpelling::Select;
            } else if (_wcsicmp(value.c_str(), L"downup4") == 0) {
                entry.leftSpelling = TrayCallback::LeftSpelling::DownThenUp4;
            } else if (_wcsicmp(value.c_str(), L"downup") == 0 ||
                       _wcsicmp(value.c_str(), L"downup3") == 0) {
                entry.leftSpelling = TrayCallback::LeftSpelling::DownThenUp3;
            }
        }
        if (entry.message) entries.push_back(entry);
    }
    std::fclose(stream);
    return entries;
}

// =====================================================================
//  WHAT THIS ICON HAS SHOWN ABOUT ITS RIGHT BUTTON, IN ITS OWN FILE
// =====================================================================
//
// %USERPROFILE%\IslandBar\tray_right.txt, one line per icon: `exe|uid|word` (see TrayRightNote in
// domain/TrayEncoding.h). It is NOT kept in the learned cache because the learning pass rewrites those lines
// wholesale from what applications register, and a verdict that cost a press to earn must not be lost by a
// pass that knows nothing about menus. Same directory, same name-keyed shape as everything else the tray
// writes, so a line can be read -- or deleted -- by hand.
std::wstring RightNotePath() {
    wchar_t home[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH) == 0) return std::wstring();
    return std::wstring(home) + L"\\IslandBar\\tray_right.txt";
}

// One icon's identity inside that file: `uid`, or `uid|guid` when the application named the icon by guid.
// See ReadRightNote for the measurement that forced the guid in.
std::wstring RightNoteIdentity(const TrayPressRequest& request) {
    std::wstring identity = std::to_wstring(request.uid);
    if (request.hasGuid) {
        // The same spelling the cache uses for a guid (`StringFromGUID2`), so a human reading either file sees
        // the same string for the same icon.
        wchar_t text[64] = {};
        StringFromGUID2(request.guid, text, 64);
        identity += L"|";
        identity += text;
    }
    return identity;
}

TrayRightNote ReadRightNote(const TrayPressRequest& request) {
    const std::wstring file = RightNotePath();
    if (file.empty()) return TrayRightNote::Untried;
    FILE* stream = nullptr;
    if (_wfopen_s(&stream, file.c_str(), L"r, ccs=UTF-8") != 0 || !stream) return TrayRightNote::Untried;
    const std::wstring exe = BaseNameLower(request.exePath);
    // THE KEY IS THE ICON'S WHOLE IDENTITY, GUID INCLUDED. Keying on the uid alone puts every GUID-registered
    // icon of one executable on the same line -- and Explorer alone has four of them here, each with a uid of 0
    // in the registry record and a guid that is the identity the shell uses. Measured the moment this file was
    // first written: `chatgpt.exe|0|both-failed` for an icon whose cache line says uid 3.
    const std::wstring wanted = RightNoteIdentity(request);
    wchar_t buffer[512] = {};
    TrayRightNote note = TrayRightNote::Untried;
    while (std::fgetws(buffer, 512, stream)) {
        std::wstring text = buffer;
        std::vector<std::wstring> fields;
        size_t start = 0;
        for (;;) {
            const size_t bar = text.find(L'|', start);
            std::wstring field = text.substr(start, bar == std::wstring::npos ? std::wstring::npos : bar - start);
            while (!field.empty() &&
                   (field.back() == L'\r' || field.back() == L'\n' || field.back() == L' ')) {
                field.pop_back();
            }
            fields.push_back(field);
            if (bar == std::wstring::npos) break;
            start = bar + 1;
        }
        if (fields.size() < 3) continue;
        std::wstring name = fields[0];
        std::transform(name.begin(), name.end(), name.begin(), ::towlower);
        // Everything between the executable and the word is the identity: `uid`, or `uid|guid`.
        std::wstring identity = fields[1];
        if (fields.size() >= 4) identity += L"|" + fields[2];
        if (name != exe || identity != wanted) continue;
        const size_t word = fields.size() >= 4 ? 3 : 2;
        if (fields.size() <= word) continue;
        for (int i = 0; i <= static_cast<int>(TrayRightNote::BothFailed); ++i) {
            const auto candidate = static_cast<TrayRightNote>(i);
            if (_wcsicmp(fields[word].c_str(), TrayRightNoteName(candidate)) == 0) note = candidate;
        }
    }
    std::fclose(stream);
    return note;
}

void WriteRightNote(const TrayPressRequest& request, TrayRightNote note) {
    const std::wstring file = RightNotePath();
    if (file.empty()) return;
    const std::wstring exe = BaseNameLower(request.exePath);
    const std::wstring wanted = RightNoteIdentity(request);
    std::vector<std::wstring> lines;
    {
        FILE* stream = nullptr;
        if (_wfopen_s(&stream, file.c_str(), L"r, ccs=UTF-8") == 0 && stream) {
            wchar_t buffer[512] = {};
            while (std::fgetws(buffer, 512, stream)) lines.push_back(buffer);
            std::fclose(stream);
        }
    }
    wchar_t fresh[320] = {};
    std::swprintf(fresh, 320, L"%ls|%ls|%ls\n", exe.c_str(), wanted.c_str(), TrayRightNoteName(note));
    bool replaced = false;
    for (std::wstring& line : lines) {
        const size_t bar = line.find(L'|');
        // THE LAST SEPARATOR ON THE LINE IS THE ONE BEFORE THE WORD, and the identity is everything between it
        // and the first one -- `uid`, or `uid|guid`. Using the SECOND separator instead (which is what this did
        // first) reads `0|{guid}|pair-failed` as the identity, never matches, and appends a second line for the
        // same icon: measured, `chatgpt.exe|0|{E5768D8B-...}|pair-failed` and the same key again with
        // `both-failed` beneath it.
        const size_t lastBar = line.find_last_of(L'|');
        if (bar == std::wstring::npos || lastBar == std::wstring::npos || lastBar <= bar) continue;
        std::wstring name = line.substr(0, bar);
        std::transform(name.begin(), name.end(), name.begin(), ::towlower);
        const std::wstring identity = line.substr(bar + 1, lastBar - bar - 1);
        if (name != exe || identity != wanted) continue;
        line = fresh;
        replaced = true;
        break;
    }
    if (!replaced) lines.push_back(fresh);
    FILE* stream = nullptr;
    if (_wfopen_s(&stream, file.c_str(), L"w, ccs=UTF-8") != 0 || !stream) return;
    for (const std::wstring& line : lines) std::fputws(line.c_str(), stream);
    std::fclose(stream);
    LogAndPrint(L"  tray press: '%ls' right button is now recorded as %ls\n", request.name,
                TrayRightNoteName(note));
    std::fflush(stdout);
}

// The application's window of the class the entry names. False when it is not running, has no such window
// yet (a tray icon that has not registered), or when nothing is known about it.
bool CallbackWindowFor(const TrayPressRequest& request, const TrayCallback& entry, HWND* out) {
    const std::vector<DWORD> pids = PidsForImage(request.exePath);
    if (pids.empty()) return false;
    for (HWND window : WindowsOf(pids)) {
        wchar_t cls[128] = {};
        if (!GetClassNameW(window, cls, 128)) continue;
        if (_wcsicmp(cls, entry.windowClass) == 0) {
            *out = window;
            return true;
        }
    }
    return false;
}

// Rewrites the entry's cache line as `none`: this application was given its own callback and did nothing
// observable with it, so from now on a press of that icon says so instead of spending a second and a half
// discovering the same thing again. (`click` used to be the word, back when the fallback was a real click;
// the fallback is gone and a cache file written by that build would be read as "unknown" -- which is why the
// word changed rather than the meaning.) The line stays in the file, so it can be edited or deleted by hand.
void MarkCallbackAsIgnored(const TrayPressRequest& request) {
    // Only for a uid-carrying icon: a GUID-registered one is matched by its GUID, and rewriting the uid line
    // of that executable would be a verdict about a different icon.
    if (!request.hasUid) return;
    wchar_t home[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH) == 0) return;
    const std::wstring file = std::wstring(home) + L"\\IslandBar\\tray_callbacks.txt";
    FILE* stream = nullptr;
    if (_wfopen_s(&stream, file.c_str(), L"r, ccs=UTF-8") != 0 || !stream) return;
    std::vector<std::wstring> lines;
    wchar_t buffer[1024] = {};
    while (fgetws(buffer, 1024, stream)) lines.push_back(buffer);
    fclose(stream);

    const std::wstring exe = BaseNameLower(request.exePath);
    bool changed = false;
    for (std::wstring& line : lines) {
        if (line.empty() || line[0] == L'#') continue;
        const size_t first = line.find(L'|');
        const size_t second = first == std::wstring::npos ? std::wstring::npos : line.find(L'|', first + 1);
        const size_t third = second == std::wstring::npos ? std::wstring::npos : line.find(L'|', second + 1);
        const size_t fourth = third == std::wstring::npos ? std::wstring::npos : line.find(L'|', third + 1);
        if (fourth == std::wstring::npos) continue;
        std::wstring name = line.substr(0, first);
        std::transform(name.begin(), name.end(), name.begin(), ::towlower);
        const unsigned uid = static_cast<unsigned>(std::wcstoul(line.substr(third + 1).c_str(), nullptr, 10));
        if (name != exe || (request.hasUid && uid != request.uid)) continue;
        const size_t end = line.find_first_of(L"\r\n", fourth + 1);
        const std::wstring tail = end == std::wstring::npos ? std::wstring() : line.substr(end);
        line = line.substr(0, fourth + 1) + L"none" + tail;
        changed = true;
        break;
    }
    if (!changed) return;
    if (_wfopen_s(&stream, file.c_str(), L"w, ccs=UTF-8") != 0 || !stream) return;
    for (const std::wstring& line : lines) fputws(line.c_str(), stream);
    fclose(stream);
    LogAndPrint(L"  tray press: the cache now says 'none' for %ls -- its callback is known to be ignored, so "
                L"that cell will not spend a second discovering it again\n",
                exe.c_str());
}

// Records which spelling of a LEFT press this application answered, as the seventh field of its cache line.
//
// The press tries the documented notification first and the shell's own measured sequence second (see the
// note where the attempts are built), which costs up to two deliveries and one short wait. Writing down
// which one worked makes every later press of that cell a single delivery again -- and, more importantly,
// turns a guess into a fact about this application that the next session does not have to re-measure.
void MarkCallbackLeftSpelling(const TrayPressRequest& request, TrayCallback::LeftSpelling spelling) {
    if (spelling == TrayCallback::LeftSpelling::Unknown) return;
    if (!request.hasUid) return;   // see MarkCallbackAsIgnored
    wchar_t home[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH) == 0) return;
    const std::wstring file = std::wstring(home) + L"\\IslandBar\\tray_callbacks.txt";
    FILE* stream = nullptr;
    if (_wfopen_s(&stream, file.c_str(), L"r, ccs=UTF-8") != 0 || !stream) return;
    std::vector<std::wstring> lines;
    wchar_t buffer[1024] = {};
    while (fgetws(buffer, 1024, stream)) lines.push_back(buffer);
    fclose(stream);

    const wchar_t* word = spelling == TrayCallback::LeftSpelling::Select   ? L"select"
                          : spelling == TrayCallback::LeftSpelling::DownThenUp4 ? L"downup4"
                                                                              : L"downup3";
    const std::wstring exe = BaseNameLower(request.exePath);
    bool changed = false;
    for (std::wstring& line : lines) {
        if (line.empty() || line[0] == L'#') continue;
        const size_t first = line.find(L'|');
        const size_t second = first == std::wstring::npos ? std::wstring::npos : line.find(L'|', first + 1);
        const size_t third = second == std::wstring::npos ? std::wstring::npos : line.find(L'|', second + 1);
        const size_t fourth = third == std::wstring::npos ? std::wstring::npos : line.find(L'|', third + 1);
        if (fourth == std::wstring::npos) continue;
        std::wstring name = line.substr(0, first);
        std::transform(name.begin(), name.end(), name.begin(), ::towlower);
        const unsigned uid = static_cast<unsigned>(std::wcstoul(line.substr(third + 1).c_str(), nullptr, 10));
        if (name != exe || (request.hasUid && uid != request.uid)) continue;
        if (line.find(word) != std::wstring::npos) return;   // already written down: nothing to do
        const size_t end = line.find_first_of(L"\r\n", fourth + 1);
        const std::wstring tail = end == std::wstring::npos ? std::wstring() : line.substr(end);
        const std::wstring body = line.substr(0, end == std::wstring::npos ? line.size() : end);
        const std::wstring fields = body + L"|" + word;
        line = fields + tail;
        changed = true;
        break;
    }
    if (!changed) return;
    if (_wfopen_s(&stream, file.c_str(), L"w, ccs=UTF-8") != 0 || !stream) return;
    for (const std::wstring& line : lines) fputws(line.c_str(), stream);
    fclose(stream);
    LogAndPrint(L"  tray press: the cache now says a left press of %ls is '%ls' -- one delivery from now on\n",
                exe.c_str(), word);
}

// THE DELIVERY IS PART OF THE SPELLING, and this is measured rather than reasoned: the module that watches
// FROM INSIDE the application (see TrayHook.cpp) recorded what the shell really sends clash-verge when its
// tray icon is pressed, and it is TWO messages SENT -- not posted --
//
//     msg 0x1772 wparam 0x00000002 lparam 0x00000201   WM_LBUTTONDOWN
//     msg 0x1772 wparam 0x00000002 lparam 0x00000202   WM_LBUTTONUP
//
// while the same values POSTED (in every one of eight shapes, both buttons' worth of packing, with and
// without the DOWN first) produced nothing at all. A message that is sent is delivered to the window
// procedure while the sender waits; a posted one is queued. The shell uses SendMessage, and the application
// is entitled to rely on it -- a handler that reads the mouse state, the capture state, or the message
// position sees something quite different for a queued message.
//
// SendMessageTimeout and not SendMessage: this runs on the bar's press worker while an application of
// unknown health is on the other side, and a hung application must cost a bounded wait, not the bar.
//
// ... AND A TIMEOUT IS NOT A FAILURE (2026-10-16, measured against the textbook handler the user pasted).
// The canonical tray handler opens its menu INSIDE the message -- GetCursorPos, SetForegroundWindow,
// TrackPopupMenu -- and TrackPopupMenu does not return until the menu is dismissed. So the send times out
// while the application's own menu is on screen, and calling that "the message could not be delivered" was
// wrong twice over: the log said failure while the menu was up (screenshot in HANDOFF 8.150), and the
// caller's `Refused != Pressed` made the card open THIS BAR's menu on top of the application's. A timeout
// here means "delivered, and its answer is not something this call can wait for"; the observer that
// follows is what decides.
BOOL DeliverMessage(HWND window, UINT message, WPARAM wparam, LPARAM lparam, bool sendNotPost) {
    if (!sendNotPost) return PostMessageW(window, message, wparam, lparam);
    DWORD_PTR ignored = 0;
    if (SendMessageTimeoutW(window, message, wparam, lparam, SMTO_ABORTIFHUNG | SMTO_NORMAL, 1500,
                            &ignored) != 0) {
        return TRUE;
    }
    // A window that is gone answers ERROR_INVALID_WINDOW_HANDLE and IS a failure; a window that is still
    // holding the message is the application's menu being open.
    return GetLastError() == ERROR_TIMEOUT;
}

// Sends that callback and waits for the application to answer. False means "this route was not taken" --
// nothing learned, no such window, or the post itself failed -- and the caller is expected to fall back.
//
// THE ANCHOR IS WHERE THE POINTER ALREADY IS. The user pressed a cell in this bar's tray card, so the pointer
// is on that cell and has not been moved: passing it through is what puts the menu under the cell the user
// clicked rather than at the application's own icon in the screen's corner. (On WeChat this is measured: the
// menu came up at exactly the coordinates passed in.)
bool SendTrayCallback(const TrayPressRequest& request, bool rightClick, TrayPressOutcome* outcome) {
    // The clock for the ONE line a right press owes the reader (see the two exits below): which shape went
    // out, whether the application answered with its own menu, and how long that took.
    const ULONGLONG rightPressStart = GetTickCount64();
    const std::vector<TrayCallback> entries = ReadTrayCallbacks();
    const std::wstring exe = BaseNameLower(request.exePath);
    const TrayCallback* entry = nullptr;
    for (const TrayCallback& candidate : entries) {
        if (exe != candidate.exe) continue;
        // THE UID IS PART OF THE IDENTITY, and that is a correction rather than a description: this lookup
        // used to take the first line of that application, which was right while every application had
        // exactly one icon and one hand-written line. Learning changed that -- explorer alone registers
        // several, and one of the lines this file learned is the Bluetooth icon -- so an exe-only match would
        // deliver the BLUETOOTH icon's message to whichever explorer cell the user pressed.
        //
        // AN ICON REGISTERED BY GUID IS MATCHED BY THAT GUID. It has no uid to compare (the registry entry
        // carries the GUID instead, and the uid both sides report is 0), and matching on that shared zero --
        // which an earlier version of this lookup did -- is how Explorer's network and volume icons would have
        // been sent the Bluetooth icon's callback. The GUID is what the application itself named the icon
        // with, and the module reports it at registration (see TrayHook.cpp, and the cache's eighth field).
        // THE GUID WINS WHEN THERE IS ONE, and the registry entry often carries both: Explorer's Bluetooth
        // icon has a uid of 0 AND a GUID, and the uid is not an identity (several of the shell's icons share
        // that zero). So a request that knows a GUID is matched on it, and only a request without one falls
        // back to the uid.
        if (request.hasGuid) {
            wchar_t wanted[64] = {};
            StringFromGUID2(request.guid, wanted, 64);
            if (_wcsicmp(candidate.guid, wanted) != 0) continue;
        } else if (request.hasUid) {
            if (candidate.uid != request.uid || candidate.guid[0]) continue;
        } else {
            continue;   // neither identity: nothing to match on, so nothing is known about it
        }
        entry = &candidate;
        break;
    }
    // EACH WAY OUT SAYS WHY, because the caller turns "false" into what the log and the card are told, and a
    // press that reports "nothing is known about this icon" when an entry exists but its window does not is
    // the kind of line this project has been burned by (measured: a Steam right press reported "no such
    // window" about an entry whose only problem was the left-button verdict).
    const auto fail = [outcome](TrayPressResult result, const wchar_t* route) {
        if (outcome) {
            outcome->result = result;
            outcome->route = route;
        }
        return false;
    };
    if (!entry) return fail(TrayPressResult::NoEntry, L"nothing learned for this icon yet");
    if (entry->rightClickOnly && !rightClick) {
        return fail(TrayPressResult::NoEntry, L"its entry carries a right press only");
    }
    // ... AND THE VERDICT IS ABOUT THE LEFT BUTTON, which is the press it was measured on: an application
    // that ignores a left click quite often opens its menu on a right one (measured on Steam and on WeChat --
    // the two are different questions, and the older spelling of this flag blocked both).
    if (entry->ignoresCallback && !rightClick) {
        return fail(TrayPressResult::NoEntry, L"its callback is known to be ignored");
    }

    HWND window = nullptr;
    if (!CallbackWindowFor(request, *entry, &window)) {
        return fail(TrayPressResult::NoWindow, L"its tray window is not there right now");
    }
    POINT anchor = {};
    if (!GetCursorPos(&anchor)) return fail(TrayPressResult::Refused, L"the pointer position could not be read");

    const WindowWatch watch = TakeWindowWatch(request);
    const HWND foregroundBefore = GetForegroundWindow();

    // ---- the right to take the foreground, granted BEFORE the message is sent --------------------------
    //
    // The rule a posted callback runs into (HANDOFF 8.105): an application restoring its own window from a
    // background callback is refused the foreground. The click that began this press landed on OUR bar, so
    // our window is the foreground window right now, and that is the documented condition under which
    // AllowSetForegroundWindow works. It grants the right; an application that does not use it gets nothing
    // from it, which is why the answer is still watched for and why BringApplicationForward exists.
    //
    // AND IT IS GRANTED FOR A RIGHT PRESS TOO (2026-10-16). This was left-click-only, with the reasoning that
    // "a right press's business is the application's own menu, so it need not move the foreground". That
    // reasoning was WRONG, and the user sent the canonical handler that shows why:
    //
    //      case WM_TRAYICON:
    //          if (lParam == WM_RBUTTONUP) {
    //              GetCursorPos(&pt);
    //              SetForegroundWindow(hWnd);            // "so the menu does not fail to close itself"
    //              TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
    //          }
    //
    // An application calls SetForegroundWindow on ITS OWN window before it shows its menu, and a modern
    // flyout (WinUI) dismisses itself when its window cannot come forward. The foreground at that moment is
    // this bar -- the click that began the press landed on the card -- so refusing the grant means the
    // application's handler runs, is refused the foreground, and raises nothing. That is exactly what was
    // measured on this machine as "应用菜单 does nothing": the window procedure returned in microseconds and
    // no menu appeared, while its own LEFT click (which was granted the foreground all along) worked.
    DWORD targetPid = 0;
    GetWindowThreadProcessId(window, &targetPid);
    bool foregroundGranted = false;
    if (targetPid) foregroundGranted = AllowSetForegroundWindow(targetPid) != FALSE;

    // ---- WHICH SPELLING, AND HOW IT IS DELIVERED ---------------------------------------------------------
    //
    // THE LAYOUT IS THE APPLICATION'S, and the entry's version says which one it is (NOTIFYICONDATA.
    // uCallbackMessage; the note at the top of this file records the two wrong guesses that cost a round):
    //
    //   v4  the anchor point is in wParam (low word x, high word y), and lParam carries the notification in
    //       its low word and the icon's uid in its high word;
    //   v3  wParam is the uid and lParam is the raw mouse message.
    //
    // AND THE DELIVERY IS PART OF THE SPELLING, which is the half that was measured rather than reasoned.
    // Watching from INSIDE clash-verge -- our own module is pinned in it, see the observation block in
    // TrayHook.cpp -- what the shell sends when that tray icon is really pressed is TWO MESSAGES, SENT and
    // not posted:
    //
    //     msg 0x1772 wparam 0x00000002 lparam 0x00000201   WM_LBUTTONDOWN
    //     msg 0x1772 wparam 0x00000002 lparam 0x00000202   WM_LBUTTONUP
    //
    // Posted, those same values did nothing in eight different shapes (both packings, with and without the
    // DOWN, up only, double click) -- and the application's window was MINIMISED for those tests, so an
    // effect would have been unmistakable. Sent, the pair brought the window back and to the front with the
    // pointer untouched. A left press is therefore a SEQUENCE, not a message, and it must be synchronous.
    //
    // A v4 application is the same problem in a different shape: the documented notification for a single
    // left click is NIN_SELECT, and no v4 tray icon was running on this machine while the above was
    // measured. Rather than guess, a press tries the documented spelling first and the measured pair second,
    // and whichever produced an answer is written into the cache (`select` / `downup`, the seventh field),
    // after which it is a single delivery again. The first wait is short because an application that reacts
    // at all does so in about a tenth of that (measured: 78-93 ms for a menu), and a press nobody answered
    // has had no effect -- so trying the other spelling cannot turn a toggle into two.
    struct Attempt {
        unsigned notify = 0;      // the notification word, or the raw mouse message
        bool downFirst = false;   // send the matching mouse DOWN first (the shell's own sequence)
        bool sent = false;        // SendMessageTimeout rather than PostMessage
        bool v4Packing = false;   // anchor in wParam, notification+uid in lParam; false: uid / mouse message
    };
    Attempt attempts[3] = {};
    int attemptCount = 0;
    const bool v4 = entry->version >= 4;
    island::TrayRightShape rightShape = island::TrayRightShape::None;
    island::TrayRightNote rightNote = island::TrayRightNote::Untried;
    if (rightClick) {
        // ONE SHAPE PER PRESS, CHOSEN FROM WHAT THIS ICON HAS ALREADY SHOWN -- see PlanTrayRightShape. For a
        // version 0/3 application the order is: the pair the SHELL sends (HANDOFF 8.142 caught it doing exactly
        // that on a real right click), then -- on a LATER press, and only if the pair did nothing -- the
        // context-menu notification (review round 44). Then this bar's own menu, and no third ask.
        rightNote = ReadRightNote(request);
        rightShape = island::PlanTrayRightShape(entry->version, rightNote);
        if (rightShape == island::TrayRightShape::None) {
            // BOTH SHAPES HAVE BEEN TRIED AND NEITHER BROUGHT UP ITS MENU: reported, not retried. The cell
            // answers with this bar's own menu (see the caller), which is the honest answer -- an application
            // that ignores two standard notifications is not going to answer a third guess.
            if (outcome) {
                outcome->result = TrayPressResult::NoEntry;
                outcome->route = L"its own menu has answered neither shape (the pair, then the notification)";
            }
            LogAndPrint(L"  tray press: '%ls' has answered neither the shell's pair nor the context-menu "
                        L"notification -- nothing else is sent\n", request.name);
            std::fflush(stdout);
            return false;
        }
        const island::TrayClickPlan plan =
            island::PlanTrayClick(entry->version, entry->uid, true, anchor.x, anchor.y);
        Attempt only = {};
        if (rightShape == island::TrayRightShape::DownUpPair) {
            // The event is READ BACK out of the message the plan built rather than written a second time here:
            // a second place that decides the spelling is how the v4 note came to be written down backwards.
            // For the pair it is the UP -- the message that carries the intent.
            only.v4Packing = false;
            only.notify = plan.hasSecond ? plan.second.lParam : plan.first.lParam;
            only.downFirst = plan.hasSecond;
            only.sent = true;
        } else {
            // THE CONTEXT-MENU NOTIFICATION. For a version-4 application that is the packed notification the
            // plan describes, POSTED (measured: clash-verge 78 ms, WeChat -- HANDOFF 8.104); for a version 0/3
            // one it is the same notification with the uid spelling, and it is posted too: it is a
            // notification rather than a mouse state, and a posted message cannot leave this press waiting on
            // an application that runs its menu loop inside the handler.
            only.v4Packing = plan.version4;
            only.notify = plan.version4 ? island::TrayLow(plan.first.lParam) : island::kTrayMouseContextMenu;
            only.downFirst = false;
            only.sent = plan.version4 ? plan.sent : false;
        }
        attempts[attemptCount++] = only;
    } else if (entry->leftSpelling == TrayCallback::LeftSpelling::Select) {
        attempts[attemptCount++] = Attempt{NIN_SELECT, false, true, true};
    } else if (entry->leftSpelling == TrayCallback::LeftSpelling::DownThenUp4) {
        attempts[attemptCount++] = Attempt{WM_LBUTTONUP, true, true, true};
    } else if (entry->leftSpelling == TrayCallback::LeftSpelling::DownThenUp3) {
        attempts[attemptCount++] = Attempt{WM_LBUTTONUP, true, true, false};
    } else if (entry->version == 0) {
        // "NEVER SAID" IS NOT "VERSION 3", and the press cannot tell the two apart from the version field
        // alone: uVersion shares its storage with uTimeout in NOTIFYICONDATA (see TrayLearn.cpp, and the
        // classic API reference it cites), so an application that IS version 4 can perfectly well have
        // registered with a 0 in that slot. So an unknown version tries the pair the SHELL itself sends --
        // measured working, and the recipe two of two applications on this machine answered -- and then the
        // documented v4 spelling. Trying only one of them is what would write a false "this application
        // ignores its callback" into the cache and cost that cell its invisible press for good.
        attempts[attemptCount++] = Attempt{WM_LBUTTONUP, true, true, false};
        attempts[attemptCount++] = Attempt{NIN_SELECT, false, true, true};
    } else if (v4) {
        attempts[attemptCount++] = Attempt{NIN_SELECT, false, true, true};
        attempts[attemptCount++] = Attempt{WM_LBUTTONUP, true, true, false};
    } else {
        attempts[attemptCount++] = Attempt{WM_LBUTTONUP, true, true, false};
    }

    bool posted = false;
    bool answered = false;
    int answeredWith = -1;
    for (int i = 0; i < attemptCount; ++i) {
        const Attempt& attempt = attempts[i];
        // THE TWO WORDS COME FROM domain/TrayEncoding.h AND NOWHERE ELSE (2026-10-16). The spelling that was
        // written here by hand was right, and the probe written later got it backwards -- which crashed
        // WeChat twice and was read as a "modal menu loop" for a round (HANDOFF 8.142). A press may choose
        // WHICH spelling to try; where the coordinates, the event and the uid go is not a per-file decision.
        const island::TrayMessage packed =
            island::EncodeTrayMessage(attempt.v4Packing, entry->uid, attempt.notify, anchor.x, anchor.y);
        if (attempt.downFirst) {
            const unsigned down = (attempt.notify == WM_RBUTTONUP) ? WM_RBUTTONDOWN : WM_LBUTTONDOWN;
            const island::TrayMessage packedDown =
                island::EncodeTrayMessage(attempt.v4Packing, entry->uid, down, anchor.x, anchor.y);
            const ULONGLONG downSent = GetTickCount64();
            posted = DeliverMessage(window, entry->message, static_cast<WPARAM>(packedDown.wParam),
                                    static_cast<LPARAM>(packedDown.lParam), attempt.sent) != FALSE;
            if (outcome) {
                const long long busy = MillisSince(downSent);
                if (busy > outcome->busyMs) outcome->busyMs = busy;
            }
            if (!posted) continue;
            Sleep(30);
        }
        // HOW LONG THE APPLICATION'S WINDOW PROCEDURE HELD THIS MESSAGE, which is the one number that tells
        // "the system dropped it" (microseconds) from "the application read it and did nothing" (tens of
        // milliseconds) -- the difference between a UIPI story and an application that ignores its own
        // callback. Measured per attempt, and reported in the one line a right press writes (see below).
        const ULONGLONG eventSent = GetTickCount64();
        posted = DeliverMessage(window, entry->message, static_cast<WPARAM>(packed.wParam),
                                static_cast<LPARAM>(packed.lParam), attempt.sent) != FALSE;
        if (outcome) {
            const long long busy = MillisSince(eventSent);
            if (busy > outcome->busyMs) outcome->busyMs = busy;
        }
        if (!posted) continue;
        answered = WatchApplicationForAnswer(watch, foregroundBefore,
                                             outcome ? outcome->answer : nullptr,
                                             outcome ? sizeof(outcome->answer) / sizeof(wchar_t) : 0,
                                             (i + 1 < attemptCount) ? 400 : 1500);
        if (answered) {
            answeredWith = i;
            break;
        }
    }
    if (!posted) return fail(TrayPressResult::Refused, L"the message could not be delivered");

    // ... AND "IT IS ALREADY IN FRONT" IS AN ANSWER, for a left press.
    //
    // That is what a left press is FOR -- the application ends up in front of the user -- so a press on an
    // application that was in the foreground already has nothing left to make observable, and the two
    // observers above (a window that came up, the foreground moving) are both blind to it by construction.
    // Measured on clash-verge: four presses in a row were reported as "nothing of its own appeared, and the
    // foreground did not move to it" while its window had simply been in front the whole time -- and the
    // verdict written for that state is `click`, which takes the invisible route away from an application
    // whose tray icon is working exactly as it should.
    if (!answered && !rightClick) {
        const HWND front = GetForegroundWindow();
        DWORD holder = 0;
        if (front) GetWindowThreadProcessId(front, &holder);
        if (holder && std::find(watch.pids.begin(), watch.pids.end(), holder) != watch.pids.end()) {
            answered = true;
            if (outcome) {
                wcsncpy_s(outcome->answer, sizeof(outcome->answer) / sizeof(wchar_t),
                          L"its own window was in front before the press and still is", _TRUNCATE);
            }
        }
    }

    wchar_t target[160] = {};
    wchar_t cls[128] = {};
    GetClassNameW(window, cls, 128);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    // "given its own callback", not "posted": since 8.109 a left press is two SENT messages and the right one
    // is posted, and a log line that names the wrong mechanism is the kind of thing this project has paid for
    // more than once.
    std::swprintf(target, 160, L"class '%ls' of pid %lu (given its own callback, not a click)", cls, pid);

    // THE SPELLING THAT WORKED IS REMEMBERED, so the next press of this cell is one delivery instead of two.
    if (answered && !rightClick && attemptCount > 1 && answeredWith >= 0) {
        MarkCallbackLeftSpelling(request, attempts[answeredWith].v4Packing
                                           ? (attempts[answeredWith].notify == NIN_SELECT
                                                  ? TrayCallback::LeftSpelling::Select
                                                  : TrayCallback::LeftSpelling::DownThenUp4)
                                           : TrayCallback::LeftSpelling::DownThenUp3);
    }

    // ---- and the left click's other half: is the application actually IN FRONT now? -------------------
    //
    // Asked rather than assumed, because "its window is on screen somewhere" and "the user can see it" are
    // different facts. `foregroundNow` is checked by pid: any of the application's processes counts, since
    // the window that answers a tray click is often not the one that owns the icon.
    if (!rightClick && targetPid && answered) {
        const HWND foregroundNow = GetForegroundWindow();
        DWORD holder = 0;
        if (foregroundNow) GetWindowThreadProcessId(foregroundNow, &holder);
        const bool inFront = holder &&
                             std::find(watch.pids.begin(), watch.pids.end(), holder) != watch.pids.end();
        if (!inFront) {
            wchar_t brought[128] = {};
            const bool forward = BringApplicationForward(watch, brought, 128);
            LogAndPrint(L"  tray press: gave 0x%04X to %ls, and the application is not in front "
                        L"(foreground permission %ls) -- %ls\n",
                        entry->message, target, foregroundGranted ? L"was granted" : L"could not be granted",
                        forward ? brought : L"it could not be brought forward");
            if (forward && outcome) {
                wcsncpy_s(outcome->answer, sizeof(outcome->answer) / sizeof(wchar_t), brought, _TRUNCATE);
            }
        }
    }

    // NOT ANSWERED MEANS THE CELL SAYS SO FROM NOW ON -- BUT ONLY FOR A LEFT PRESS.
    //
    // THE VERDICT IS ABOUT THE LEFT BUTTON, AND IT IS PERMANENT: it is what makes the press refuse to send
    // anything at all the next time (the check above, and the one in SendTrayCallback). Reading a failed
    // RIGHT press into it therefore takes the working half of an application away for good, and that is
    // exactly what happened on this machine on 2026-10-15: a right press on ChatGPT.exe got no answer (the
    // application opens no menu from our posted message), wrote 'none' into the cache for it, and from then
    // on a LEFT press on that same icon would have been refused -- while an hour earlier the same left press
    // measured 2094 ms and worked. The two buttons are not the same question and an answer about one is not
    // an answer about the other.
    if (!answered) {
        LogAndPrint(L"  tray press: gave 0x%04X to %ls and the application did nothing with it (%ls) -- %ls\n",
                    entry->message, target, outcome ? outcome->answer : L"no answer",
                    rightClick ? L"nothing is recorded about the left press from this: the two buttons are "
                                 L"different questions"
                               : L"recorded as 'none' for that cell");
        if (!rightClick) MarkCallbackAsIgnored(request);
        if (outcome) {
            outcome->result = TrayPressResult::NoEntry;
            outcome->route = rightClick ? L"its own menu did not come up" : L"its callback is ignored (recorded)";
            wcsncpy_s(outcome->target, sizeof(outcome->target) / sizeof(wchar_t), target, _TRUNCATE);
        }
        // ON THE RIGHT BUTTON, ONE LINE SAYS THE WHOLE STORY: the shape that was sent, that it was sent ONCE,
        // and that the application answered with nothing. `own_menu` is the caller's answer to this (the card
        // opens THIS BAR's menu over that cell), and it is logged there rather than here because this function
        // does not own the card.
        if (rightClick) {
            // THE NEXT SHAPE IS WRITTEN DOWN, so the following press of this cell is one message (the other
            // candidate) instead of the same one again -- and when both have failed, that is written down too
            // and no press of this icon asks a third time. Silent unless it changes.
            const island::TrayRightNote next = island::NextTrayRightNote(rightNote, rightShape, false);
            if (next != rightNote) WriteRightNote(request, next);
            LogAndPrint(L"  tray context-menu: app=%ls version=%u strategy=%ls attempts=%d busy_ms=%lld "
                        L"outcome=failed evidence='%ls' elapsed_ms=%lld -- next press of this cell: %ls\n",
                        BaseNameLower(request.exePath).c_str(), entry->version,
                        island::TrayRightShapeName(rightShape), attemptCount,
                        outcome ? outcome->busyMs : 0, outcome ? outcome->answer : L"no answer",
                        MillisSince(rightPressStart), island::TrayRightNoteName(next));
            std::fflush(stdout);
        }
        return false;
    }

    if (outcome) {
        outcome->route = L"its own callback";
        outcome->result = TrayPressResult::Pressed;
        outcome->appAnswered = answered;
        wcsncpy_s(outcome->target, sizeof(outcome->target) / sizeof(wchar_t), target, _TRUNCATE);
    }
    LogAndPrint(L"  tray press: gave 0x%04X to %ls, the pointer was NOT moved (anchor %ld,%ld) -- the "
                L"application %ls\n",
                entry->message, target, anchor.x, anchor.y,
                answered ? L"answered" : L"has not been seen to answer yet, and nothing else was done about "
                                       L"it (pressing twice would be worse)");
    // ... and the same one line for the right press that DID bring up the application's own menu. `evidence`
    // is the window or menu the observer caught appearing, because a right press that merely moves the
    // foreground is not an answer to "show me your menu".
    if (rightClick) {
        // The shape that worked is written down as this icon's shape: the next press sends it directly instead
        // of starting from the other candidate again. Silent unless it changes.
        const island::TrayRightNote next = island::NextTrayRightNote(rightNote, rightShape, true);
        if (next != rightNote) WriteRightNote(request, next);
        LogAndPrint(L"  tray context-menu: app=%ls version=%u strategy=%ls attempts=%d busy_ms=%lld "
                    L"outcome=native_menu evidence='%ls' elapsed_ms=%lld\n",
                    BaseNameLower(request.exePath).c_str(), entry->version,
                    island::TrayRightShapeName(rightShape), attemptCount, outcome ? outcome->busyMs : 0,
                    outcome ? outcome->answer : L"answered", MillisSince(rightPressStart));
        std::fflush(stdout);
    }
    return true;
}

bool TrayRightClickExhausted(const TrayPressRequest& request) {
    return ReadRightNote(request) == TrayRightNote::BothFailed;
}

bool TrayPressReachable(const TrayPressRequest& request) {
    const std::vector<TrayCallback> entries = ReadTrayCallbacks();
    const std::wstring exe = BaseNameLower(request.exePath);
    wchar_t wanted[64] = {};
    if (request.hasGuid) StringFromGUID2(request.guid, wanted, 64);
    for (const TrayCallback& candidate : entries) {
        if (exe != candidate.exe) continue;
        if (request.hasGuid) {   // see SendTrayCallback: the GUID is the stronger identity
            if (_wcsicmp(candidate.guid, wanted) != 0) continue;
        } else if (request.hasUid) {
            if (candidate.uid != request.uid || candidate.guid[0]) continue;
        } else {
            continue;
        }
        if (candidate.ignoresCallback) return false;
        HWND window = nullptr;
        return CallbackWindowFor(request, candidate, &window);
    }
    return false;
}

const wchar_t* TrayPressResultName(TrayPressResult result) {
    switch (result) {
        case TrayPressResult::Pressed: return L"pressed";
        case TrayPressResult::NoEntry: return L"nothing is known about this icon yet";
        case TrayPressResult::NoWindow: return L"that application has no such window right now";
        case TrayPressResult::Refused: return L"the message could not be delivered";
    }
    return L"?";
}

// --- reading a name out of a report line ------------------------------------------------------------
//
// The three helpers below are the reader's, not the writer's: they pull one ` key value ` token, one number and
// one quoted value out of a line of tray_learned.txt. Small and local on purpose -- the file's other reader
// (TrayLearn.cpp) has its own, and the two are different jobs: that one turns lines into cache entries, this
// one answers "what does the shell call this icon".
bool ReportField(const wchar_t* line, const wchar_t* key, wchar_t* out, size_t count) {
    wchar_t needle[32] = {};
    _snwprintf_s(needle, 32, _TRUNCATE, L" %ls ", key);
    const wchar_t* found = wcsstr(line, needle);
    if (!found) return false;
    found += wcslen(needle);
    size_t i = 0;
    while (found[i] && found[i] != L' ' && found[i] != L'"' && i + 1 < count) {
        out[i] = found[i];
        ++i;
    }
    out[i] = 0;
    return i > 0;
}

bool ReportFieldNumber(const wchar_t* line, const wchar_t* key, unsigned* out) {
    wchar_t text[32] = {};
    if (!ReportField(line, key, text, 32)) return false;
    wchar_t* end = nullptr;
    *out = static_cast<unsigned>(wcstoul(text, &end, 10));
    return end != text;
}

bool ReportFieldQuoted(const wchar_t* line, const wchar_t* key, wchar_t* out, size_t count) {
    wchar_t needle[32] = {};
    _snwprintf_s(needle, 32, _TRUNCATE, L" %ls \"", key);
    const wchar_t* found = wcsstr(line, needle);
    if (!found) return false;
    found += wcslen(needle);
    size_t i = 0;
    while (found[i] && found[i] != L'"' && i + 1 < count) {
        out[i] = found[i];
        ++i;
    }
    out[i] = 0;
    return i > 0;
}

// THE NAME THE SHELL ITSELF USES FOR THIS ICON, out of the registrations the module has reported.
//
// The two names the bar has for a cell are usually the same string and are not always. A GUID-registered icon
// -- the security centre, Explorer's own network and volume buttons -- has an EMPTY tooltip in the shell's
// registry record, so the card falls back to the executable's name ("SecurityHealthSystray.exe") while the
// shell's element is named "Windows 安全中心 - 建议的操作。". The shell told us that name in the registration it
// sent when the icon appeared, and the report file holds it: this is a read of that file, by the same
// (exe, uid/guid) identity the cache is keyed by, for the one case where a press needs a name and the card has
// the wrong one. The file is small and a press is a rare event, so the scan is free.
std::wstring RecordedShellTipFor(const TrayPressRequest& request) {
    wchar_t home[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH) == 0) return std::wstring();
    const std::wstring report = std::wstring(home) + L"\\IslandBar\\tray_learned.txt";
    FILE* stream = nullptr;
    if (_wfopen_s(&stream, report.c_str(), L"r, ccs=UTF-8") != 0 || !stream) return std::wstring();

    const std::wstring exe = BaseNameLower(request.exePath);
    std::wstring tip;
    wchar_t line[1024] = {};
    while (fgetws(line, 1024, stream)) {
        if (!wcsstr(line, L"call S NIM_ADD") && !wcsstr(line, L"call S NIM_MODIFY")) continue;
        wchar_t lineExe[260] = {};
        if (!ReportField(line, L"exe", lineExe, 260) || BaseNameLower(lineExe) != exe) continue;
        // The identity: the guid when the icon is named by one, the uid otherwise -- the same rule the cache
        // uses, so a cell and its record cannot be paired up two different ways.
        wchar_t lineGuid[64] = {};
        const bool haveLineGuid = ReportField(line, L"guid", lineGuid, 64) && lineGuid[0] != L'-';
        if (request.hasGuid || haveLineGuid) {
            if (!request.hasGuid) continue;
            GUID guid = request.guid;
            wchar_t wanted[64] = {};
            _snwprintf_s(wanted, 64, _TRUNCATE,
                         L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                         static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3, guid.Data4[0],
                         guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5],
                         guid.Data4[6], guid.Data4[7]);
            if (!haveLineGuid || _wcsicmp(lineGuid, wanted) != 0) continue;
        } else {
            unsigned uid = 0;
            if (!ReportFieldNumber(line, L"uid", &uid) || uid != request.uid) continue;
        }
        wchar_t lineTip[256] = {};
        if (ReportFieldQuoted(line, L"tip", lineTip, 256) && lineTip[0]) {
            tip = lineTip;
            break;
        }
    }
    fclose(stream);
    return tip;
}

// Presses one icon. The default route is the application's own callback message, sent to its own tray window
// (see the top of TrayPress.h, and SendTrayCallback for what the message looks like).
//
// Everything else this function used to do -- borrow the shell's taskbar invisibly, open its overflow flyout,
// ask for the icon's rectangle, find the automation button on it, put the pointer on it, click, put the pointer
// back, and hand the shell's panel to a watchdog -- is gone. It was one route with eight failure modes and a
// visible cost the user refused to live with, and it existed because the message route was not yet understood.
TrayPressOutcome PressTrayIcon(const TrayPressRequest& request, bool rightClick) {
    TrayPressOutcome outcome;
    const ULONGLONG pressStart = GetTickCount64();
    if (!SendTrayCallback(request, rightClick, &outcome)) {
        // NOTHING WAS KNOWN ABOUT THIS ICON, or the window it registered is not there any more. The caller
        // answers that cell with the bar's OWN menu -- see the card's click handler -- so the user always gets
        // something deliberate instead of a button that does nothing.
        if (outcome.result == TrayPressResult::Pressed) outcome.result = TrayPressResult::NoEntry;
    }

    // ... AND ONE MORE ROUTE, FOR THE CELLS THAT WILL NEVER BE LEARNABLE.
    //
    // Some icons cannot be pressed by sending anything: nothing ever registers them (NVIDIA's, whose icons were
    // created before this bar existed), they are not applications at all (the shell's own network, volume and
    // clock buttons), or they refuse to be entered (AppContainer and elevated applications). For those the
    // honest answer is to ask the SHELL to press them, which is what the real tray does -- see shell/TrayUia.h.
    //
    // IT IS A FALLBACK, NOT THE ROAD. It costs something visible: the shell's taskbar has to exist for the
    // length of the press, because UIA cannot see a tray that is not there. It is therefore disabled by
    // default, and an unlearned cell opens this bar's own menu without ever showing the native taskbar.
    // ... AND IT IS TRIED EVEN WHEN THE VERDICT SAYS THE CALLBACK IS IGNORED, which was the first version's
    // mistake: "this application ignores its own callback" is a fact about the MESSAGE route, and this is a
    // different one -- the shell pressing the icon itself. Measured: Steam's left press has been recorded as
    // ignored for two rounds, and through UIA it answers in 453 ms.
    if (ShouldTryTrayUiaFallback(outcome.result, rightClick, TrayUiaFallbackEnabled())) {
        // TWO NAMES, because the card's is sometimes the executable: see RecordedShellTipFor.
        const std::wstring recordedTip = RecordedShellTipFor(request);
        const TrayUiaOutcome uia =
            PressTrayIconWithUia(request.name, recordedTip.empty() ? nullptr : recordedTip.c_str());
        if (uia.pressed) {
            outcome.result = TrayPressResult::Pressed;
            outcome.route = L"the shell's own tray (UI Automation)";
            if (outcome.answer[0] == L'\0') {
                wcsncpy_s(outcome.answer, uia.detail, _TRUNCATE);
            }
        } else {
            // Reported rather than swallowed: "the icon was not in the shell's tree" and "the shell refused to
            // invoke it" are different answers, and the log is the only place either one is visible.
            LogAndPrint(L"  tray press: the UI Automation fallback did not press '%ls' -- %ls\n",
                        request.name, uia.detail);
            std::fflush(stdout);
        }
    } else if (outcome.result == TrayPressResult::NoEntry && !rightClick) {
        LogAndPrint(L"  tray press: '%ls' fallback=bar-menu native-taskbar-show=0 reason=uia-disabled\n",
                    request.name);
        std::fflush(stdout);
    }

    outcome.ms = MillisSince(pressStart);
    return outcome;
}

} // namespace island
