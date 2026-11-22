// =====================================================================
//  TrayHook.cpp -- INSIDE THE APPLICATION, WHEN IT TELLS THE SHELL ABOUT ITS TRAY ICON
//
//  WHAT THIS IS FOR. Pressing a tray cell without moving the pointer means POSTING the application's own
//  callback message to its own tray window, and that needs three things about the icon: the window, the
//  message number and the uid. Until now the message number was learned by hand, one application at a time,
//  with a Win32k trace and a third-party program in the room (HANDOFF 8.104) -- which the user called out,
//  rightly, as a chore rather than a feature.
//
//  The way out is to be present when the application REGISTERS its icon, because the structure it passes to
//  the documented API carries every field needed. This module is that presence: the bar loads it into each
//  running tray application with SetWindowsHookEx(WH_GETMESSAGE) on that application's threads, and once it
//  is inside, it points the application's OWN import slots for Shell_NotifyIconW/A at the detour below. The
//  application then writes down its own answer the next time it registers -- which the bar asks for with the
//  standard TaskbarCreated broadcast, the same signal the shell sends when Explorer restarts.
//
//  WHY IMPORT TABLES AND NOT CODE PATCHING. Overwriting the first bytes of the shell's function would catch
//  one more case -- an application that resolves the address itself and calls through its own pointer, which
//  is what WeChat's Qt tray does (measured: 0 import slots, so it is not caught here) -- but it means copying
//  an unknown number of instruction bytes and putting them back somewhere, in a process that is not ours. An
//  import slot is a pointer in a table; if this module is wrong about it, the call still reaches the real
//  function. That is the trade this file makes deliberately, and the missed case is written down in
//  HANDOFF 8.107 rather than hidden.
//
//  WHAT IT WRITES: %USERPROFILE%\IslandBar\tray_learned.txt, one line per call. The bar reads that file,
//  resolves each window's class and turns the lines into its callback cache.
//
//  ASCII-only on purpose. Do not add non-ASCII text to this file.
// =====================================================================
#include <windows.h>
#include <shellapi.h>   // NOTIFYICONDATA and friends: WIN32_LEAN_AND_MEAN leaves them out of windows.h
#include <delayimp.h>
#include <intrin.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

// The wire format of a registration, and the reason it is a header in domain/ rather than code here: this
// module runs inside other people's processes and cannot be exercised by the self-test, so the part of it that
// can be wrong -- the reading of the bytes -- is somewhere the self-test can reach. See the header for how the
// offsets were measured.
#include "domain/TrayRegistration.h"

// The channel back to the bar: a shared-memory ring, so that a registration costs a memcpy instead of a disk
// write inside an application's synchronous call to the shell. See platform/TrayRing.h for the argument.
#include "platform/TrayRing.h"

// Declared HERE, at file scope, rather than inside the anonymous namespace below: the pin needs an address
// that is inside this module, and pinning is what stops the loader from ever unloading it. Inside the
// namespace the definition below would be a second, differently linked function of the same name.
extern "C" __declspec(dllexport) LRESULT CALLBACK IslandTrayHookProc(int code, WPARAM wparam, LPARAM lparam);

namespace {

// The ring this module publishes on. One per process, opened on the first registration.
island::TrayRingPublisher g_ring;

// SRWLOCK has a constant initializer, so the report path needs no setup from DllMain. The critical section
// this replaced was created under the loader lock, and the first log line then performed file I/O there.
SRWLOCK g_lock = SRWLOCK_INIT;
volatile LONG g_state = 0;   // 0 = not tried yet
volatile LONG g_hooked = 0;  // how many import slots were redirected
void* g_realW = nullptr;
void* g_realA = nullptr;
void* g_realGetProcAddress = nullptr;

// Declared first: everything below uses them, and the detour of GetProcAddress hands out their addresses.
BOOL WINAPI DetourShellNotifyIconW(DWORD message, PNOTIFYICONDATAW data);
BOOL WINAPI DetourShellNotifyIconA(DWORD message, PNOTIFYICONDATAA data);
typedef FARPROC(WINAPI* GetProcAddress_t)(HMODULE, LPCSTR);
FARPROC WINAPI DetourGetProcAddress(HMODULE module, LPCSTR name);
// Defined further down, next to the in-application report that uses it; the shell-side report above needs it
// first.
const char* MessageName(DWORD message);

// THE APPLICATION'S OWN WAY OF FINDING Shell_NotifyIcon, which the import walk cannot see.
//
// Measured on this machine (HANDOFF 8.107): WeChat's Qt tray resolves Shell_NotifyIcon at run time and calls
// it through its own pointer -- its import tables hold NO slot for either spelling, so patching the import
// table covers WeChat, QQ Music's launcher and the rest of the frameworks that do it this way: nothing to
// patch. The application still has to ASK for the address, though, and that question goes through
// `GetProcAddress` -- which IS an import, in every module that needs it.
//
// So the walk redirects those slots too, and this answers the application with our detour when the name it
// asks for is one of the two. Everything else is forwarded unchanged, through a pointer captured before any
// slot was touched, so this cannot recurse and cannot change any other behaviour of the process.
bool IsShell32Handle(HMODULE module) {
    if (!module) return false;
    HMODULE shell = GetModuleHandleW(L"shell32.dll");
    return shell && module == shell;
}

FARPROC WINAPI DetourGetProcAddress(HMODULE module, LPCSTR name) {
    GetProcAddress_t real = reinterpret_cast<GetProcAddress_t>(g_realGetProcAddress);
    if (!real) return nullptr;
    if (name && IsShell32Handle(module)) {
        // Two string comparisons on a short literal, in a function called very often: uppercase first
        // character is enough to dismiss every other name in the process.
        if (name[0] == 'S' || name[0] == 's') {
            if (_stricmp(name, "Shell_NotifyIconW") == 0) {
                return reinterpret_cast<FARPROC>(&DetourShellNotifyIconW);
            }
            if (_stricmp(name, "Shell_NotifyIconA") == 0) {
                return reinterpret_cast<FARPROC>(&DetourShellNotifyIconA);
            }
        }
    }
    return real(module, name);
}

// Is this process the bar itself? The global hook reaches every process on the desktop, its own included, and
// the bar must not patch its own import tables or report itself as a tray application.
bool IsOurOwnProcess() {
    wchar_t self[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) return false;
    const wchar_t* leaf = wcsrchr(self, L'\\');
    return leaf && _wcsicmp(leaf + 1, L"island_bar.exe") == 0;
}

// A GUID as the registry and the cache file spell it: braces, upper-case hex. Written by hand rather than
// with StringFromGUID2 because that one lives in ole32, and this module deliberately links nothing but
// user32 and shell32 -- it is loaded into every GUI process on the desktop.
void GuidText(const GUID& guid, wchar_t* out, size_t count) {
    _snwprintf_s(out, count, _TRUNCATE, L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                 static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3, guid.Data4[0],
                 guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6],
                 guid.Data4[7]);
}

void ReportPath(wchar_t* out, size_t count) {
    wchar_t home[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH) == 0) {
        wcscpy_s(out, count, L"C:\\island_bar_tray_learned.txt");
        return;
    }
    _snwprintf_s(out, count, _TRUNCATE, L"%s\\IslandBar\\tray_learned.txt", home);
}

