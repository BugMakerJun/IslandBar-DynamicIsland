// =====================================================================
//  DisplayBrightness.cpp -- see the header for why DDC/CI and not WMI.
// =====================================================================
#include "platform/DisplayBrightness.h"

#include "core/Log.h"
#include "platform/WorkerThread.h"

#include <mutex>

#pragma comment(lib, "dxva2.lib")
#pragma comment(lib, "user32.lib")

namespace island {
namespace {

// The physical-monitor API, declared here rather than by including
// lowlevelmonitorconfigurationapi.h: six functions, and the header drags in a chain this file has no
// use for. The layouts and signatures are the documented ones (dxva2.dll).
struct PHYSICAL_MONITOR {
    HANDLE hPhysicalMonitor;
    wchar_t szPhysicalMonitorDescription[128];
};

extern "C" {
BOOL WINAPI GetNumberOfPhysicalMonitorsFromHMONITOR(HMONITOR monitor, LPDWORD count);
BOOL WINAPI GetPhysicalMonitorsFromHMONITOR(HMONITOR monitor, DWORD count,
                                            PHYSICAL_MONITOR* monitors);
BOOL WINAPI DestroyPhysicalMonitors(DWORD count, const PHYSICAL_MONITOR* monitors);
BOOL WINAPI GetMonitorCapabilities(HANDLE monitor, LPDWORD caps, LPDWORD colorTemperatures);
BOOL WINAPI GetMonitorBrightness(HANDLE monitor, LPDWORD minimum, LPDWORD current,
                                 LPDWORD maximum);
BOOL WINAPI SetMonitorBrightness(HANDLE monitor, DWORD brightness);
}

constexpr DWORD kMcCapsBrightness = 0x00000002;

struct CachedMonitor {
    HMONITOR monitor = nullptr;
    PHYSICAL_MONITOR physical = {};
    DWORD minimum = 0;
    DWORD maximum = 100;
    bool valid = false;
};

CachedMonitor g_cached;
std::mutex g_mutex;

void DropLocked() {
    if (g_cached.physical.hPhysicalMonitor) {
        DestroyPhysicalMonitors(1, &g_cached.physical);
    }
    g_cached = CachedMonitor{};
}

// The monitor's handle, acquired once per display. A DDC/CI call goes over the video cable, which is
// not free, so the handle is kept; it is dropped when the bar moves to another display (the bar
// follows the pointer) or when a call stops answering.
bool AcquireLocked(HMONITOR monitor) {
    if (g_cached.valid && g_cached.monitor == monitor) return true;
    DropLocked();
    if (!monitor) return false;

    DWORD count = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(monitor, &count) || count == 0) {
        LogLine(L"  brightness: the display reports no physical monitors");
        return false;
    }

    // One is all this bar asks about. A display behind a KVM or a splitter can report more than one,
    // and the first is what the monitor itself would call "the monitor".
    PHYSICAL_MONITOR physical = {};
    if (!GetPhysicalMonitorsFromHMONITOR(monitor, 1, &physical) || !physical.hPhysicalMonitor) {
        LogLine(L"  brightness: the physical monitor could not be opened");
        return false;
    }

    DWORD caps = 0;
    DWORD temperatures = 0;
    if (!GetMonitorCapabilities(physical.hPhysicalMonitor, &caps, &temperatures)) {
        LogLine(L"  brightness: the monitor did not answer its capabilities (DDC/CI off in its menu?)");
        DestroyPhysicalMonitors(1, &physical);
        return false;
    }
    if ((caps & kMcCapsBrightness) == 0) {
        LogLine(L"  brightness: capabilities 0x%lX -- this monitor does not expose brightness",
                static_cast<unsigned long>(caps));
        DestroyPhysicalMonitors(1, &physical);
        return false;
    }

    DWORD minimum = 0;
    DWORD current = 0;
    DWORD maximum = 0;
    if (!GetMonitorBrightness(physical.hPhysicalMonitor, &minimum, &current, &maximum) ||
        maximum <= minimum) {
        LogLine(L"  brightness: the monitor would not report its range");
        DestroyPhysicalMonitors(1, &physical);
        return false;
    }

