// =====================================================================
//  ScreenInk.h -- the luminance of a few patches of the screen, read off the frame loop
// =====================================================================
//
// WHY THIS EXISTS, with the measurement that put it here (2026-10-18): the island chooses its words'
// ink from the brightness of what is behind it, and that reading was taken with GDI BitBlt on the
// frame loop -- four corner patches for each of the bar's four sample areas, and four more for the
// card. One BitBlt from the desktop DC costs about 6 ms on this machine (2560x1440, 96 DPI), so the
// frame paid ~120 ms PER FRAME for it. Every animation in the bar ran at 8 fps. Measured with
// ISLANDBAR_TRACE_PHASES: `render-phase: ink-sample 95 ms`, `card+strip 23 ms`, and `slow frame: 121 ms`
// on every single frame, whether anything was moving or not.
//
// The number does not need that cadence. It decides whether the text takes the theme's dark ink or its
// highlight, and that decision is already smoothed over a quarter of a second (SetPillInkBackdrop), so
// a reading every 200 ms is indistinguishable -- while the frame loop pays nothing but a comparison.
//
// The thread is not a polling loop: it waits on its own stop event with a timeout, does ONE readback
// for the union of every patch it was asked for, and goes back to waiting.

#pragma once

#include <windows.h>

namespace island {

class ScreenInk {
public:
    // The most patches one reading can carry: sixteen for the bar (four areas x four corners) and four
    // for the card.
    static constexpr int kMaxRects = 32;

    ScreenInk() = default;
    ~ScreenInk();
    ScreenInk(const ScreenInk&) = delete;
    ScreenInk& operator=(const ScreenInk&) = delete;

    // Which rectangles to read, in screen pixels. Posted every frame; a list that has not moved costs a
    // comparison and nothing else. Reading starts on the first request and stops with the object.
    void Request(const RECT* rects, int count);

    // The mean grey of rectangle `index` from the last reading, 0..1, or -1 when that rectangle has not
    // been read (yet, or at all -- an off-screen patch answers -1 too).
    float Grey(int index) const;

private:
    struct State;
    State* state_ = nullptr;

    bool Start();
    static unsigned long __stdcall ThreadEntry(void* self);
    void Run();
};

}  // namespace island
