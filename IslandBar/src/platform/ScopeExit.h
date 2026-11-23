// platform/ScopeExit.h -- run one cleanup action on every scope exit.
//
// Worker entry points use this for state that must also be cleared when an
// exception unwinds to WorkerThread::Bootstrap. The callable must not throw.
#pragma once

#include <type_traits>
#include <utility>

namespace island {

template <typename Function>
class ScopeExit {
public:
    explicit ScopeExit(Function&& function) noexcept
        : function_(std::forward<Function>(function)) {}

    ScopeExit(ScopeExit&& other) noexcept
        : function_(std::move(other.function_)),
          active_(std::exchange(other.active_, false)) {}

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ScopeExit& operator=(ScopeExit&&) = delete;

    ~ScopeExit() {
        if (active_) function_();
    }

    void Dismiss() noexcept { active_ = false; }

private:
    Function function_;
    bool active_ = true;
};

template <typename Function>
ScopeExit<std::decay_t<Function>> MakeScopeExit(Function&& function) {
    return ScopeExit<std::decay_t<Function>>(std::forward<Function>(function));
}

}  // namespace island
