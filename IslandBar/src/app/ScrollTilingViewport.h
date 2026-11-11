// =====================================================================
//  ScrollTilingViewport.h -- one coalescing viewport timeline.
//
//  Events change only the target. A target change during an animation keeps
//  the current position and retargets the remaining motion, so rapid wheel or
//  focus events cannot race each other.
// =====================================================================
#pragma once

#include <cstdint>

namespace island {

class ScrollTilingViewport {
public:
    void Reset(int offset);
    void SetTarget(int target, unsigned long long nowMs,
                   unsigned long long durationMs = 160);
    bool Advance(unsigned long long nowMs, int* offset);

    int Current() const { return current_; }
    int Target() const { return target_; }
    bool Animating(unsigned long long nowMs) const;

private:
    int current_ = 0;
    int target_ = 0;
    int from_ = 0;
    unsigned long long startedMs_ = 0;
    unsigned long long durationMs_ = 0;
};

} // namespace island