    g_cached.monitor = monitor;
    g_cached.physical = physical;
    g_cached.minimum = minimum;
    g_cached.maximum = maximum;
    g_cached.valid = true;
    LogLine(L"  brightness: DDC/CI on '%ls', range %lu..%lu, now %lu",
            physical.szPhysicalMonitorDescription, static_cast<unsigned long>(minimum),
            static_cast<unsigned long>(maximum), static_cast<unsigned long>(current));
    return true;
}

bool ReadLocked(int* percent) {
    DWORD minimum = 0;
    DWORD current = 0;
    DWORD maximum = 0;
    if (!GetMonitorBrightness(g_cached.physical.hPhysicalMonitor, &minimum, &current, &maximum) ||
        maximum <= minimum) {
        // A handle that has stopped answering is worth re-acquiring once: a monitor switched off and on
        // again, or moved to another input, invalidates it.
        DropLocked();
        return false;
    }

    g_cached.minimum = minimum;
    g_cached.maximum = maximum;
    if (percent) {
        *percent = static_cast<int>((static_cast<unsigned long long>(current - minimum) * 100 +
                                     (maximum - minimum) / 2) /
                                    (maximum - minimum));
    }
    return true;
}

DWORD ToRaw(int percent, DWORD minimum, DWORD maximum) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    const DWORD span = maximum - minimum;
    return minimum + static_cast<DWORD>((static_cast<unsigned long long>(span) * percent + 50) / 100);
}

} // namespace

bool BrightnessAvailable(HMONITOR monitor) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return AcquireLocked(monitor);
}

bool ReadBrightnessPercent(HMONITOR monitor, int* percent) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!AcquireLocked(monitor)) return false;
    return ReadLocked(percent);
}

bool SetBrightnessPercent(HMONITOR monitor, int percent, int* landedPercent) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!AcquireLocked(monitor)) return false;

    const DWORD minimum = g_cached.minimum;
    const DWORD maximum = g_cached.maximum;
    const DWORD wanted = ToRaw(percent, minimum, maximum);
    if (!SetMonitorBrightness(g_cached.physical.hPhysicalMonitor, wanted)) {
        if (landedPercent) *landedPercent = percent;
        return false;
    }

    // Read back, because DDC/CI drops writes without saying so: some monitors reject a value, some stop
    // answering after a burst of them, and the call's return value does not distinguish any of that.
    int landed = percent;
    if (!ReadLocked(&landed)) {
        if (landedPercent) *landedPercent = percent;
        return false;
    }
    if (landedPercent) *landedPercent = landed;
    // Compared in the MONITOR's own units: one raw step at the bottom of the range is worth more than a
    // percentage, and a monitor with a 0..10 range must not be judged on a scale it does not have.
    const DWORD readBack = ToRaw(landed, minimum, maximum);
    return readBack == wanted;
}

// --- the asynchronous read -------------------------------------------------------------------------
//
// One worker for the whole process, made on the first request. It waits on two events -- its stop and
// its own wake-up -- and does nothing else, so a bar that never opens the panel never pays for it.
namespace {

struct AsyncRead {
    WorkerThread worker;
    HANDLE wake = nullptr;
    std::mutex mutex;
    HMONITOR monitor = nullptr;
    bool pending = false;
    bool finished = false;
    bool ok = false;
    int percent = 0;

    ~AsyncRead() {
        worker.Stop();
        if (wake) CloseHandle(wake);
    }
};

AsyncRead& Reader() {
    static AsyncRead reader;
    return reader;
}

unsigned long __stdcall ReadThread(void* self) {
    auto& state = *static_cast<AsyncRead*>(self);
    const HANDLE events[2] = {state.worker.StopEvent(), state.wake};
    while (true) {
        if (WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) break;
        HMONITOR monitor = nullptr;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            monitor = state.monitor;
        }
        int percent = 0;
        const bool ok = ReadBrightnessPercent(monitor, &percent);
        if (!ok) LogLine(L"  brightness: the monitor did not answer the reading");
        std::lock_guard<std::mutex> lock(state.mutex);
        state.ok = ok;
        state.percent = percent;
        state.finished = true;
        state.pending = false;
    }
    return 0;
}

bool StartReader(AsyncRead& state) {
    if (state.worker.Started()) return true;
    if (!state.worker.Prepare()) return false;
    state.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!state.wake) {
        state.worker.Stop();
        return false;
    }
    DWORD threadId = 0;
    HANDLE thread = CreateThread(nullptr, 0, &ReadThread, &state, 0, &threadId);
    if (!thread) {
        state.worker.Stop();
        CloseHandle(state.wake);
        state.wake = nullptr;
        return false;
    }
    state.worker.Attach(thread);
    return true;
}

}  // namespace

bool RequestBrightnessPercent(HMONITOR monitor) {
    AsyncRead& state = Reader();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.pending) return false;
        state.pending = true;
        state.finished = false;
        state.monitor = monitor;
    }
    if (!StartReader(state)) {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.pending = false;
        return false;
    }
    SetEvent(state.wake);
    return true;
}

bool TakeBrightnessPercent(bool* ok, int* percent) {
    AsyncRead& state = Reader();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.finished) return false;
    state.finished = false;
    if (ok) *ok = state.ok;
    if (percent) *percent = state.percent;
    return true;
}

} // namespace island