// One open handle per process, and the line written through it, instead of an open/write/close per line.
//
// WHY THIS IS NOT A MICRO-OPTIMISATION. The line this writes is written while an application is waiting:
// WM_COPYDATA is SYNCHRONOUS -- the sender is blocked until this process's message handling returns -- and
// that handling is where a registration is read (see ReportShellCopy). An open and a close per line puts
// three system calls, plus whatever a filter driver wants to do with them, in the middle of somebody else's
// registration. With the handle kept, the same line costs one WriteFile.
//
// It is still a synchronous write: a line that has been written is on the file before the application is let
// go, so nothing a reader can miss is left in this process's memory (which matters, because this module runs
// in a process that is not ours and may end at any moment).
HANDLE g_report = INVALID_HANDLE_VALUE;

HANDLE ReportHandle(const wchar_t* path) {
    if (g_report != INVALID_HANDLE_VALUE) return g_report;
    // Shared for writing, because the module can be mapped in more than one process at once and both of them
    // report to this same file.
    //
    // FILE_SHARE_DELETE AS WELL, and that one was missing until it was measured: the bar's learning pass
    // TRUNCATES this report before it offers the module (`DeleteFileW`, see TrayLearn.cpp) so that everything
    // in the file belongs to that pass. With a handle open that did not allow deletion, the delete failed
    // silently -- a running shell's copy of the module is holding it for the life of that process -- and the
    // file kept growing without the bar ever noticing. The sharing mode is the whole difference between "the
    // report is rotated" and "the report is append-only forever", and it costs nothing.
    g_report = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return g_report;
}

