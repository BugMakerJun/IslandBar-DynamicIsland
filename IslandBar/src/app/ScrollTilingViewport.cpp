#include "app/ScrollTilingViewport.h"

#include <algorithm>
#include <cmath>

namespace island {

void ScrollTilingViewport::Reset(int offset) {
    current_ = offset;
    target_ = offset;
    from_ = offset;
    startedMs_ = 0;
    durationMs_ = 0;
}

void ScrollTilingViewport::SetTarget(int target, unsigned long long nowMs,
                                     unsigned long long durationMs) {
    target = std::max(0, target);
    if (target == target_) return;
    from_ = current_;
    target_ = target;
    startedMs_ = nowMs;
    durationMs_ = durationMs;
    if (durationMs_ == 0 || from_ == target_) {
        current_ = target_;
        durationMs_ = 0;
    }
}

bool ScrollTilingViewport::Advance(unsigned long long nowMs, int* offset) {
    if (durationMs_ == 0 || current_ == target_) {
        if (offset) *offset = current_;
        return false;
    }
    const unsigned long long elapsed =
        nowMs > startedMs_ ? nowMs - startedMs_ : 0;
    if (elapsed >= durationMs_) {
        current_ = target_;
        durationMs_ = 0;
        if (offset) *offset = current_;
        return false;
    }
    const double t = static_cast<double>(elapsed) /
                     static_cast<double>(durationMs_);
    const double eased = t * t * (3.0 - 2.0 * t);
    current_ = from_ +
               static_cast<int>(std::lround((target_ - from_) * eased));
    if (offset) *offset = current_;
    return true;
}

bool ScrollTilingViewport::Animating(unsigned long long nowMs) const {
    if (durationMs_ == 0 || current_ == target_) return false;
    return nowMs - startedMs_ < durationMs_;
}

} // namespace island
