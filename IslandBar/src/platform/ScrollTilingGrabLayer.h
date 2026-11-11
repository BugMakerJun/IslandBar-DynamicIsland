// =====================================================================
//  ScrollTilingGrabLayer.h -- temporary mouse surface for Win+Alt dragging.
//
//  The normal IslandBar window is only a strip at the top of the monitor and
//  cannot receive the desktop-wide mouse gesture. While the gesture modifier
//  is held, this no-activate window covers the current monitor, supplies the
//  hand cursor, and reports pointer input. It does not move application
//  windows or own any tiling policy.
// =====================================================================
#pragma once

#include <windows.h>

namespace island {

struct ScrollTilingGrabInput {
    bool started = false;
    bool moved = false;
    bool ended = false;
    bool cancelled = false;
    POINT position = {};
};

class ScrollTilingGrabLayer {
public:
    ScrollTilingGrabLayer() = default;
    ~ScrollTilingGrabLayer();
    ScrollTilingGrabLayer(const ScrollTilingGrabLayer&) = delete;
    ScrollTilingGrabLayer& operator=(const ScrollTilingGrabLayer&) = delete;

    bool Create(const RECT& monitorScreen);
    void Destroy();
    bool Show(const RECT& monitorScreen);
    void Hide();

    bool TakeInput(ScrollTilingGrabInput* input);
    bool CoversPointer(POINT screenPoint) const;
    bool Visible() const { return visible_; }
    HWND Handle() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void RememberPoint();
    void ClearInput();

    HWND hwnd_ = nullptr;
    bool visible_ = false;
    bool leftDown_ = false;
    bool trackingLeave_ = false;
    bool pendingStarted_ = false;
    bool pendingMoved_ = false;
    bool pendingEnded_ = false;
    bool pendingCancelled_ = false;
    bool havePendingPoint_ = false;
    POINT pendingPoint_ = {};
};

} // namespace island