void ReportLine(const wchar_t* format, ...) {
    AcquireSRWLockExclusive(&g_lock);
    wchar_t path[MAX_PATH] = {};
    ReportPath(path, MAX_PATH);
    wchar_t line[2048] = {};
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    int used = _snwprintf_s(line, 2048, _TRUNCATE, L"[%02u:%02u:%02u.%03u] ", now.wHour, now.wMinute,
                            now.wSecond, now.wMilliseconds);
    if (used < 0) used = 0;
    va_list args;
    va_start(args, format);
    const int added = _vsnwprintf_s(line + used, 2048 - used, _TRUNCATE, format, args);
    va_end(args);
    if (added > 0) used += added;
    if (used > 2046) used = 2046;
    line[used++] = L'\n';

    // UTF-8 explicitly: the file is read by the bar's own reader, and a CRT locale would make that depend on
    // the codepage of whichever process happens to be reporting.
    char utf8[4096] = {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line, used, utf8, sizeof(utf8), nullptr, nullptr);
    const HANDLE handle = ReportHandle(path);
    if (handle != INVALID_HANDLE_VALUE && bytes > 0) {
        DWORD written = 0;
        WriteFile(handle, utf8, static_cast<DWORD>(bytes), &written, nullptr);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

// =====================================================================
//  TEMPORARY MEASUREMENT -- WHAT THE SHELL ACTUALLY SENDS TO THE TRAY WINDOW
//
//  DELETE THIS WHOLE BLOCK once the answer is written down in HANDOFF (and the marker file it needs).
//
//  The question it answers: pressing clash-verge's tray icon with a posted message does nothing, in EIGHT
//  shapes (v3 and v4 packing, WM_LBUTTONUP / NIN_SELECT / WM_LBUTTONDOWN+UP / WM_LBUTTONDBLCLK), while the
//  RIGHT button of the same icon opens its menu from a single post and a REAL click on the left does restore
//  its window. So the shell sends something this module does not know how to spell, and the only place that
//  can be seen is inside the application: this module is already there, pinned.
//
//  Two channels, because "sent" and "posted" are different Windows mechanisms and the project has already
//  measured that the shell's own notification does not appear in a message queue trace:
//
//    * WH_CALLWNDPROC, installed on this process's threads from inside      -> messages SENT to the window
//    * this module's existing WH_GETMESSAGE procedure (see below)           -> messages POSTED to it
//
//  Off unless the marker file exists, so it costs nothing and logs nothing in a normal session.
// =====================================================================
volatile LONG g_callbackMessage = 0;   // the callback message the application registered (from NIM_ADD)
void* volatile g_trayWindow = nullptr; // ... and the window it registered
bool g_watch = false;

// How many WH_CALLWNDPROC hooks this process will hold, and how many times the sent channel is retried. The
// cap on the walk is what it always was (96): measured, Explorer has 143 threads, so the walk alone can never
// promise that the tray thread is among them -- which is the whole reason the attempt below aims first.
constexpr int kMaxSentHooks = 96;
constexpr LONG kMaxSentAttempts = 3;
HHOOK g_sentHooks[kMaxSentHooks] = {};
int g_sentHookCount = 0;

// THE SENT CHANNEL'S STATE. `attempted`/`settled`/`inFlight` are separate from the report structure below
// because the hook procedure runs on EVERY hooked thread of this process and two of them can reach this at
// once; the numbers themselves are only written while an attempt is in flight and read afterwards.
volatile LONG g_sentAttempted = 0;   // EnsureHooked's full attempt has run
volatile LONG g_sentInFlight = 0;    // ... and an attempt is running right now
volatile LONG g_sentSettled = 0;     // ready, or out of attempts: stop trying
volatile LONG g_sentAttempts = 0;

bool MarkerPresent() {
    wchar_t home[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH) == 0) return false;
    wchar_t path[MAX_PATH] = {};
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\IslandBar\\tray_watch.txt", home);
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

void ReportObserved(const wchar_t* channel, HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    wchar_t cls[96] = {};
    if (window) GetClassNameW(window, cls, 96);
    ReportLine(L"observed %ls pid %lu hwnd %p class \"%ls\" msg 0x%04X wparam 0x%08X lparam 0x%08X",
               channel, GetCurrentProcessId(), window, cls, message, static_cast<unsigned>(wparam),
               static_cast<unsigned>(lparam));
}

// Everything at or above WM_USER is the application's own business: a window procedure is sent no end of
// WM_* bookkeeping, and only the private range can be the tray callback.
bool WorthObserving(UINT message) {
    return message >= WM_USER || message == (UINT)g_callbackMessage || message == WM_COPYDATA;
}

// =====================================================================
//  THE SHELL'S OWN COPY OF A REGISTRATION -- HOW AN ICON IS LEARNED WITH NOBODY IN THE APPLICATION
// =====================================================================
//
//  An application tells the shell about its tray icon by SENDING a WM_COPYDATA to the window of class
//  "Shell_TrayWnd" (measured on this machine 2026-10-15: receiver class "Shell_TrayWnd" inside explorer, the
//  sender window equal to the tray window the payload names, dwData 1, cbData 1484). Because this module is
//  already mapped inside the shell process -- the bar offers it to Explorer for exactly this reason -- the
//  WH_CALLWNDPROC channel above sees that copy arrive, and the payload is marshalled into THIS process, so
//  every field of the registration can simply be read. Nothing has to be injected into the application.
//
//  That is the answer to the limitation this project kept hitting: an AppContainer application refuses the
//  hook (measured: SetWindowsHookEx returns 87 for ChatGPT.exe), and a self-protecting one accepts the hooks
//  and never loads the module (QQ Music). Their registrations still travel to the shell, and this is where
//  they are read. Measured in the first run of this code: ChatGPT.exe (AppContainer) registered uid 3,
//  message 0x8001, tip "ChatGPT", guid E5768D8B-6936-4F45-B1AD-4C5FB414CB35.
//
//  THE LAYOUT, read off the bytes rather than assumed. A controlled registration (probe-tray-registrar.exe:
//  uid 7, message 0xABCD, tip "ISLANDBAR-PROBE-4711", NIM_SETVERSION 4) and a cross-check against
//  applications whose answers the bar already had (Steam's copy names uid 1 with message 0x0401 and the tip
//  "Steam"; the GUID-registered icons carry exactly the guids the shell's own registry entries name) give:
//
//      +0x000  the shell's tag, 0x34753423, in every copy
//      +0x004  the NIM_ command: 0 ADD, 1 MODIFY, 2 DELETE, 4 SETVERSION
//      +0x008  956, in every copy
//      +0x00C  hWnd   -- the application's tray window (4 bytes; handles fit, so the shell stores one DWORD)
//      +0x010  uID
//      +0x014  uFlags  (0x20 is NIF_GUID: the icon is identified by guid, not by uid)
//      +0x018  uCallbackMessage
//      +0x01C  hIcon
//      +0x020  szTip, wide, 128 characters
//      +0x328  uVersion -- non-zero only in the NIM_SETVERSION copy (the union with uTimeout, so a
//              registration that never says reads 0 -- see HANDOFF 8.111)
//      +0x3B0  guidItem, 16 raw bytes
//
//  Every offset is eight bytes earlier than the documented NOTIFYICONDATAW, which is that structure without
//  its leading cbSize and with pointers stored as DWORDs. That is why the two lowest offsets were found by
//  searching for the values a controlled registration had written, not by adding up a header.
// =====================================================================
const wchar_t* OwnerLeaf(HWND window, wchar_t* out, size_t count) {
    out[0] = L'\0';
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (!pid) return out;
    // QUERY_LIMITED_INFORMATION, because the answer is wanted for a process that may be in a container or at
    // a different integrity level, and that is the strongest right a process is given over one it does not own.
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return out;
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        const wchar_t* leaf = wcsrchr(path, L'\\');
        wcsncpy_s(out, count, leaf ? leaf + 1 : path, _TRUNCATE);
    }
    CloseHandle(process);
    return out;
}

// A COPY THAT DID NOT DECODE, SAID OUT LOUD (2026-10-16).
//
// This used to be a silent return, and that silence cost a round of measuring: the channel was installed on
// the right thread, it was CALLED, it saw 23 WM_COPYDATA messages arrive at Shell_TrayWnd -- and the bar
// counted 0 registrations, with nothing anywhere saying why. Three lines per process is enough: what the
// shell sent (dwData), how big it was (cbData), and what its first bytes said.
void ReportUndecodedCopy(const COPYDATASTRUCT* data) {
    static volatile LONG explained = 0;
    if (!data || InterlockedIncrement(&explained) > 3) return;
    unsigned tag = 0;
    unsigned command = 0xFFFFFFFFu;
    if (data->lpData && data->cbData >= sizeof(tag)) {
        std::memcpy(&tag, data->lpData, sizeof(tag));
        if (data->cbData >= 8) {
            std::memcpy(&command, static_cast<const unsigned char*>(data->lpData) + 4, sizeof(command));
        }
    }
    ReportLine(L"call S ? pid %lu: a WM_COPYDATA did not decode -- dwData %lu, cbData %lu, tag 0x%08X, "
               L"command %u (the tag has to be 0x%08X and the copy at least %llu bytes)",
               GetCurrentProcessId(), data->dwData, data->cbData, tag, command, island::kTrayCopyTag,
               static_cast<unsigned long long>(island::kTrayCopyMinimum));
}

// One WM_COPYDATA, decoded if it is a tray registration. Writes the same line the in-application path writes
// (see LogCall), with "S" in place of "W"/"A" so the reader can tell where it was read, and with pid instead
// of exe when the owning process refuses to name its image.
void ReportShellCopy(const wchar_t* channel, HWND receiver, WPARAM wparam, LPARAM lparam) {
    if (!lparam) return;
    const COPYDATASTRUCT* data = reinterpret_cast<const COPYDATASTRUCT*>(lparam);
    if (!data || !data->lpData) return;

    // THE WIRE FORMAT IS NOT DECODED HERE. It lives in domain/TrayRegistration.h, with no Windows in it, so the
    // self-test can check the decoding against the bytes of a real registration on a machine with no shell,
    // no tray and no application running -- which is the only way a change to these offsets can be caught
    // before it reaches somebody's desktop.
    const island::TrayRegistration registration =
        island::DecodeTrayRegistration(data->lpData, data->cbData);
    if (!registration.valid) {
        ReportUndecodedCopy(data);
        return;
    }

    // ... AND IT GOES OUT OVER THE RING, not into a file.
    //
    // This function runs INSIDE the shell's message handling, with the registering application waiting for it
    // to return (WM_COPYDATA is synchronous). Everything that used to happen here -- resolving the window's
    // owner, formatting a line, opening and writing and closing a file beside the bar -- was work the
    // application paid for. What is left is a memcpy into a shared ring and an event: the bar does the reading,
    // the formatting and the writing, on its own time, where nobody is waiting.
    //
    // The decoded form travels, not the raw copy: the raw bytes are the shell's private structure at offsets
    // this module measured, and a reader that had to decode them itself would be a second place for those
    // offsets to be wrong. See platform/TrayRing.h.
    g_ring.Publish(registration);
    if (g_ring.refused && !g_ring.reported) {
        g_ring.reported = true;
        ReportLine(L"call S %hs pid %lu: the ring answered with a different ABI -- not publishing (see "
                   L"platform/TrayRing.h)", MessageName(registration.command), GetCurrentProcessId());
    }
    (void)receiver;
    (void)wparam;
    (void)channel;
}

LRESULT CALLBACK SentProc(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0 && lparam) {
        const CWPSTRUCT* call = reinterpret_cast<const CWPSTRUCT*>(lparam);
        if (call && call->message == WM_COPYDATA) {
            // THE LEARNING PATH, and it is NOT behind the marker file: a registration is exactly what this
            // module is here for. Read while the copy is still marshalled into this process, because a sent
            // message's payload is valid only for the duration of the call.
            __try {
                ReportShellCopy(L"SENT", call->hwnd, call->wParam, call->lParam);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ReportLine(L"call S ? pid %lu: reading a copy's payload faulted", GetCurrentProcessId());
            }
        }
        if (g_watch && call && WorthObserving(call->message)) {
            ReportObserved(L"sent", call->hwnd, call->message, call->wParam, call->lParam);
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

// One CALLWNDPROC hook on one thread. hMod is NULL because the procedure lives in this very process
// (documented for exactly this case), and the hooks are deliberately never removed: this module is pinned and
// the process is about to be measured, not lived in.
bool HookOneThread(DWORD threadId, unsigned* error) {
    if (threadId == 0) return false;
    if (g_sentHookCount >= kMaxSentHooks) return false;
    const HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, SentProc, nullptr, threadId);
    if (!hook) {
        if (error) *error = GetLastError();
        return false;
    }
    g_sentHooks[g_sentHookCount++] = hook;
    return true;
}

// The thread the shell DELIVERS registrations to, and whether it is ours to hook at all.
//
// "Ours" decides the whole strategy: hMod NULL means the hook procedure lives in the CALLING process, so only
// its own threads can be hooked. Inside the shell that is exactly the case this module was deployed for;
// inside an application the tray window belongs to the shell, where this hook would be refused, and the walk
// below is the only route that can work.
DWORD TrayThreadForThisProcess(bool* inShell) {
    if (inShell) *inShell = false;
    const HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) return 0;
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(tray, &pid);
    if (inShell) *inShell = pid == GetCurrentProcessId();
    return tid;
}

// WHAT ONE ATTEMPT MANAGED. Every number here travels to the bar (see PublishSentWatch), because the failure
// this replaces was silent: a pass reported 143 threads and 57 accepted hooks with no line saying WHICH
// threads, so it could not be told apart from "the tray thread was in the other 86".
struct SentWatchReport {
    bool ready = false;        // the channel is up: the thread that receives registrations is watched
    bool inShell = false;
    unsigned trayThread = 0;
    unsigned hooks = 0;        // hooks this process holds by the end of the attempt
    unsigned attempts = 0;
    unsigned lastError = 0;
};
SentWatchReport g_sentWatch;

// THE HANDSHAKE, and why it goes to the ring rather than to the report file.
//
// The bar used to answer "is anybody listening?" with a sleep, and its first ask after Explorer restarted
// went out before this side existed at all -- measured 2026-10-16: 0 registrations captured from a pass that
// reported 143 thread hooks, and 42 from the same build minutes later. A record in shared memory is the one
// signal that survives the bar truncating the report file (a pinned module holds its handle and keeps writing
// into the unlinked file, see platform/TrayRing.h), and it is where the bar is already looking.
void PublishSentWatch(const SentWatchReport& report) {
    island::TrayHookState state;
    state.pid = GetCurrentProcessId();
    state.inShell = report.inShell;
    state.ready = report.ready;
    state.trayThread = report.trayThread;
    state.hooks = report.hooks;
    state.attempts = report.attempts;
    state.lastError = report.lastError;
    g_ring.PublishHookState(state);
}

// One attempt at the sent channel. `first` adds the snapshot walk; the retries repeat only the ONE hook that
// matters, because the walk cannot be undone and repeating it would add hooks without adding coverage.
//
// ORDER IS THE FIX OF 2026-10-16. A registration is a WM_COPYDATA SENT to Shell_TrayWnd, so the hook that
// sees it is a WH_CALLWNDPROC on THAT WINDOW'S THREAD. The walk alone never promised that thread: it hooks up
// to 96 of this process's 143 threads in whatever order the snapshot produced. The measurement that settled
// it is the probe in scripts/retired/tray (probe-tray-observer.cpp): hooked on that ONE thread, it captures
// 12-53 registrations every single time.
void AttemptSentWatch(bool first) {
    const LONG attempt = InterlockedIncrement(&g_sentAttempts);
    SentWatchReport report;
    report.attempts = static_cast<unsigned>(attempt);
    report.trayThread = TrayThreadForThisProcess(&report.inShell);

    if (report.inShell && report.trayThread) {
        if (HookOneThread(report.trayThread, &report.lastError)) report.ready = true;
    }

    if (!report.ready && first) {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot != INVALID_HANDLE_VALUE) {
            THREADENTRY32 entry = {};
            entry.dwSize = sizeof(entry);
            if (Thread32First(snapshot, &entry)) {
                do {
                    if (entry.th32OwnerProcessID != GetCurrentProcessId()) continue;
                    if (g_sentHookCount >= kMaxSentHooks) break;
                    unsigned error = 0;
                    if (!HookOneThread(entry.th32ThreadID, &error)) {
                        if (error && !report.ready) report.lastError = error;
                        continue;
                    }
                    // ... and the walk DOES settle the question when it happens to cover the tray thread:
                    // that, and not the number of hooks, is what makes the channel ready.
                    if (report.inShell && entry.th32ThreadID == report.trayThread) report.ready = true;
                } while (Thread32Next(snapshot, &entry));
            }
            CloseHandle(snapshot);
        }
    }

    report.hooks = static_cast<unsigned>(g_sentHookCount);
    // Outside the shell there is no tray thread to be ready ON: any accepted hook is the channel, exactly as
    // it was before this change.
    if (!report.inShell) report.ready = report.hooks > 0;

    g_sentWatch = report;
    if (report.ready || attempt >= kMaxSentAttempts) InterlockedExchange(&g_sentSettled, 1);

    PublishSentWatch(report);
    ReportLine(L"sent watch pid %lu: %ls, tray thread %u, %u of this process's threads hooked "
               L"(attempt %u of %ld, last error %u)",
               GetCurrentProcessId(),
               report.ready ? L"the channel is up" : L"NOT ready -- registrations may go unread",
               report.trayThread, report.hooks, report.attempts, kMaxSentAttempts, report.lastError);
}

