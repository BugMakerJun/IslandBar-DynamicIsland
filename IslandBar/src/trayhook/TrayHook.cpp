// =====================================================================
//  TrayHook.cpp -- the observer that lives inside the shell process
//
//  PRODUCTION IS OBSERVER-ONLY. The bar is interested in the WM_COPYDATA
//  registrations applications send to Shell_TrayWnd. Installing an
//  IAT/GetProcAddress detour in Explorer is unnecessary for that job and put
//  disk I/O and import rewriting in somebody else's message path. This module
//  now installs one WH_CALLWNDPROC hook on the Shell_TrayWnd thread, copies a
//  decoded registration into the ABI 6 ring, and returns immediately.
//
//  The same module also hosts the taskbar recovery watcher. That thread does
//  nothing until the bar disappears; normal registration traffic never touches
//  the recovery file.
//
//  ASCII-only on purpose. Do not add non-ASCII text to this file.
// =====================================================================
#include <windows.h>

#include <cstring>

#include "domain/TrayRegistration.h"
#include "platform/TaskbarRecovery.h"
#include "platform/TrayRing.h"

extern "C" __declspec(dllexport) LRESULT CALLBACK IslandTrayHookProc(int code, WPARAM wparam,
                                                                   LPARAM lparam);

namespace {

constexpr int kMaxSentHooks = 3;
constexpr LONG kMaxSentAttempts = 3;

SRWLOCK g_watchLock = SRWLOCK_INIT;
island::TrayRingPublisher g_ring;
volatile LONG g_state = 0;  // 0 = not initialized, 1 = initializing, 2 = initialized
volatile LONG g_sentAttempted = 0;
volatile LONG g_sentInFlight = 0;
volatile LONG g_sentSettled = 0;
volatile LONG g_sentAttempts = 0;
int g_sentHookCount = 0;
HHOOK g_sentHooks[kMaxSentHooks] = {};
island::TrayHookState g_sentWatch;
void* g_trayWindow = nullptr;

bool IsOurOwnProcess() {
    wchar_t self[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return false;
    const wchar_t* leaf = wcsrchr(self, L'\\');
    return leaf && _wcsicmp(leaf + 1, L"island_bar.exe") == 0;
}

DWORD TrayThreadForThisProcess(bool* inShell) {
    if (inShell) *inShell = false;
    const HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) return 0;
    DWORD pid = 0;
    const DWORD threadId = GetWindowThreadProcessId(tray, &pid);
    if (inShell) *inShell = pid == GetCurrentProcessId();
    if (inShell && *inShell) {
        InterlockedExchangePointer(&g_trayWindow, tray);
    }
    return threadId;
}

LRESULT CALLBACK SentProc(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0 && lparam) {
        const CWPSTRUCT* call = reinterpret_cast<const CWPSTRUCT*>(lparam);
        if (call && call->message == WM_COPYDATA) {
            const HWND tray = static_cast<HWND>(
                InterlockedCompareExchangePointer(&g_trayWindow, nullptr, nullptr));
            if (!tray || call->hwnd == tray) {
                const COPYDATASTRUCT* data = reinterpret_cast<const COPYDATASTRUCT*>(call->lParam);
                if (data && data->lpData) {
                    __try {
                        const island::TrayRegistration registration =
                            island::DecodeTrayRegistration(data->lpData, data->cbData);
                        if (registration.valid) g_ring.Publish(registration);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        // A malformed copy must not take Explorer down.
                    }
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

void PublishWatch(const island::TrayHookState& report) {
    AcquireSRWLockExclusive(&g_watchLock);
    g_sentWatch = report;
    const island::TrayHookState snapshot = g_sentWatch;
    ReleaseSRWLockExclusive(&g_watchLock);
    g_ring.PublishHookState(snapshot);
}

bool HookOneThread(DWORD threadId, unsigned* error) {
    if (!threadId || g_sentHookCount >= kMaxSentHooks) return false;
    const HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, &SentProc, nullptr, threadId);
    if (!hook) {
        if (error) *error = GetLastError();
        return false;
    }
    g_sentHooks[g_sentHookCount++] = hook;
    return true;
}

void AttemptSentWatch(bool first) {
    const LONG attempt = InterlockedIncrement(&g_sentAttempts);
    island::TrayHookState report;
    report.pid = GetCurrentProcessId();
    report.attempts = static_cast<unsigned>(attempt);
    bool inShell = false;
    report.trayThread = TrayThreadForThisProcess(&inShell);
    report.inShell = inShell;

    // The production module is only offered to the shell process. If an old
    // hook injects it somewhere else, stop instead of installing anything in
    // that application.
    if (!inShell) {
        report.lastError = ERROR_INVALID_WINDOW_HANDLE;
        InterlockedExchange(&g_sentSettled, 1);
        PublishWatch(report);
        return;
    }

    if (HookOneThread(report.trayThread, &report.lastError)) {
        report.ready = true;
        PostThreadMessageW(report.trayThread, WM_NULL, 0, 0);
    }
    report.hooks = static_cast<unsigned>(g_sentHookCount);
    if (report.ready || attempt >= kMaxSentAttempts) InterlockedExchange(&g_sentSettled, 1);
    PublishWatch(report);
    (void)first;
}

void MaybeRetrySentWatch() {
    if (!g_sentAttempted || g_sentSettled || g_sentInFlight) return;
    if (InterlockedCompareExchange(&g_sentInFlight, 1, 0) != 0) return;
    AttemptSentWatch(false);
    InterlockedExchange(&g_sentInFlight, 0);
}

void EnsureHooked() {
    if (InterlockedCompareExchange(&g_state, 1, 0) != 0) return;
    if (IsOurOwnProcess()) {
        InterlockedExchange(&g_state, 2);
        return;
    }

    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&IslandTrayHookProc), &pinned) ||
        !pinned) {
        island::TrayHookState report;
        report.pid = GetCurrentProcessId();
        report.lastError = GetLastError();
        PublishWatch(report);
        InterlockedExchange(&g_state, 2);
        return;
    }

    bool inShell = false;
    (void)TrayThreadForThisProcess(&inShell);
#if !defined(ISLANDBAR_TRAY_PROBE_RING)
    if (inShell) (void)island::EnsureTaskbarGuardianThread();
#endif

    InterlockedExchange(&g_sentAttempted, 1);
    InterlockedExchange(&g_sentInFlight, 1);
    AttemptSentWatch(true);
    InterlockedExchange(&g_sentInFlight, 0);
    InterlockedExchange(&g_state, 2);
}

} // namespace

extern "C" __declspec(dllexport) LRESULT CALLBACK IslandTrayHookProc(int code, WPARAM wparam,
                                                                   LPARAM lparam) {
    __try {
        EnsureHooked();
        MaybeRetrySentWatch();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_state, 2);
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    return TRUE;
}
