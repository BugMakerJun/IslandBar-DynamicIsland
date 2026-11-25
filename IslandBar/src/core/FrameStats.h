// =====================================================================
//  FrameStats.h -- the frame clock and the bounded timing windows
//
//  Every number used to judge frame pacing comes from QueryPerformanceCounter.
//  The windows are fixed-size on purpose: this code runs in the animation hot
//  path and must not allocate, grow, or retain an unbounded history.
// =====================================================================
#pragma once

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cwchar>

namespace island {

inline constexpr size_t kTimingSampleCapacity = 512;
inline constexpr size_t kTimingBucketCount = 12;
inline constexpr double kBlockingWaitThresholdMs = 0.1;

struct TimingSummary {
    long long samples = 0;
    double minimumMs = 0.0;
    double meanMs = 0.0;
    double p50Ms = 0.0;
    double p95Ms = 0.0;
    double p99Ms = 0.0;
    double maximumMs = 0.0;
};

inline size_t TimingBucketFor(double milliseconds) noexcept {
    if (milliseconds < 4.0) return 0;
    if (milliseconds < 5.0) return 1;
    if (milliseconds < 6.0) return 2;
    if (milliseconds < 7.0) return 3;
    if (milliseconds < 8.0) return 4;
    if (milliseconds < 10.0) return 5;
    if (milliseconds < 12.0) return 6;
    if (milliseconds < 16.0) return 7;
    if (milliseconds < 24.0) return 8;
    if (milliseconds < 33.0) return 9;
    if (milliseconds < 50.0) return 10;
    return 11;
}

inline bool TimingCountsAsBlocking(double milliseconds) noexcept {
    return std::isfinite(milliseconds) && milliseconds >= kBlockingWaitThresholdMs;
}

// A fixed-capacity ring with nearest-rank percentiles. Summarize() takes a
// stack copy only when a report is emitted, never while recording a frame.
class TimingSeries {
public:
    void Add(double milliseconds) noexcept {
        if (!std::isfinite(milliseconds) || milliseconds < 0.0) return;
        if (sampleCount_ == values_.size()) {
            --buckets_[TimingBucketFor(values_[next_])];
        }
        values_[next_] = milliseconds;
        next_ = (next_ + 1) % values_.size();
        if (sampleCount_ < values_.size()) ++sampleCount_;
        ++buckets_[TimingBucketFor(milliseconds)];
    }

    void Reset() noexcept {
        values_.fill(0.0);
        buckets_.fill(0);
        next_ = 0;
        sampleCount_ = 0;
    }

    long long SampleCount() const noexcept { return static_cast<long long>(sampleCount_); }

    const std::array<int, kTimingBucketCount>& Buckets() const noexcept { return buckets_; }

    long long BucketCountSum() const noexcept {
        long long total = 0;
        for (int count : buckets_) total += count;
        return total;
    }

    TimingSummary Summarize() const noexcept {
        TimingSummary summary;
        summary.samples = static_cast<long long>(sampleCount_);
        if (sampleCount_ == 0) return summary;

        std::array<double, kTimingSampleCapacity> sorted = values_;
        std::sort(sorted.begin(), sorted.begin() + sampleCount_);

        double total = 0.0;
        for (size_t i = 0; i < sampleCount_; ++i) total += sorted[i];

        summary.minimumMs = sorted[0];
        summary.meanMs = total / static_cast<double>(sampleCount_);
        summary.p50Ms = sorted[PercentileIndex(50.0)];
        summary.p95Ms = sorted[PercentileIndex(95.0)];
        summary.p99Ms = sorted[PercentileIndex(99.0)];
        summary.maximumMs = sorted[sampleCount_ - 1];
        return summary;
    }

private:
    size_t PercentileIndex(double percentile) const noexcept {
        const double rank = std::ceil(percentile * static_cast<double>(sampleCount_) / 100.0);
        const size_t index = rank <= 1.0 ? 0 : static_cast<size_t>(rank - 1.0);
        return std::min(index, sampleCount_ - 1);
    }

    std::array<double, kTimingSampleCapacity> values_{};
    std::array<int, kTimingBucketCount> buckets_{};
    size_t next_ = 0;
    size_t sampleCount_ = 0;
};

struct FrameStats {
    LARGE_INTEGER frequency = {};
    LARGE_INTEGER last = {};
    LARGE_INTEGER lastDrawn = {};
    bool haveLastDrawn = false;
    bool started = false;

    static constexpr double kHitchMs = 50.0;
    static constexpr double kSlowFrameMs = 40.0;

    long long count = 0;
    long long drawnIntervals = 0;
    double totalMs = 0.0;
    double totalDrawnMs = 0.0;
    double worstMs = 0.0;
    int hitches = 0;
    double idleWorstMs = 0.0;
    long long idleCount = 0;