// The retries, driven by the messages this process already handles: no sleep and no spin inside somebody
// else's message pump, one attempt per message, and nothing at all once the state is settled.
void MaybeRetrySentWatch() {
    if (!g_sentAttempted || g_sentSettled || g_sentInFlight) return;
    if (InterlockedCompareExchange(&g_sentInFlight, 1, 0) != 0) return;
    AttemptSentWatch(false);
    InterlockedExchange(&g_sentInFlight, 0);
}

// What one walk found: how many slots were redirected, and how many ALREADY pointed at the detour. The
// second number is the one that answers "is this application being watched", and it is not the same question
// as the first: measured, a process reported 0 redirects and then captured four calls anyway, which sent the
// first version of this log down the wrong path. Whatever install time says, the count that matters is the
// one taken at the moment of a call.
struct PatchCount {
    int patched = 0;
    int alreadyOurs = 0;
    // How many of `patched` held an address that was in NO loaded module -- i.e. a slot this walk REPAIRED
    // rather than redirected. See OwnerOfSlot: a copy of this module that was unloaded leaves exactly that,
    // and it is what crashed Explorer and wallpaper64.exe on this machine on 2026-10-15.
    int healed = 0;
    // ... and the same three numbers for the GetProcAddress slots of this process.
    int patchedGetProcAddress = 0;
    int alreadyOursGetProcAddress = 0;
};

// The module a code address belongs to, and its file name.
HMODULE ModuleOfAddress(void* address, const wchar_t** leafOut) {
    if (leafOut) *leafOut = nullptr;
    if (!address) return nullptr;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module) ||
        !module) {
        return nullptr;
    }
    static wchar_t path[MAX_PATH];   // one walk at a time, and this is only used for its file name
    path[0] = 0;
    if (GetModuleFileNameW(module, path, MAX_PATH) == 0) return module;
    const wchar_t* leaf = wcsrchr(path, L'\\');
    if (leafOut) *leafOut = leaf ? leaf + 1 : path;
    return module;
}

