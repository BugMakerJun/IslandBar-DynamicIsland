// =====================================================================
//  ScreenInk.cpp
// =====================================================================
#include "platform/ScreenInk.h"

#include <algorithm>
#include <atomic>
#include <new>

#include "platform/CriticalSection.h"
#include "platform/WorkerThread.h"

namespace island {
namespace {

// How often the screen is read. The values this feeds change on a human timescale and the ink itself is
// smoothed over a quarter of a second, so this is a floor on the lag between "the window behind went
// dark" and "the words turn white", not a frame rate.
constexpr unsigned long kSampleIntervalMs = 200;

struct InjectedFailures {
    std::atomic<int> getDc{0};
    std::atomic<int> memoryDc{0};
    std::atomic<int> dib{0};
    std::atomic<int> select{0};
    std::atomic<unsigned long long> hit{0};
};

InjectedFailures g_injected;
std::atomic<unsigned long long> g_gdiAcquisitions{0};
std::atomic<unsigned long long> g_gdiReleases{0};
std::atomic<unsigned long long> g_gdiThreadMismatches{0};
std::atomic<unsigned long long> g_gdiReleaseFailures{0};
std::atomic<DWORD> g_gdiAcquireThread{0};

bool TakeInjectedFailure(std::atomic<int>& remaining) {
    int count = remaining.load(std::memory_order_acquire);
    while (count > 0) {
        if (remaining.compare_exchange_weak(count, count - 1, std::memory_order_acq_rel)) {
            g_injected.hit.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

// The screen rectangle a BitBlt is allowed to reach. Patches are clamped to it rather than refused:
// the bar sits on the screen's top edge, so its own top strip is partly outside on a maximised window.
void ScreenBounds(RECT* bounds) {
    bounds->left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    bounds->top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    bounds->right = bounds->left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    bounds->bottom = bounds->top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

}  // namespace

struct ScreenInk::State {
    WorkerThread worker;
    CriticalSection lock;
    std::atomic<bool> paused{false};

    // Posted by the frame loop, read by the worker.
    RECT rects[kMaxRects] = {};
    int count = 0;

    // Published by the worker, read by the frame loop. -1 is "not read".
    float grey[kMaxRects];

    // The worker's own GDI surface, made once and reused: a screen DC, a compatible DC and a DIB big
    // enough for the union of the patches it was last asked for.
    HDC screenDc = nullptr;
    HDC memoryDc = nullptr;
    HBITMAP dib = nullptr;
    void* pixels = nullptr;
    int dibWidth = 0;
    int dibHeight = 0;

    State() {
        for (float& value : grey) value = -1.0f;
    }

    ~State() {
        // GDI resources are released by ThreadEntry on the thread that acquired screenDc.
    }
};

void ScreenInk::ReleaseGdiSurface(State& state) {
    if (state.memoryDc) {
        DeleteDC(state.memoryDc);
        state.memoryDc = nullptr;
    }
    if (state.dib) {
        DeleteObject(state.dib);
        state.dib = nullptr;
    }
    if (state.screenDc) {
        const DWORD acquireThread = g_gdiAcquireThread.load(std::memory_order_acquire);
        if (acquireThread != 0 && acquireThread != GetCurrentThreadId()) {
            g_gdiThreadMismatches.fetch_add(1, std::memory_order_relaxed);
        }
        if (!ReleaseDC(nullptr, state.screenDc)) {
            g_gdiReleaseFailures.fetch_add(1, std::memory_order_relaxed);
        }
        g_gdiReleases.fetch_add(1, std::memory_order_relaxed);
        state.screenDc = nullptr;
    }
    state.pixels = nullptr;
    state.dibWidth = 0;
    state.dibHeight = 0;
}

ScreenInk::~ScreenInk() {
    if (state_) {
        if (state_->worker.Stop() != WorkerThread::StopResult::Stopped) {
            LogLine(L"  ink: worker did not stop; retained by its shared owner");
        }
        state_.reset();
    }
}

bool ScreenInk::Start() {
    if (state_ && state_->worker.Started()) return true;
    state_.reset();
    auto state = std::make_shared<State>();
    if (!state->worker.Start(&ScreenInk::ThreadEntry, state)) {
        state->worker.Stop();
        return false;
    }
    state_ = std::move(state);
    return true;
}

void ScreenInk::Request(const RECT* rects, int count) {
    if (state_ && state_->paused.load(std::memory_order_relaxed)) return;
    if (!rects || count <= 0) return;
    if (!Start()) return;
    count = std::min(count, kMaxRects);

    CriticalSection::Guard guard(state_->lock);
    bool same = (count == state_->count);
    for (int i = 0; same && i < count; ++i) {
        same = rects[i].left == state_->rects[i].left &&
               rects[i].top == state_->rects[i].top &&
               rects[i].right == state_->rects[i].right &&
               rects[i].bottom == state_->rects[i].bottom;
    }
    if (same) return;
    for (int i = 0; i < count; ++i) state_->rects[i] = rects[i];
    state_->count = count;
}

void ScreenInk::SetPaused(bool paused) {
    if (state_) state_->paused.store(paused, std::memory_order_relaxed);
}

float ScreenInk::Grey(int index) const {
    if (!state_ || index < 0 || index >= kMaxRects) return -1.0f;
    CriticalSection::Guard guard(state_->lock);
    return state_->grey[index];
}

unsigned long __stdcall ScreenInk::ThreadEntry(void* self) {
    State& state = *static_cast<State*>(self);
    struct SurfaceGuard {
        State& state;
        ~SurfaceGuard() { ReleaseGdiSurface(state); }
    } guard{state};
    try {
        Run(state);
    } catch (...) {
        LogLine(L"  ink: worker threw an exception; GDI cleanup continues on the worker thread");
    }
    return 0;
}

void ScreenInk::Run(State& state) {
    const WorkerThread::Signal workerSignal = WorkerThread::CurrentSignal();
    const HANDLE stopEvent = workerSignal.StopEvent();

    // The first reading is taken at once rather than after a full interval: the bar draws its first
    // frame before the thread would otherwise wake, and until then the ink falls back to the lens.
    while (true) {
        if (state.paused.load(std::memory_order_relaxed)) {
            if (WaitForSingleObject(stopEvent, kSampleIntervalMs) != WAIT_TIMEOUT) break;
            continue;
        }

        RECT rects[kMaxRects] = {};
        int count = 0;
        {
            CriticalSection::Guard guard(state.lock);
            count = state.count;
            for (int i = 0; i < count; ++i) rects[i] = state.rects[i];
        }

        float grey[kMaxRects];
        for (float& value : grey) value = -1.0f;

        if (count > 0) {
            RECT bounds = {};
            ScreenBounds(&bounds);
            // ONE READBACK FOR EVERY PATCH. Clamping and intersecting here, rather than per BitBlt,
            // is what makes a single blit enough: the union of twenty small patches is one band.
            RECT cover = {bounds.right, bounds.bottom, bounds.left, bounds.top};
            bool any = false;
            for (int i = 0; i < count; ++i) {
                RECT patch = rects[i];
                patch.left = std::max(patch.left, bounds.left);
                patch.top = std::max(patch.top, bounds.top);
                patch.right = std::min(patch.right, bounds.right);
                patch.bottom = std::min(patch.bottom, bounds.bottom);
                if (patch.right - patch.left < 1 || patch.bottom - patch.top < 1) continue;
                cover.left = std::min(cover.left, patch.left);
                cover.top = std::min(cover.top, patch.top);
                cover.right = std::max(cover.right, patch.right);
                cover.bottom = std::max(cover.bottom, patch.bottom);
                any = true;
            }
            if (any) {
                const int width = cover.right - cover.left;
                const int height = cover.bottom - cover.top;
                if (!state.screenDc && !TakeInjectedFailure(g_injected.getDc)) {
                    state.screenDc = GetDC(nullptr);
                    if (state.screenDc) {
                        g_gdiAcquireThread.store(GetCurrentThreadId(), std::memory_order_release);
                        g_gdiAcquisitions.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                if (state.screenDc && !state.memoryDc &&
                    !TakeInjectedFailure(g_injected.memoryDc)) {
                    state.memoryDc = CreateCompatibleDC(state.screenDc);
                }
                bool surfaceReady =
                    state.memoryDc && state.dib && state.pixels &&
                    ScreenInkSurfaceFits(state.dibWidth, state.dibHeight, width, height);
                const bool resized = !surfaceReady;
                if (state.screenDc && state.memoryDc && resized) {
                    bool selected = false;
                    BITMAPINFO info = {};
                    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                    info.bmiHeader.biWidth = width;
                    info.bmiHeader.biHeight = -height;   // top-down: row n of the DIB is screen y + n
                    info.bmiHeader.biPlanes = 1;
                    info.bmiHeader.biBitCount = 32;
                    info.bmiHeader.biCompression = BI_RGB;
                    void* bits = nullptr;
                    HBITMAP dib = TakeInjectedFailure(g_injected.dib)
                                      ? nullptr
                                      : CreateDIBSection(state.screenDc, &info, DIB_RGB_COLORS,
                                                         &bits, nullptr, 0);
                    if (dib && bits) {
                        const HGDIOBJ previous =
                            TakeInjectedFailure(g_injected.select)
                                ? nullptr
                                : SelectObject(state.memoryDc, dib);
                        if (!previous || previous == HGDI_ERROR) {
                            DeleteObject(dib);
                        } else {
                            // Once the new bitmap is selected, the old one is no longer referenced by the DC.
                            if (state.dib) DeleteObject(state.dib);
                            state.dib = dib;
                            state.pixels = bits;
                            state.dibWidth = width;
                            state.dibHeight = height;
                            selected = true;
                        }
                    } else if (dib) {
                        DeleteObject(dib);
                    }
                    surfaceReady = selected;
                }
                if (surfaceReady && state.dib && state.pixels &&
                    BitBlt(state.memoryDc, 0, 0, width, height, state.screenDc, cover.left,
                           cover.top, SRCCOPY)) {
                    const auto* bytes = static_cast<const unsigned char*>(state.pixels);
                    for (int i = 0; i < count; ++i) {
                        RECT patch = rects[i];
                        patch.left = std::max(patch.left, cover.left);
                        patch.top = std::max(patch.top, cover.top);
                        patch.right = std::min(patch.right, cover.right);
                        patch.bottom = std::min(patch.bottom, cover.bottom);
                        if (patch.right - patch.left < 1 || patch.bottom - patch.top < 1) continue;
                        double sum = 0.0;
                        int pixels = 0;
                        for (LONG y = patch.top; y < patch.bottom; ++y) {
                            const unsigned char* row =
                                bytes + static_cast<size_t>(y - cover.top) * state.dibWidth * 4;
                            for (LONG x = patch.left; x < patch.right; ++x) {
                                const unsigned char* p = row + static_cast<size_t>(x - cover.left) * 4;
                                sum += 0.0722 * p[0] + 0.7152 * p[1] + 0.2126 * p[2];
                                ++pixels;
                            }
                        }
                        if (pixels > 0) grey[i] = static_cast<float>(sum / pixels / 255.0);
                    }
                }
            }
        }

        {
            CriticalSection::Guard guard(state.lock);
            // Published whole, not merged: a patch that has gone off-screen (or is too small to read
            // where the shape now is) has to answer -1 rather than keep the brightness it had before
            // it moved, which is what a merge would do.
            for (int i = 0; i < kMaxRects; ++i) state.grey[i] = grey[i];
        }

        if (WaitForSingleObject(stopEvent, kSampleIntervalMs) != WAIT_TIMEOUT) break;
    }
}

void ConfigureScreenInkGdiFailuresForTest(int getDcFailures, int memoryDcFailures,
                                          int dibFailures, int selectFailures) {
    g_injected.getDc.store(std::max(0, getDcFailures), std::memory_order_release);
    g_injected.memoryDc.store(std::max(0, memoryDcFailures), std::memory_order_release);
    g_injected.dib.store(std::max(0, dibFailures), std::memory_order_release);
    g_injected.select.store(std::max(0, selectFailures), std::memory_order_release);
}

ScreenInkGdiStatsForTest ScreenInkGdiDiagnosticsForTest() {
    ScreenInkGdiStatsForTest diagnostics;
    diagnostics.acquisitions = g_gdiAcquisitions.load(std::memory_order_acquire);
    diagnostics.releases = g_gdiReleases.load(std::memory_order_acquire);
    diagnostics.threadMismatches = g_gdiThreadMismatches.load(std::memory_order_acquire);
    diagnostics.releaseFailures = g_gdiReleaseFailures.load(std::memory_order_acquire);
    diagnostics.injectedFailuresHit = g_injected.hit.load(std::memory_order_acquire);
    return diagnostics;
}

void ResetScreenInkGdiDiagnosticsForTest() {
    ConfigureScreenInkGdiFailuresForTest(0, 0, 0, 0);
    g_injected.hit.store(0, std::memory_order_release);
    g_gdiAcquisitions.store(0, std::memory_order_release);
    g_gdiReleases.store(0, std::memory_order_release);
    g_gdiThreadMismatches.store(0, std::memory_order_release);
    g_gdiReleaseFailures.store(0, std::memory_order_release);
    g_gdiAcquireThread.store(0, std::memory_order_release);
}

}  // namespace island
