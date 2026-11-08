// =====================================================================
//  Geometry.h
//  Layout metrics and the one hit-test predicate the whole bar shares.
//
//  Everything layout-related is expressed in DIPs (96 dpi units). Physical
//  pixels appear only at the two boundaries: the Win32 monitor rects that
//  come in, and the window rect that goes out.
//
//  There is exactly ONE rounded-rectangle test in the project. The renderer
//  draws with it and WM_NCHITTEST decides with it, so the clickable region
//  can never drift away from the painted pill.
//
//  THIS HEADER DOES NOT INCLUDE <windows.h>, AND THAT IS LOAD-BEARING.
//
//  It used to, which meant every translation unit that wanted a rectangle -- the layout, the animation, the
//  self-test -- was compiled against the whole Win32 surface: HWND, MSG, the min/max macros, the lot. That is
//  what made the pure logic indistinguishable from the platform code in the first place. The two conversion
//  helpers below are the only thing here that ever needed a Win32 type, and they only needed it as a number:
//  LONG is a 32-bit integer and UINT is an unsigned one, so `int` and `unsigned` say the same thing without
//  dragging the platform in. Callers pass LONG/UINT values and they convert implicitly, unchanged.
//
//  core/PurityCheck.cpp turns this rule into a build failure rather than a convention: if <windows.h> ever
//  comes back into this file or anything it includes, that translation unit stops compiling.
// =====================================================================
#pragma once

#include <algorithm>
#include <cmath>