bool IsOurs(const wchar_t* leaf) {
    if (!leaf) return false;
    // ANY VERSION OF THIS MODULE, by prefix rather than by exact name.
    //
    // The module is deployed under a content-stamped file name (see TrayLearn.cpp: the copy in the target
    // process is PINNED and therefore locked, so a new build must arrive as a new file). An application can
    // therefore hold slots pointing at `island_tray_hook-3f2a....dll` while this copy is
    // `island_tray_hook-91bc....dll`, and the newer copy is the one that has to take those slots over.
    if (_wcsnicmp(leaf, L"island_tray_hook", 16) == 0) return true;
    return _wcsicmp(leaf, L"probe-tray-notifyicon-hook.dll") == 0;
}

// WHAT A SLOT HOLDS RIGHT NOW, which is the question "may this be overwritten" is really asking.
//
// This used to be a yes/no over three cases, and the case it refused is the one that shipped a crash: an
// address that belongs to NO loaded module. Measured on this machine (2026-10-15): the earlier build
// unhooked and unloaded itself at the end of the learning window while applications' import slots still
// pointed at its detour, so Explorer crashed six times, wallpaper64.exe four times (Application log:
// module `island_tray_hook.dll_unloaded`, exception 0xc0000005 at offset 0x10a0 -- the detour's entry).
//
// A slot NAMED Shell_NotifyIcon (the name is checked before this, see PatchThunkArray) whose value is in no
// module at all is either that dangling pointer or a stub of a module that has been unloaded: either way the
// value is unusable, and pointing it at a live function is the repair rather than a risk. Every OTHER case
// is unchanged, including the one that stays out of bounds: a real module that is not ours, not shell32 and
// not the importing module itself (MyDockFinder's own hook, for one) is never touched.
enum class SlotOwner {
    NotOurs,   // inside some other module of the application's
    Shell,     // shell32's own function: the normal case
    Ours,      // an earlier copy of this module, or of the probe that came before it
    Stub,      // the importing module's own delay-load stub
    Orphan,    // in no loaded module: a dangling pointer left by a copy of ours that was unloaded
};

SlotOwner OwnerOfSlot(void* address, HMODULE ownModule) {
    if (!address) return SlotOwner::NotOurs;
    const wchar_t* leaf = nullptr;
    HMODULE owner = ModuleOfAddress(address, &leaf);
    if (!owner) return SlotOwner::Orphan;
    if (owner == ownModule) return SlotOwner::Stub;
    if (IsOurs(leaf)) return SlotOwner::Ours;
    if (leaf && _wcsicmp(leaf, L"shell32.dll") == 0) return SlotOwner::Shell;
    return SlotOwner::NotOurs;
}

bool MayTakeOver(SlotOwner owner) { return owner != SlotOwner::NotOurs; }

// MAY THIS IMPORT SLOT BE REDIRECTED? The slot's NAME has already said it is Shell_NotifyIcon; this decides
// whether the value sitting in it may be overwritten. Three states may be taken over, and everything else is
// left exactly as it is:
//
//   * shell32's own function, which is the normal case;
//   * an EARLIER COPY OF OURS: measured, an application hooked by a previous build (or by the probe that came
//     before this module) keeps its slot pointing at that older module. The first version of this walk only
//     replaced slots holding shell32's address, so those applications reported "0 slots" and stayed unwatched
//     until somebody restarted them -- which is exactly what the user hit;
//   * the linker's delay-load stub, which lives in the module being walked.
//
// Somebody else's hook (MyDockFinder's, another tray tool's) is not ours to replace. Neither is an address
// that cannot be placed in any module at all: an earlier version of this treated that as "a stub, take it",
// and the walk then redirected junk -- 816 slots in Explorer, 288 in Razer, 78 in WeChat, all of them
// unrelated APIs pointed at Shell_NotifyIconW. Conservative by default is the correction.
// Does this import slot NAME Shell_NotifyIcon? The name is the criterion, and it is the one the first version
// of this walk lacked: without it, "the slot belongs to shell32" was mistaken for "the slot is the function we
// want", and every one of an application's shell32 imports was redirected to this module's detour.
bool SlotNames(IMAGE_THUNK_DATA* slot, IMAGE_THUNK_DATA* byName, BYTE* base, const char* wanted) {
    if (!slot || !byName || !wanted) return false;
    if (byName->u1.Ordinal & IMAGE_ORDINAL_FLAG) return false;
    const IMAGE_IMPORT_BY_NAME* hint = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + byName->u1.AddressOfData);
    return hint && strcmp(reinterpret_cast<const char*>(hint->Name), wanted) == 0;
}

// How many slots one process may legitimately have redirected at once. An application imports
// Shell_NotifyIcon once, sometimes twice (A and W), across a handful of its own modules -- so a walk that
// wants to redirect dozens has gone wrong, and the honest answer is to stop rather than to keep pointing
// somebody else's imports at our function.
const int kMaxRedirectsPerProcess = 8;

// ... AND A SECOND CAP, for the GetProcAddress slots (see DetourGetProcAddress). Those are expected in the
// dozens -- every module that resolves anything dynamically imports it -- so the limit only exists to stop a
// walk that has gone wrong, and it is deliberately well above what a real process carries.
const int kMaxGetProcAddressRedirects = 96;

// One import slot by address: an ordinary import already holds the address being replaced.
//
// `patch` false means COUNT ONLY, which is how the watch checks below ask "what points at the detour right
// now" without touching anything. The first version of that check passed a null target instead and got its
// early return: it reported 0 while the detour was demonstrably being called.
void PatchThunkArray(IMAGE_THUNK_DATA* iat, IMAGE_THUNK_DATA* byName, BYTE* base, void* targetW, void* detourW,
                     void* targetA, void* detourA, void* targetG, void* detourG, PatchCount* count, bool patch,
                     HMODULE ownModule) {
    if (!iat || !detourW || !detourA || !count) return;
    for (; iat->u1.Function; ++iat, byName = byName ? byName + 1 : nullptr) {
        void* current = reinterpret_cast<void*>(iat->u1.Function);
        if (current == detourW || current == detourA || (detourG && current == detourG)) {
            ++count->alreadyOurs;
            continue;
        }
        if (!patch) continue;
        // The application's own way of looking Shell_NotifyIcon up (see DetourGetProcAddress): patched by
        // name as well, because a slot holding GetProcAddress is what a dynamic framework calls.
        if (detourG && targetG &&
            ((current == targetG) || SlotNames(iat, byName, base, "GetProcAddress"))) {
            const SlotOwner owner = OwnerOfSlot(current, ownModule);
            // THE SLOT IS NAMED GetProcAddress, and the only value it can legitimately hold is the system's
            // own function -- which lives in kernelbase.dll (kernel32 forwards to it since Windows 7), a
            // module MayTakeOver deliberately refuses. The name check is what makes allowing it safe here:
            // this branch is only reached for a slot the import table itself calls GetProcAddress.
            const wchar_t* ownerLeaf = nullptr;
            ModuleOfAddress(current, &ownerLeaf);
            const bool isSystem = ownerLeaf && (_wcsicmp(ownerLeaf, L"kernel32.dll") == 0 ||
                                                _wcsicmp(ownerLeaf, L"kernelbase.dll") == 0);
            if (!MayTakeOver(owner) && !isSystem) continue;
            if (count->patchedGetProcAddress >= kMaxGetProcAddressRedirects) continue;
            DWORD old = 0;
            if (!VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) continue;
            iat->u1.Function = reinterpret_cast<ULONG_PTR>(detourG);
            DWORD ignored = 0;
            VirtualProtect(&iat->u1.Function, sizeof(void*), old, &ignored);
            ++count->patchedGetProcAddress;
            continue;
        }
        const bool wantsW = (current == targetW) || SlotNames(iat, byName, base, "Shell_NotifyIconW");
        const bool wantsA = (current == targetA) || SlotNames(iat, byName, base, "Shell_NotifyIconA");
        if (!wantsW && !wantsA) continue;
        const SlotOwner owner = OwnerOfSlot(current, ownModule);
        if (!MayTakeOver(owner)) continue;
        if (count->patched >= kMaxRedirectsPerProcess) continue;   // see kMaxRedirectsPerProcess
        DWORD old = 0;
        if (!VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) continue;
        iat->u1.Function = reinterpret_cast<ULONG_PTR>(wantsW ? detourW : detourA);
        DWORD ignored = 0;
        VirtualProtect(&iat->u1.Function, sizeof(void*), old, &ignored);
        ++count->patched;
        if (owner == SlotOwner::Orphan) ++count->healed;
    }
}

