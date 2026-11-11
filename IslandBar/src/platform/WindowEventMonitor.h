// =====================================================================
//  WindowEventMonitor.h -- OUTOFCONTEXT WinEvent queue for desktop state.
//
//  The callback does no layout, window enumeration, file I/O or SetWindowPos.
//  It only records a compact event and wakes the IslandBar message loop. The
//  frame loop drains the queue and coalesces the result for tiling, task-row,
//  WeChat and full-screen reconciliation.
// =====================================================================
#pragma once

#include <windows.h>

#include <array>
#include <atomic>
#include <mutex>
#include <vector>

namespace island {

enum class WindowEventKind {
    Foreground,
    Created,
    Shown,
    Hidden,
    Destroyed,
    NameChanged,
};

struct WindowEvent {
    WindowEventKind kind = WindowEventKind::Foreground;
    HWND hwnd = nullptr;
};

class WindowEventMonitor {
public:
    WindowEventMonitor() = default;
    ~WindowEventMonitor();
    WindowEventMonitor(const WindowEventMonitor&) = delete;
    WindowEventMonitor& operator=(const WindowEventMonitor&) = delete;

    bool Start(HWND wakeWindow);
    void Stop();
    bool Running() const {
        return foregroundHook_ != nullptr && objectHook_ != nullptr && nameHook_ != nullptr;
    }
    void Poll(std::vector<WindowEvent>* events);

private:
    static void CALLBACK EventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                   LONG object, LONG child, DWORD thread,
                                   DWORD time);
    void Enqueue(WindowEventKind kind, HWND hwnd);

    HWINEVENTHOOK foregroundHook_ = nullptr;
    HWINEVENTHOOK objectHook_ = nullptr;
    HWINEVENTHOOK nameHook_ = nullptr;
    static constexpr size_t kMaxQueuedEvents = 256;
    std::atomic<HWND> wakeWindow_ = nullptr;
    std::atomic<bool> wakePosted_ = false;
    mutable std::mutex mutex_;
    std::array<WindowEvent, kMaxQueuedEvents> queue_ = {};
    size_t queueHead_ = 0;
    size_t queueCount_ = 0;
    bool receiving_ = false;
};

} // namespace island
