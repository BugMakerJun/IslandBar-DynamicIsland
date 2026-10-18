// =====================================================================
//  LensProfile.h -- where one pixel of the lens samples the backdrop.
//
//  THE LENS IS THE MATERIAL (2026-10-18). The glass theme's surface is the window behind the island,
//  captured by platform/BackdropCapture and bent by render/LensMaterial. This header is the bend, as a
//  pure function: no D2D, no Win32, no captured frame. The self-test can therefore assert its shape
//  without a device or a screenshot.
//
//  THE SHAPE, from the reference the user chose (shuding/liquid-glass, MIT):
//
//    * a FLAT CENTRE -- the date and the time sit on the backdrop as it is, not on a warped one;
//    * a NARROW BAND at the edge that samples a little further IN, so the surface reads as a thick lens
//      rather than as a hole with a bent rim;
//    * the ROUND ENDS bend more than the straight top and bottom (the reference's corners do the same),
//      which is what `|nx|` weights below.
//
//  WHY THESE NUMBERS. The reference leaves the middle 57.5% of its 200 px height alone and moves the
//  edge by 3-8% of its size. A 36 DIP pill cannot carry a 43 DIP band: a 12-18 DIP band keeps 12 DIP of
//  flat centre and still bends 5 DIP at the top and 11 DIP at the ends -- enough to see the glass on a
//  detailed desktop. The shift is kept below ~2/3 of the band, because the sample is `pixel + shift`: a
//  shift that falls faster than the band would make the mapping fold, and the image would mirror at the
//  rim. The self-test asserts the flat centre, the monotonic mapping and the DPI scaling.
// =====================================================================
#pragma once

#include <algorithm>
#include <cmath>

namespace island {

struct LensOffset {
    float dx = 0.0f;
    float dy = 0.0f;
    // How far the pixel is from the pill's edge, in physical pixels. The renderer uses it to feather the
    // lens's own alpha into the wallpaper over the last DIP; a hard bitmap edge is the seam the user saw.
    float inside = 0.0f;
};

// In DIP; `scale` converts them to physical pixels at the current DPI. The band grows on the ends,
// where the surface is more curved and the reference bends hardest.
inline constexpr float kLensBandBaseDip = 12.0f;
inline constexpr float kLensBandEndDip = 18.0f;
inline constexpr float kLensShiftTopDip = 5.0f;
inline constexpr float kLensShiftEndDip = 11.0f;

// `px`/`py` are the pixel's centre inside the pill, in physical pixels (0..pillW, 0..pillH). The result
// is how far the sample moves from that pixel, in physical pixels, along the INWARD normal: the top
// edge samples down, the left end samples right. It is zero in the flat centre and never outward.
inline LensOffset LensOffsetFor(float px, float py, float pillW, float pillH, float scale) {
    const float radius = pillH * 0.5f;
    const float cxL = radius;
    const float cxR = pillW - radius;
    const float cy = radius;

    float inside = 0.0f;   // distance from the edge: 0 at the boundary, radius at the middle line
    float nx = 0.0f;       // inward normal
    float ny = 0.0f;
    if (px >= cxL && px <= cxR) {
        // The straight top and bottom. The normal flips at the middle line, but the shift is zero
        // there (the band ends 10 DIP before it), so the flip cannot make a seam.
        if (py < cy) {
            inside = py;
            ny = 1.0f;
        } else {
            inside = pillH - py;
            ny = -1.0f;
        }
    } else {
        // A round end: the inward normal points at the cap's own centre.
        const float centreX = (px < cxL) ? cxL : cxR;
        const float dx = centreX - px;
        const float dy = cy - py;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length > 0.0001f) {
            nx = dx / length;
            ny = dy / length;
        }
        inside = radius - length;
    }

    const float end = std::fabs(nx);
    const float band =
        (kLensBandBaseDip + (kLensBandEndDip - kLensBandBaseDip) * end) * scale;
    const float maxShift =
        (kLensShiftTopDip + (kLensShiftEndDip - kLensShiftTopDip) * end) * scale;
    const float t = std::clamp(inside / band, 0.0f, 1.0f);
    const float w = 1.0f - t;
    // Smoothstep, so the mapping leaves and re-enters 1:1: no crease where the band meets the flat
    // centre, and no fold at the edge (the peak slope is 1.5 * maxShift / band < 1).
    const float weight = w * w * (3.0f - 2.0f * w);
    const float shift = maxShift * weight;
    return {nx * shift, ny * shift, inside};
}

}  // namespace island