// One delay-loaded slot, by NAME: its slot may still hold the linker's stub rather than the function, so an
// address comparison would miss it.
void PatchDelaySlot(IMAGE_THUNK_DATA* slot, IMAGE_THUNK_DATA* byName, const char* wanted, void* detour,
                    PatchCount* count, bool patch, HMODULE ownModule) {
    if (!slot || !byName || !wanted || !detour || !count) return;
    if (byName->u1.Ordinal & IMAGE_ORDINAL_FLAG) return;
    const IMAGE_IMPORT_BY_NAME* hint = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(byName->u1.AddressOfData);
    if (!hint || strcmp(reinterpret_cast<const char*>(hint->Name), wanted) != 0) return;
    if (reinterpret_cast<void*>(slot->u1.Function) == detour) {
        ++count->alreadyOurs;
        return;
    }
    if (!patch) return;
    const SlotOwner owner = OwnerOfSlot(reinterpret_cast<void*>(slot->u1.Function), ownModule);
    if (!MayTakeOver(owner)) return;
    DWORD old = 0;
    if (!VirtualProtect(&slot->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return;
    slot->u1.Function = reinterpret_cast<ULONG_PTR>(detour);
    DWORD ignored = 0;
    VirtualProtect(&slot->u1.Function, sizeof(void*), old, &ignored);
    ++count->patched;
    if (owner == SlotOwner::Orphan) ++count->healed;
}

// Every offset is checked against the module's SizeOfImage before it is followed: measured, an unchecked walk
// of this kind killed the target process on its third module.
void PatchModuleUnchecked(HMODULE module, size_t imageSize, void* targetW, void* detourW, void* targetA,
                          void* detourA, void* targetG, void* detourG, PatchCount* count, bool patch) {
    if (!module || imageSize < sizeof(IMAGE_DOS_HEADER) || !count) return;
    BYTE* base = reinterpret_cast<BYTE*>(module);
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    if (dos->e_lfanew < 0 || static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS) > imageSize)
        return;
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    const IMAGE_DATA_DIRECTORY& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports.VirtualAddress && imports.Size && imports.VirtualAddress < imageSize) {
        const IMAGE_IMPORT_DESCRIPTOR* descriptor =
            reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
        for (; descriptor->Name && descriptor->Name < imageSize; ++descriptor) {
            if (descriptor->FirstThunk == 0 || descriptor->FirstThunk >= imageSize) continue;
            IMAGE_THUNK_DATA* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
            IMAGE_THUNK_DATA* byName =
                descriptor->OriginalFirstThunk ? reinterpret_cast<IMAGE_THUNK_DATA*>(
                                                     base + descriptor->OriginalFirstThunk)
                                               : nullptr;
            PatchThunkArray(iat, byName, base, targetW, detourW, targetA, detourA, targetG, detourG, count,
                            patch, module);
        }
    }

    const IMAGE_DATA_DIRECTORY& delayed = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (delayed.VirtualAddress && delayed.Size && delayed.VirtualAddress < imageSize) {
        const ImgDelayDescr* descriptor = reinterpret_cast<const ImgDelayDescr*>(base + delayed.VirtualAddress);
        for (; descriptor->rvaDLLName && descriptor->rvaDLLName < imageSize; ++descriptor) {
            if ((descriptor->grAttrs & dlattrRva) == 0) continue;   // only the RVA form is understood here
            if (descriptor->rvaIAT == 0 || descriptor->rvaIAT >= imageSize) continue;
            if (descriptor->rvaINT == 0 || descriptor->rvaINT >= imageSize) continue;
            IMAGE_THUNK_DATA* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->rvaIAT);
            IMAGE_THUNK_DATA* byName = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->rvaINT);
            int slots = 0;
            for (; iat->u1.Function && slots < 4096; ++iat, ++byName, ++slots) {
                PatchDelaySlot(iat, byName, "Shell_NotifyIconW", detourW, count, patch, module);
                PatchDelaySlot(iat, byName, "Shell_NotifyIconA", detourA, count, patch, module);
            }
        }
    }
}

