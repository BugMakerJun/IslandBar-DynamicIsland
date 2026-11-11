#include "platform/WindowEventMonitor.h"

#include <algorithm>
#include <atomic>

#pragma comment(lib, "user32.lib")

namespace island {
namespace {

std::atomic<WindowEventMonitor*> g_activeMonitor = nullptr;

} // namespace

WindowEventMonitor::~WindowEventMonitor() {
    Stop();
}

bool WindowEventMonitor::Start(HWND wakeWindow) {
    Stop();
    if (!wakeWindow || !IsWindow(wakeWindow)) return false;
    wakeWindow_.store(wakeWindow, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        receiving_ = true;
    }
    g_activeMonitor.store(this, std::memory_order_release);
    foregroundHook_ = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
        &WindowEventMonitor::EventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    objectHook_ = SetWinEventHook(
        EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE, nullptr,
        &WindowEventMonitor::EventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!foregroundHook_ || !objectHook_) {
        Stop();
        return false;
    }
    return true;
}

void WindowEventMonitor::Stop() {
    g_activeMonitor.store(nullptr, std::memory_order_release);
    if (foregroundHook_) {
        UnhookWinEvent(foregroundHook_);
        foregroundHook_ = nullptr;
    }
    if (objectHook_) {
        UnhookWinEvent(objectHook_);
        objectHook_ = nullptr;
    }
    wakeWindow_.store(nullptr, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        receiving_ = false;
        wakePosted_.store(false, std::memory_order_release);
        queueHead_ = 0;
        queueCount_ = 0;
    }
}

void WindowEventMonitor::Poll(std::vector<WindowEvent>* events) {
    if (!events) return;
    events->clear();
    std::lock_guard<std::mutex> lock(mutex_);
    while (queueCount_ > 0) {
        events->push_back(queue_[queueHead_]);
        queueHead_ = (queueHead_ + 1) % kMaxQueuedEvents;
        --queueCount_;
    }
    queueHead_ = 0;
    wakePosted_.store(false, std::memory_order_release);
}

void CALLBACK WindowEventMonitor::EventProc(
    HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG object, LONG child,
    DWORD thread, DWORD time) {
    (void)hook;
    (void)thread;
    (void)time;
    if (!hwnd) return;

    WindowEventKind kind;
    if (event == EVENT_SYSTEM_FOREGROUND) {
        kind = WindowEventKind::Foreground;
    } else {
        if (object != OBJID_WINDOW || child != CHILDID_SELF) return;
        switch (event) {
            case EVENT_OBJECT_CREATE: kind = WindowEventKind::Created; break;
            case EVENT_OBJECT_SHOW: kind = WindowEventKind::Shown; break;
            case EVENT_OBJECT_HIDE: kind = WindowEventKind::Hidden; break;
            case EVENT_OBJECT_DESTROY: kind = WindowEventKind::Destroyed; break;
            default: return;
        }
    }

    WindowEventMonitor* monitor =
        g_activeMonitor.load(std::memory_order_acquire);
    if (monitor) monitor->Enqueue(kind, hwnd);
}

void WindowEventMonitor::Enqueue(WindowEventKind kind, HWND hwnd) {
    bool postWake = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!receiving_) return;
        if (queueCount_ == kMaxQueuedEvents) {
            queueHead_ = (queueHead_ + 1) % kMaxQueuedEvents;
            --queueCount_;
        }
        const size_t tail =
            (queueHead_ + queueCount_) % kMaxQueuedEvents;
        queue_[tail] = {kind, hwnd};
        ++queueCount_;
        postWake = !wakePosted_.exchange(true, std::memory_order_acq_rel);
    }
    if (!postWake) return;
    const HWND wakeWindow =
        wakeWindow_.load(std::memory_order_acquire);
    if (!wakeWindow || !IsWindow(wakeWindow) ||
        !PostMessageW(wakeWindow, WM_NULL, 0, 0)) {
        wakePosted_.store(false, std::memory_order_release);
    }
}

} // namespace island
