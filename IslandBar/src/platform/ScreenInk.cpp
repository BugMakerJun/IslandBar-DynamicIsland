// =====================================================================
//  ScreenInk.cpp
// =====================================================================
#include "platform/ScreenInk.h"

#include <algorithm>
#include <new>

#include "platform/CriticalSection.h"
#include "platform/WorkerThread.h"

namespace island {
namespace {

// How often the screen is read. The values this feeds change on a human timescale and the ink itself is
// smoothed over a quarter of a second, so this is a floor on the lag between "the window behind went
// dark" and "the words turn white", not a frame rate.
constexpr unsigned long kSampleIntervalMs = 200;

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
        if (dib) DeleteObject(dib);
        if (memoryDc) DeleteDC(memoryDc);
        if (screenDc) ReleaseDC(nullptr, screenDc);
    }
};

ScreenInk::~ScreenInk() {
    if (state_) {
        state_->worker.Stop();
        delete state_;
        state_ = nullptr;
    }
}

bool ScreenInk::Start() {
    if (state_) return true;
    state_ = new (std::nothrow) State();
    if (!state_) return false;
    if (!state_->worker.Prepare()) {
        delete state_;
        state_ = nullptr;
        return false;
    }
    DWORD threadId = 0;
    HANDLE thread = CreateThread(nullptr, 0, &ScreenInk::ThreadEntry, this, 0, &threadId);
    if (!thread) {
        state_->worker.Stop();
        delete state_;
        state_ = nullptr;
        return false;
    }
    state_->worker.Attach(thread);
    return true;
}

void ScreenInk::Request(const RECT* rects, int count) {
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

float ScreenInk::Grey(int index) const {
    if (!state_ || index < 0 || index >= kMaxRects) return -1.0f;
    CriticalSection::Guard guard(state_->lock);
    return state_->grey[index];
}

unsigned long __stdcall ScreenInk::ThreadEntry(void* self) {
    static_cast<ScreenInk*>(self)->Run();
    return 0;
}

void ScreenInk::Run() {
    State& state = *state_;

    // The first reading is taken at once rather than after a full interval: the bar draws its first
    // frame before the thread would otherwise wake, and until then the ink falls back to the lens.
    while (true) {
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
                if (!state.screenDc) {
                    state.screenDc = GetDC(nullptr);
                    if (state.screenDc) state.memoryDc = CreateCompatibleDC(state.screenDc);
                }
                const bool resized =
                    !state.dib || width > state.dibWidth || height > state.dibHeight;
                if (state.screenDc && state.memoryDc && resized) {
                    BITMAPINFO info = {};
                    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                    info.bmiHeader.biWidth = width;
                    info.bmiHeader.biHeight = -height;   // top-down: row n of the DIB is screen y + n
                    info.bmiHeader.biPlanes = 1;
                    info.bmiHeader.biBitCount = 32;
                    info.bmiHeader.biCompression = BI_RGB;
                    void* bits = nullptr;
                    HBITMAP dib = CreateDIBSection(state.screenDc, &info, DIB_RGB_COLORS, &bits,
                                                   nullptr, 0);
                    if (dib && bits) {
                        if (state.dib) DeleteObject(state.dib);
                        state.dib = dib;
                        state.pixels = bits;
                        state.dibWidth = width;
                        state.dibHeight = height;
                        SelectObject(state.memoryDc, state.dib);
                    } else if (dib) {
                        DeleteObject(dib);
                    }
                }
                if (state.dib && state.pixels &&
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

        if (WaitForSingleObject(state.worker.StopEvent(), kSampleIntervalMs) != WAIT_TIMEOUT) break;
    }
}

}  // namespace island