PatchCount WalkProcessImports(void* targetW, void* detourW, void* targetA, void* detourA, void* targetG,
                              void* detourG, bool patch) {
    PatchCount count;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W entry = {};
        entry.dwSize = sizeof(entry);
        if (Module32FirstW(snapshot, &entry)) {
            do {
                __try {
                    PatchModuleUnchecked(entry.hModule, entry.modBaseSize, targetW, detourW, targetA, detourA,
                                         targetG, detourG, &count, patch);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    // A module whose headers do not walk cleanly is skipped: measured, an unguarded walk
                    // killed the target process on its third module.
                }
            } while (Module32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    return count;
}

void* RealAddress(void* volatile* slot, const char* name) {
    if (*slot) return *slot;
    HMODULE shell = GetModuleHandleW(L"shell32.dll");
    if (!shell) shell = LoadLibraryW(L"shell32.dll");
    if (!shell) return nullptr;
    PVOID address = reinterpret_cast<PVOID>(GetProcAddress(shell, name));
    InterlockedCompareExchangePointer(slot, address, nullptr);
    return *slot;
}

const char* MessageName(DWORD message) {
    switch (message) {
        case NIM_ADD: return "NIM_ADD";
        case NIM_MODIFY: return "NIM_MODIFY";
        case NIM_DELETE: return "NIM_DELETE";
        case NIM_SETFOCUS: return "NIM_SETFOCUS";
        case NIM_SETVERSION: return "NIM_SETVERSION";
        default: return "NIM_?";
    }
}

void LogCall(const char* which, DWORD message, const void* data, void* returnAddress) {
    // ASK THE IMPORT TABLES AGAIN, on the first call of this process. "install time said 0 slots" and "four
    // calls arrived anyway" were both true at once, and that contradiction is worth a line of its own rather
    // than a guess: nothing is redirected here (no target given), so what this counts is exactly the number of
    // slots that already point at the detour.
    static volatile LONG firstCall = 0;
    if (InterlockedCompareExchange(&firstCall, 1, 0) == 0) {
        const PatchCount now = WalkProcessImports(nullptr, reinterpret_cast<void*>(&DetourShellNotifyIconW),
                                                 nullptr, reinterpret_cast<void*>(&DetourShellNotifyIconA),
                                                 nullptr, reinterpret_cast<void*>(&DetourGetProcAddress), false);
        ReportLine(L"watch check pid %lu: %d slot(s) point at the detour at the moment of the first call "
                   L"(install time said %ld)",
                   GetCurrentProcessId(), now.alreadyOurs, g_hooked);
    }
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const wchar_t* leaf = wcsrchr(self, L'\\');
    const wchar_t* exe = leaf ? leaf + 1 : self;
    wchar_t caller[64] = {};
    HMODULE owner = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(returnAddress), &owner) &&
        owner) {
        wchar_t full[MAX_PATH] = {};
        GetModuleFileNameW(owner, full, MAX_PATH);
        const wchar_t* callerLeaf = wcsrchr(full, L'\\');
        wcsncpy_s(caller, callerLeaf ? callerLeaf + 1 : full, _TRUNCATE);
    } else {
        wcscpy_s(caller, L"?");
    }
    if (!data) {
        ReportLine(L"call %hs %hs pid %lu exe %ls caller %ls hwnd NULL", which, MessageName(message),
                   GetCurrentProcessId(), exe, caller);
        return;
    }
    HWND window = nullptr;
    UINT uid = 0;
    UINT callback = 0;
    UINT version = 0;
    UINT flags = 0;
    wchar_t tip[128] = {};
    // THE GUID, for the applications that name their icon with one instead of a uid -- which is the modern
    // way and, on this machine, what ChatGPT, the shell's own network and volume icons and the security
    // center all use. The press needs it because a uid of 0 is not an identity: several icons of one
    // application share it, and a press that matched on it would deliver one icon's callback to another.
    wchar_t guidText[64] = {};
    GUID registration = {};
    if (strcmp(which, "W") == 0) {
        const NOTIFYICONDATAW* nid = reinterpret_cast<const NOTIFYICONDATAW*>(data);
        window = nid->hWnd;
        uid = nid->uID;
        callback = nid->uCallbackMessage;
        version = nid->uVersion;
        flags = nid->uFlags;
        registration = nid->guidItem;
        wcsncpy_s(tip, nid->szTip, _TRUNCATE);
    } else {
        const NOTIFYICONDATAA* nid = reinterpret_cast<const NOTIFYICONDATAA*>(data);
        window = nid->hWnd;
        uid = nid->uID;
        callback = nid->uCallbackMessage;
        version = nid->uVersion;
        flags = nid->uFlags;
        registration = nid->guidItem;
        MultiByteToWideChar(CP_ACP, 0, nid->szTip, -1, tip, 128);
    }
    {
        GUID none = {};
        if (memcmp(&registration, &none, sizeof(GUID)) != 0) {
            GuidText(registration, guidText, 64);
        }
    }
    // A tooltip may be a whole sentence with line breaks, and one call has to stay one line: the bar reads
    // this file line by line.
    for (wchar_t* p = tip; *p; ++p) {
        if (*p == L'\n' || *p == L'\r' || *p == L'"') *p = L' ';
    }
    // TEMPORARY (see the block above): remember what this application registered, so the two observation
    // channels know what to look for. Removed with the rest of that block.
    if (message == NIM_ADD && callback) {
        g_callbackMessage = static_cast<LONG>(callback);
        g_trayWindow = window;
    }
    // The hook count is repeated here on purpose: measured, the "hooked ... 0 slot(s)" line was written in
    // one process that then captured calls anyway, and a future session reading only that line would
    // conclude the mechanism cannot work. Whatever the reading at install time says, this line is the count
    // in force at the moment of the call.
    ReportLine(L"call %hs %hs pid %lu exe %ls caller %ls hwnd %p uid %u msg 0x%04X version %u flags 0x%X "
               L"guid %ls tip \"%ls\" (slots %ld)",
               which, MessageName(message), GetCurrentProcessId(), exe, caller, window, uid, callback,
               version, flags, guidText[0] ? guidText : L"-", tip, g_hooked);
}

typedef BOOL(WINAPI* ShellNotifyIconW_t)(DWORD, PNOTIFYICONDATAW);
typedef BOOL(WINAPI* ShellNotifyIconA_t)(DWORD, PNOTIFYICONDATAA);

BOOL WINAPI DetourShellNotifyIconW(DWORD message, PNOTIFYICONDATAW data) {
    LogCall("W", message, data, _ReturnAddress());
    ShellNotifyIconW_t real = reinterpret_cast<ShellNotifyIconW_t>(RealAddress(&g_realW, "Shell_NotifyIconW"));
    if (!real) return FALSE;
    return real(message, data);
}

BOOL WINAPI DetourShellNotifyIconA(DWORD message, PNOTIFYICONDATAA data) {
    LogCall("A", message, data, _ReturnAddress());
    ShellNotifyIconA_t real = reinterpret_cast<ShellNotifyIconA_t>(RealAddress(&g_realA, "Shell_NotifyIconA"));
    if (!real) return FALSE;
    return real(message, data);
}