namespace island {

inline constexpr float kDipPerInch = 96.0f;

// --- Metric constants (DIPs) -----------------------------------------

// Window height. The window is a full-width strip whose top edge is the top of
// the work area; the pill is a smaller shape inside it. The window is created
// once at this size and NEVER resized afterwards, because resizing a
// DirectComposition swap-chain window flickers. Every state change moves the pill
// inside this fixed window.
//
// 240 is derived, not chosen: the tallest state is the 132 DIP card, and the
// window must also hold the 36 DIP resting pill. 200 was enough for the pill but
// clipped the card's bottom -- the self-test caught it.
// 260, not 240.
//
// The tallest thing the bar draws is the card, which is 250 DIP and hangs from the pill margin at
// y=6 -- so it reaches y=256. The window was 240, which fitted the 232 DIP card the music player
// used and nothing taller.
//
// Growing it is free: the window is a transparent strip over the desktop with its input region
// narrowed to what is drawn, so nothing below the card is visible or clickable, and the extra rows
// cost nothing per frame because the region clips them.
//
// 320, not 260.
//
// The outer cards -- the system panel and the notification -- now hang BELOW the bar instead of
// growing out of it, so they start at the pill's bottom (42) plus the gap (8) and are 280 tall: the
// card reaches y=330. At 260 the bottom 40 DIP of the panel were outside the window and could not be
// painted or clicked at all, which is the same clip as the one that made 260 necessary in the first
// place -- the arithmetic moved, so the constant has to move with it.
//
// 360, and the arithmetic that moved this time is one row: the card grew by 30 DIP for the brightness
// slider (Motion.h: kExpandedHeightDip 250 -> 280), so it ends at 330 and the window keeps the same
// 30 DIP of slack it had at 320. Growing the window is free -- nothing is drawn below the card, and
// the window is a transparent strip over the desktop whose input region is narrowed to what is drawn.
//
// 540, because the card is 520 x 500 now: it reaches y=506 and the same 30 DIP of slack puts the
// window's bottom edge at 540. The window is still never resized, so this is the one number that has to track
// the tallest card.
inline constexpr float kWindowHeightDip = 540.0f;

// The resting pill.
inline constexpr float kPillWidthDip    = 220.0f;
inline constexpr float kPillHeightDip   = 36.0f;
inline constexpr float kCornerRadiusDip = 18.0f;   // pill height / 2

// Distance from the top of the window strip down to the pill's top edge, i.e.
// the gap between the work-area edge and the resting pill.
inline constexpr float kPillTopMarginDip = 6.0f;

// How much of the screen's top the bar reserves for itself when it claims the strip as an AppBar.
//
// This is the margin PLUS the pill, and it has to be stated as its own constant rather than left for
// the caller to add up. The first version reserved only kPillTopMarginDip + kCollapsedHeightDip while
// the window's chrome is kPillTopMarginDip + kPillHeightDip high, so the reservation was 6 DIP short and
// a maximized window would tuck under the pill's bottom edge -- visible, but only once the bar was
// actually claiming space, which cannot be done from the development sandbox.
inline constexpr float kReservedBarHeightDip = kPillTopMarginDip + kPillHeightDip;

// Slack around the pill that still counts as a hit, so the pointer does not
// have to land on the exact curve.
inline constexpr float kHitToleranceDip = 2.0f;

// --- Conversions -----------------------------------------------------

inline float PixelsToDip(int pixels, unsigned dpi) {
    return static_cast<float>(pixels) * kDipPerInch / static_cast<float>(dpi ? dpi : 96);
}

inline int DipToPixels(float dip, unsigned dpi) {
    const float scaled = dip * static_cast<float>(dpi ? dpi : 96) / kDipPerInch;
    return static_cast<int>(std::lround(scaled));
}

// --- Geometry --------------------------------------------------------

struct RectF {
    float left   = 0.0f;
    float top    = 0.0f;
    float right  = 0.0f;
    float bottom = 0.0f;
};

inline float Width(const RectF& r)  { return r.right - r.left; }
inline float Height(const RectF& r) { return r.bottom - r.top; }

// A rounded rectangle whose corner radii are given per pair of corners.
// The pill is a pure "stadium": radius = half the short side, so left and
// right are semicircles. Keeping the two pairs separate is what lets the
// expanded bar later keep its corners tight while the collapsed pill stays
// fully round, using the same predicate.
struct RoundedBox {
    RectF rect;
    float radiusX = 0.0f;
    float radiusY = 0.0f;
};

// The pill, centred horizontally in the monitor and sitting just under the
// top of the work area.
//
// originX / originY are the window's top-left corner in DIPs within the
// monitor, so the return value is in window-local DIPs.
inline RoundedBox PillBox(float originX, float originY,
                          float monitorWidthDip, float workTopDip) {
    RoundedBox box;
    const float centerX = monitorWidthDip * 0.5f;
    const float top     = workTopDip + kPillTopMarginDip - originY;

    box.rect.left   = centerX - kPillWidthDip * 0.5f - originX;
    box.rect.top    = top;
    box.rect.right  = box.rect.left + kPillWidthDip;
    box.rect.bottom = box.rect.top + kPillHeightDip;
    box.radiusX     = kCornerRadiusDip;
    box.radiusY     = kCornerRadiusDip;
    return box;
}

// True when a point lies inside a rounded rectangle.
//
// The test splits into three horizontal bands. Above and below the corner
// radius the shape is a plain rectangle; the two bands that contain the
// corners are checked against the corner circles themselves, with a
// chamfer-style fallback for degenerate radii. In the corner circles the
// y offset is shared with the x offset term, which is why this is not a
// plain per-axis clamp.
//
//   y <= top + r      -> upper corner band: y term is (top + r - y)
//   y >= bottom - r   -> lower corner band: y term is (y - (bottom - r))
//   otherwise         -> straight middle: only the x term can fail
inline bool Contains(const RoundedBox& box, float x, float y) {
    const RectF& r = box.rect;
    if (x < r.left || x > r.right || y < r.top || y > r.bottom) {
        return false;
    }

    const float rx = std::min(box.radiusX, Width(r) * 0.5f);
    const float ry = std::min(box.radiusY, Height(r) * 0.5f);

    // Pick the corner circle this point may belong to, and the direction of
    // the y offset from that circle's centre.
    float dx = 0.0f;   // horizontal distance from the nearest vertical edge
    float dy = 0.0f;   // vertical distance from the nearest horizontal edge
    bool inCornerBand = false;

    if (x < r.left + rx) {
        dx = r.left + rx - x;
        inCornerBand = true;
    } else if (x > r.right - rx) {
        dx = x - (r.right - rx);
        inCornerBand = true;
    }

    if (y < r.top + ry) {
        dy = r.top + ry - y;
    } else if (y > r.bottom - ry) {
        dy = y - (r.bottom - ry);
    } else {
        // Straight middle band: the vertical extent is unbounded here.
        return !inCornerBand || dx <= rx;
    }

    if (!inCornerBand) {
        return true;
    }
    if (rx <= 0.0f || ry <= 0.0f) {
        return true;   // square corners
    }

    const float nx = dx / rx;
    const float ny = dy / ry;
    return (nx * nx + ny * ny) <= 1.0f;
}

// Hit test with the pointer slack applied.
inline bool ContainsWithTolerance(const RoundedBox& box, float x, float y, float tolerance) {
    RoundedBox grown = box;
    grown.rect.left   -= tolerance;
    grown.rect.top    -= tolerance;
    grown.rect.right  += tolerance;
    grown.rect.bottom += tolerance;
    grown.radiusX += tolerance;
    grown.radiusY += tolerance;
    return Contains(grown, x, y);
}

} // namespace island
