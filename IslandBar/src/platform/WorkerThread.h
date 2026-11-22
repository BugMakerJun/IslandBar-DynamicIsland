// platform/WorkerThread.h -- the stop event, the thread, and the rule that ends both
//
// WHY THIS IS A COMPONENT, and it is a measured reason rather than a taste: seven readers in this program
// start a background thread and must end it the same way -- set the stop event, wait for the thread with a
// TIMEOUT (a thread that never returns must not hang the bar), close the thread handle, close the event.
// Four of them had written that dance out identically, line for line, which is how the copy-paste was found
// (scripts/tmp-dup-scan.ps1, 2026-10-16), and the other three had written it almost identically.
//
// THE COPIES HAD ALREADY DRIFTED, WHICH IS THE LEAK. Three of the seven returned false out of Start()
// without ending what they had created: the stop event stayed open, the thread stayed running (with
// `running_` false, so Stop() was never called again), and the critical section stayed initialised -- and a
// second Start(), which the callers do perform, initialised that section a second time over the same
// storage. Nothing visible leaked in the steady state, which is exactly why it survived: the leak needs a
// start that fails and then gets retried.
//
// WHAT IS DELIBERATELY NOT HERE: the second event. Four of the readers also own a wake-up event
// (a write, a refresh, a toggle) whose auto-reset semantics and whose meaning belong to that reader's own
// protocol. The rule for ending a thread is shared; the protocol of waking it is not.
//
// THE OWNER CREATES THE THREAD, not this class: each reader's entry point is its own static member with its
// own name, and hiding that behind a `std::function` here would cost an allocation and a header to buy
// nothing. The order is Prepare(), then start the thread, then Attach(). Any failure between the first and
// the last is cleaned up by Stop(), which is idempotent -- so a reader that fails halfway does not have to
// write the unwind itself.

#pragma once

#include <windows.h>

#include <atomic>
#include <memory>
#include <new>

#include "core/Log.h"

namespace island {

class WorkerThread {
private:
    struct State {
        HANDLE stopEvent = nullptr;
        HANDLE thread = nullptr;
        std::atomic<bool> stopping{false};

        ~State() {
            if (thread) CloseHandle(thread);
            if (stopEvent) CloseHandle(stopEvent);
        }
    };

public:
    enum class StopResult {
        Stopped,
        Timeout,
        Failed,
    };

    using EntryPoint = unsigned long (__stdcall*)(void*);

    class Signal {
    public:
        HANDLE StopEvent() const { return state_ ? state_->stopEvent : nullptr; }

    private:
        friend class WorkerThread;
        explicit Signal(std::shared_ptr<State> state) : state_(std::move(state)) {}
        std::shared_ptr<State> state_;
    };

    WorkerThread() = default;
    ~WorkerThread() { (void)Stop(0); }

    WorkerThread(const WorkerThread&) = delete;
    WorkerThread& operator=(const WorkerThread&) = delete;

    // Creates the thread and gives its entry point a shared State lease before user code runs.
    bool Start(EntryPoint entry, void* context);

    bool Started() const { return state_ && state_->thread != nullptr; }
    bool Prepared() const { return state_ && state_->stopEvent != nullptr; }

    // The thread itself acquires this before running the owner's entry point. It stays valid even if
    // WorkerThread and the owner are released while a non-cooperative call is still returning.
    static Signal CurrentSignal() { return Signal(g_currentState); }

    StopResult Stop(unsigned long timeoutMs = 3000);

private:
    struct Launch {
        std::shared_ptr<State> state;
        EntryPoint entry = nullptr;
        void* context = nullptr;
    };

    static unsigned long __stdcall Bootstrap(void* argument) {
        std::unique_ptr<Launch> launch(static_cast<Launch*>(argument));
        g_currentState = launch->state;
        launch->entry(launch->context);
        g_currentState.reset();
        return 0;
    }

    bool Prepare();

    std::shared_ptr<State> state_;
    inline static thread_local std::shared_ptr<State> g_currentState;
};

inline bool WorkerThread::Prepare() {
    if (state_) {
        if (!state_->thread) {
            state_.reset();
        } else {
            const DWORD status = WaitForSingleObject(state_->thread, 0);
            if (status == WAIT_TIMEOUT) {
                LogLine(L"  worker thread: start refused while the previous thread is alive");
                return false;
            }
            if (status != WAIT_OBJECT_0) {
                LogLine(L"  worker thread: previous thread state could not be joined");
                return false;
            }
            state_.reset();
        }
    }

    std::shared_ptr<State> state = std::make_shared<State>();
    state->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!state->stopEvent) return false;
    state_ = std::move(state);
    return true;
}

inline bool WorkerThread::Start(EntryPoint entry, void* context) {
    if (!entry || !Prepare()) return false;

    auto* launch = new (std::nothrow) Launch{state_, entry, context};
    if (!launch) {
        state_.reset();
        return false;
    }

    const HANDLE thread = CreateThread(nullptr, 0, &WorkerThread::Bootstrap, launch, 0, nullptr);
    if (!thread) {
        delete launch;
        state_.reset();
        return false;
    }
    state_->thread = thread;
    return true;
}

inline WorkerThread::StopResult WorkerThread::Stop(unsigned long timeoutMs) {
    if (!state_) return StopResult::Stopped;

    std::shared_ptr<State> state = state_;
    if (state->stopEvent) SetEvent(state->stopEvent);
    if (!state->thread) {
        state_.reset();
        return StopResult::Stopped;
    }

    const DWORD status = WaitForSingleObject(state->thread, timeoutMs);
    if (status == WAIT_OBJECT_0) {
        state_.reset();
        return StopResult::Stopped;
    }
    if (status == WAIT_TIMEOUT) {
        state->stopping.store(true, std::memory_order_release);
        LogLine(L"  worker thread: stop timeout state=retained thread=alive prepare=blocked");
        return StopResult::Timeout;
    }

    state->stopping.store(true, std::memory_order_release);
    LogLine(L"  worker thread: stop failed status=%lu state=retained", status);
    return StopResult::Failed;
}

}  // namespace island