    TimingSeries intervals;
    TimingSeries animationIntervals;
    TimingSeries fullscreenIntervals;

    void Start() {
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&last);
        lastDrawn = {};
        haveLastDrawn = false;
        started = true;
    }

    double RecordAtTicks(LONGLONG nowTicks, bool drew, bool animating = false,
                         bool fullscreen = false) {
        if (!started || frequency.QuadPart <= 0) return 0.0;
        LARGE_INTEGER now = {};
        now.QuadPart = nowTicks;
        const double passMs = static_cast<double>(now.QuadPart - last.QuadPart) * 1000.0 /
                              static_cast<double>(frequency.QuadPart);
        last = now;

        ++count;
        totalMs += passMs;
        if (drew) {
            const bool timedSequence = animating || fullscreen;
            if (timedSequence && haveLastDrawn) {
                const double ms = static_cast<double>(now.QuadPart - lastDrawn.QuadPart) * 1000.0 /
                                  static_cast<double>(frequency.QuadPart);
                ++drawnIntervals;
                totalDrawnMs += ms;
                if (ms > worstMs) worstMs = ms;
                if (ms > kHitchMs) ++hitches;
                intervals.Add(ms);
                if (animating) animationIntervals.Add(ms);
                if (fullscreen) fullscreenIntervals.Add(ms);
            }
            if (timedSequence) {
                lastDrawn = now;
                haveLastDrawn = true;
            } else {
                // A one-off clock/notification draw is not the next sample of
                // an animation; the interval after it includes idle time.
                haveLastDrawn = false;
            }
        } else {
            ++idleCount;
            if (passMs > idleWorstMs) idleWorstMs = passMs;
            haveLastDrawn = false;
        }
        return passMs;
    }

    double Record(bool drew, bool animating = false, bool fullscreen = false) {
        LARGE_INTEGER now = {};
        QueryPerformanceCounter(&now);
        return RecordAtTicks(now.QuadPart, drew, animating, fullscreen);
    }

    void Reset() {
        count = 0;
        drawnIntervals = 0;
        totalMs = 0.0;
        totalDrawnMs = 0.0;
        worstMs = 0.0;
        hitches = 0;
        idleWorstMs = 0.0;
        idleCount = 0;
        intervals.Reset();
        animationIntervals.Reset();
        fullscreenIntervals.Reset();
        Start();
    }

    void Describe(wchar_t* out, size_t capacity) const {
        if (!out || capacity == 0) return;
        const long long drawn = count - idleCount;
        const TimingSummary timing = intervals.Summarize();
        const double lifetimeMean =
            drawnIntervals > 0 ? totalDrawnMs / static_cast<double>(drawnIntervals) : 0.0;
        std::swprintf(
            out, capacity,
            L"drawn %lld: p50 %.1f p95 %.1f p99 %.1f mean %.1f max %.1f ms | "
            L"lifetime avg %.1f worst %.1f hitches %d | idle %lld pass(es), worst wait %.1f ms",
            drawn, timing.p50Ms, timing.p95Ms, timing.p99Ms, timing.meanMs, timing.maximumMs,
            lifetimeMean, worstMs, hitches, idleCount, idleWorstMs);
    }

    void DescribeAnimation(wchar_t* out, size_t capacity) const {
        if (!out || capacity == 0) return;
        const TimingSummary timing = animationIntervals.Summarize();
        std::swprintf(out, capacity,
                      L"samples %lld: p50 %.1f p95 %.1f p99 %.1f mean %.1f max %.1f ms",
                      timing.samples, timing.p50Ms, timing.p95Ms, timing.p99Ms, timing.meanMs,
                      timing.maximumMs);
    }

    void DescribeFullscreen(wchar_t* out, size_t capacity) const {
        if (!out || capacity == 0) return;
        const TimingSummary timing = fullscreenIntervals.Summarize();
        std::swprintf(out, capacity,
                      L"samples %lld: p50 %.1f p95 %.1f p99 %.1f mean %.1f max %.1f ms",
                      timing.samples, timing.p50Ms, timing.p95Ms, timing.p99Ms, timing.meanMs,
                      timing.maximumMs);
    }

    void DescribeBuckets(wchar_t* out, size_t capacity) const {
        if (!out || capacity == 0) return;
        const auto& b = intervals.Buckets();
        std::swprintf(
            out, capacity,
            L"<4:%d 4-5:%d 5-6:%d 6-7:%d 7-8:%d 8-10:%d 10-12:%d "
            L"12-16:%d 16-24:%d 24-33:%d 33-50:%d 50+:%d",
            b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11]);
    }
};

} // namespace island