void EnsureHooked() {
    if (InterlockedCompareExchange(&g_state, 1, 0) != 0) return;

    // The application's file name, for every line written from here on.
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const wchar_t* leaf = wcsrchr(self, L'\\');
    const wchar_t* exe = leaf ? leaf + 1 : self;
    ReportLine(L"attached pid %lu exe %ls", GetCurrentProcessId(), exe);

    // ---- PIN THIS MODULE IN THIS PROCESS, BEFORE THE FIRST SLOT IS TOUCHED ---------------------------------
    //
    // This is the fix for the crash this module shipped on 2026-10-15, and the reason it is first rather
    // than best-effort.
    //
    // What happened: the bar unhooked and freed this module at the end of its learning window, the loader
    // then unloaded the copy inside each application, and the import slots this walk had redirected kept
    // pointing at the detour's address -- in a page that no longer belonged to anything. The next time the
    // application called Shell_NotifyIcon it jumped into freed memory. Measured in the Application log:
    // Explorer.EXE six times, wallpaper64.exe four times, module `island_tray_hook.dll_unloaded`, exception
    // 0xc0000005 / 0xc000041d, fault offset 0x10a0 -- the detour's own entry point.
    //
    // GET_MODULE_HANDLE_EX_FLAG_PIN makes the loader ignore every later FreeLibrary for this module, for the
    // life of this process. The slot therefore always points at code that is still mapped, whether the hook
    // has been removed or the bar has exited. The cost is honest and has to be stated: the file this module
    // was loaded from stays locked until the application restarts, which is why the bar stages it under a
    // content-stamped name (see TrayLearn.cpp) instead of loading `island_tray_hook.dll` directly.
    //
    // If the pin FAILS the walk does not run at all: a redirected slot that can be unloaded again is exactly
    // the state that crashed those applications, and "not watching this application" is the safe answer.
    HMODULE pinned = nullptr;
    const BOOL pinOk = GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&IslandTrayHookProc), &pinned);
    if (!pinOk || !pinned) {
        const DWORD error = GetLastError();
        // 2 = decided, and the decision was "no": the hook procedure must not try this again on every
        // message, and the bar is told why in the report.
        InterlockedExchange(&g_state, 2);
        ReportLine(L"hooked pid %lu exe %ls: REFUSED -- this module could not be pinned in the process "
                   L"(error %lu), so not one import slot was touched",
                   GetCurrentProcessId(), exe, error);
        return;
    }

    // ... AND NOT IN THE BAR'S OWN PROCESS. The global hook (see TrayLearn.cpp) delivers this module to every
    // GUI process on the desktop, its installer included; the bar must not repoint its own imports or appear
    // in the report as one of the applications being watched.
    if (IsOurOwnProcess()) {
        InterlockedExchange(&g_state, 2);
        ReportLine(L"hooked pid %lu exe %ls: this is the bar itself -- not patching its own imports",
                   GetCurrentProcessId(), exe);
        return;
    }

    HMODULE shell = GetModuleHandleW(L"shell32.dll");
    if (!shell) shell = LoadLibraryW(L"shell32.dll");
    void* targetW = shell ? reinterpret_cast<void*>(GetProcAddress(shell, "Shell_NotifyIconW")) : nullptr;
    void* targetA = shell ? reinterpret_cast<void*>(GetProcAddress(shell, "Shell_NotifyIconA")) : nullptr;
    g_realW = targetW;
    g_realA = targetA;

    // The application's own look-up route as well (see DetourGetProcAddress). kernel32's GetProcAddress is
    // taken BEFORE any slot is touched, so the detour can forward through it without ever re-entering itself.
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    void* targetG = kernel ? reinterpret_cast<void*>(GetProcAddress(kernel, "GetProcAddress")) : nullptr;
    if (kernel && !targetG) {
        // Forwarded exports live in kernelbase since Windows 7, and a GetProcAddress that cannot be captured
        // would make the walk redirect slots at a null pointer: measured risk, not a theoretical one.
        HMODULE base = GetModuleHandleW(L"kernelbase.dll");
        if (base) targetG = reinterpret_cast<void*>(GetProcAddress(base, "GetProcAddress"));
    }
    g_realGetProcAddress = targetG;

    const PatchCount count =
        WalkProcessImports(targetW, reinterpret_cast<void*>(&DetourShellNotifyIconW), targetA,
                           reinterpret_cast<void*>(&DetourShellNotifyIconA), targetG,
                           targetG ? reinterpret_cast<void*>(&DetourGetProcAddress) : nullptr, true);
    InterlockedExchange(&g_hooked, count.patched + count.alreadyOurs);
    ReportLine(L"hooked pid %lu exe %ls: %d slot(s) redirected (%d of them repaired a pointer into a module "
               L"that is no longer loaded), %d already pointing here, %d GetProcAddress slot(s) redirected "
               L"(the route a framework that resolves Shell_NotifyIcon by name uses), this module is pinned; "
               L"watching=%ls",
               GetCurrentProcessId(), exe, count.patched, count.healed, count.alreadyOurs,
               count.patchedGetProcAddress,
               (count.patched + count.alreadyOurs) ? L"yes"
                                                   : L"not yet -- it does not import Shell_NotifyIcon; if it "
                                                     L"asks for the address at run time the GetProcAddress "
                                                     L"slots above are what will catch it");

    // THE SENT CHANNEL IS ALWAYS ON: it is how a registration is READ, not a diagnostic.
    //
    // It was moved out of the marker-file gate on 2026-10-15, and the reason is worth keeping: with the gate
    // in place the module was present in the shell, its import slots redirected, and it still reported nothing
    // -- because registrations do not pass through Shell_NotifyIcon in the shell's process, they arrive as a
    // WM_COPYDATA sent TO it (see ReportShellCopy), and that is read on the WH_CALLWNDPROC side only. The
    // marker file still decides whether the NOISY observation log is written; it no longer decides whether
    // registrations are seen.
    //
    // ... AND IT IS ATTEMPTED HERE, ONCE. This runs inside the shell's own message handling, so a failed
    // attempt is retried on the next messages that arrive (MaybeRetrySentWatch, driven by the hook procedure)
    // rather than in a loop that would stall the pump it is running in.
    InterlockedExchange(&g_sentAttempted, 1);
    // Claimed before the attempt, not after: the hook procedure can run on another thread of this process
    // while this one is installing, and it must not start a second attempt beside it.
    InterlockedExchange(&g_sentInFlight, 1);
    AttemptSentWatch(true);
    InterlockedExchange(&g_sentInFlight, 0);
    if (MarkerPresent()) {
        g_watch = true;
        ReportLine(L"observing pid %lu exe %ls: marker present, %d thread(s) hooked for SENT messages and "
                   L"the posted channel is on", GetCurrentProcessId(), exe, g_sentHookCount);
    }
}

} // namespace

// The hook procedure the bar installs. It is inert: it looks at no message and changes none. Its presence is
// what gets this module loaded into the application at all, and the first call is what does the hooking.
//
// IT MAY NOT THROW, AND THAT IS NOT DEFENSIVENESS. This runs inside somebody else's message pump: an
// exception that leaves it either kills the application (measured on this machine as exception 0xc000041d,
// "a fatal exception escaped a user callback", which is how wallpaper64.exe died twice on 2026-10-15) or is
// swallowed and leaves the hook in an unknown state. Every failure mode this module has is already reported
// through the report file, so the answer is to let nothing escape.
extern "C" __declspec(dllexport) LRESULT CALLBACK IslandTrayHookProc(int code, WPARAM wparam, LPARAM lparam) {
    __try {
        EnsureHooked();
        // THE SENT CHANNEL'S LATER ATTEMPTS, on the messages this process is already handling. A tray thread
        // that refused a hook while Explorer was still starting up may accept it a message later, and this is
        // where that is noticed without anybody waiting.
        MaybeRetrySentWatch();
        // TEMPORARY (see the observation block above): messages the application PULLS OUT OF ITS QUEUE that
        // are for its own tray window, or that carry its registered callback number. The filter is what keeps
        // this from logging the application's whole private message traffic.
        if (g_watch && code >= 0 && lparam) {
            const MSG* message = reinterpret_cast<const MSG*>(lparam);
            // ... and only the private message range for the tray window: measured, its queue carries a
            // WM_TIMER every few milliseconds, which is the application beating, not the shell pressing.
            if (message &&
                (message->message == (UINT)g_callbackMessage ||
                 (message->hwnd == g_trayWindow && message->message >= WM_USER))) {
                ReportObserved(L"posted", message->hwnd, message->message, message->wParam, message->lParam);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_state, 2);   // see EnsureHooked: no second attempt after a failure
        ReportLine(L"hook procedure pid %lu: the install raised 0x%08lX and the application was left "
                   L"untouched", GetCurrentProcessId(), static_cast<unsigned long>(GetExceptionCode()));
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
