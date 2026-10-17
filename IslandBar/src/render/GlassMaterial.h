// =====================================================================
//  GlassMaterial.h
//  The two layers of "glass" that are neither colour nor edge: the soft shadow under the shape and the
//  faint grain over it.
//
//  WHY A MODULE. The review brief this answers asks for a material built from layers -- blur, gradient,
//  noise, highlight, edge, shadow -- and the bar already has the gradient, the highlight and the edge
//  (they are what Theme.h tunes; the blur is measured as unavailable, see Theme.h and HANDOFF 8.91). The
//  two that were missing are here rather than inside the window or the renderer, because both are drawn
//  identically for the pill AND for every card, and one copy of "what a soft shadow is" is the same rule
//  the surfaces already follow.
//
//  WHAT IS DELIBERATELY NOT HERE: any kind of CPU blur, screenshot, or GDI path. The brief forbids those
//  and this project does too -- everything below is Direct2D primitives on the GPU.
//
//  WHY IT LIVES IN render/ (2026-10-16): it is drawing -- Direct2D primitives for a surface -- and it
//  already depends on render/Theme.h for the constants it uses. It sat in platform/ for a while, and that
//  single include was the ONLY back edge in the layer graph (platform -> render while render -> platform),
//  which is what kept the layers from being locked with static libraries. See ARCHITECTURE.md section 7.
// =====================================================================
#pragma once

#include <windows.h>

#include <d2d1_3.h>

#include "render/Theme.h"

namespace island {

// THE SMOKED BASE: the two-stop fill the surface is made of, plus the hairline in the top colour that
// covers the antialiased fringe along the curve. The two colours come from the caller because they are the
// theme's pill colours MIXED WITH THE WALLPAPER (TintedFill), which the renderer already owns.
//
// Its gradient brush is cached (device + colours), so an animated shape costs a transform rather than a
// stop collection and a brush every frame.
void DrawSmokedBase(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape, const Theme& theme,
                    const D2D1_COLOR_F& top, const D2D1_COLOR_F& bottom, float opacity);

// THE SMOKED DEPTH: the centre of the glass is heavier than its rim -- a blue-black ellipse that fades out
// before the edge. `theme.rim` drives it, so a theme with no glass gets none of it.
void DrawSmokedDepth(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape, const Theme& theme,
                     float opacity);

// THE LIGHTING A GLASS SURFACE GETS FROM THE ROOM: a highlight along the top, a shade along the bottom, and
// a hairline just inside the edge.
//
// ONE FUNCTION FOR THE PILL AND FOR EVERY CARD, which is the same rule the surfaces themselves follow: the
// bar and its cards cannot end up lit differently, and there is one place to change how the material
// behaves. Both gradients are filled over the SHAPE's own rounded rectangle, so they follow its curve --
// an inner line drawn as a plain rectangle pokes out at the rounded ends, which is the trap this avoids.
//
// One theme field drives all three (`theme.gloss`): light from above and dark from below are the same
// amount of lighting, and a theme that asks for no gloss gets a flat surface, as 默认 does.
void DrawSurfaceLighting(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape,
                         const Theme& theme, float opacity);

// THE TOP SHEEN: the reflection along the top is a few overlapping soft blobs, not a band across the whole
// shape -- a band is the "整条白线" the brief forbids. `pointerFraction` only shifts them a little, so a
// hovering pointer adds a little life without a decorative animation at rest.
void DrawTopSheen(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape, const Theme& theme,
                  float pointerFraction, float opacity);
// One-DIP specular contour: the lit edge of the glass, brighter at the top and again at the bottom.
// Distinct from DrawGlassRim below, which is the old thick double edge.
void DrawThinRim(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape, const Theme& theme,
                 float opacity);

// THE EDGE REFRACTION: a very weak cool-blue blob at the left end and a violet-blue one at the right, kept
// away from the middle where the text lives. Driven by `theme.tint`.
void DrawEdgeTints(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape, const Theme& theme,
                   float opacity);

// A soft shadow: nested rounded rectangles, each inflated a little and drawn fainter, which is a shadow
// with no intermediate surface, no effect graph and no allocation. `spreadDip` is how far it reaches.
//
// It is not a Gaussian and does not claim to be: a real one needs an offscreen bitmap and a blur pass,
// and at the size of these shapes (a 36 DIP pill, a 280 DIP card) the stepped version is what the eye
// sees anyway. What it must not be is a hard edge, so the steps overlap rather than tile.
void DrawSoftShadow(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape, float opacity,
                    float spreadDip);

// The grain over a surface, so it does not read as flat plastic. Clipped to the shape by a layer mask.
void DrawGrain(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape, float opacity);

// --- the two liquid-glass layers (round 53) ---------------------------------------------------------

// Where the specular sweep's centre sits, given the pointer's position across the pill (0 = its left
// edge, 1 = its right). PURE, so the self-test can assert the clamping rather than the drawing: a pointer
// at or past an edge must not push the highlight off the glass and leave a bright spot on the corner.
//
// The highlight travels across the INNER 80% of the width. That margin is the only number here, and it
// exists because the alternative -- a highlight allowed to reach the very end -- reads as a bloom on the
// rounded corner rather than as light moving along a surface.
inline float SpecularCentreX(float left, float right, float fraction) {
    const float t = fraction < 0.0f ? 0.0f : (fraction > 1.0f ? 1.0f : fraction);
    const float span = right - left;
    return left + span * 0.1f + span * 0.8f * t;
}

// A soft specular highlight along the inside of the top edge, centred where the pointer is. Filled over
// the shape with a radial brush, so it is clipped by the glass and needs no layer (see the note on
// DrawGrain: a layer with a geometry mask cost this frame its island).
void DrawSpecularSweep(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape, const Theme& theme,
                       float pointerFraction, float opacity);

// The double edge: a faint dark line on the outside of the shape and a bright one just inside it, which
// together are what read as the THICKNESS of a piece of glass. It complements DrawSurfaceLighting's own
// directional hairline (inset 1 DIP) rather than replacing it: that one is the light on the face, this is
// the edge of the material.
void DrawGlassRim(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape, const Theme& theme,
                  float opacity);

// THE EDGE REFRACTION BAND (round 56). A 2.2 DIP ring just inside the rim, brightest along the top and
// almost gone at the bottom -- the curved wall of a piece of glass rather than a line drawn round it --
// plus two micro-offset hairlines (cool blue up-left, violet down-right) that read as dispersion.
void DrawEdgeRefraction(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape, const Theme& theme,
                        float opacity);

// THE CURVATURE INSIDE: a soft, wider shade that falls toward the bottom (the surface bending away from
// the light) and a broken, faint inner reflection line. Both stay well inside the rim.
void DrawCurvature(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape, const Theme& theme,
                   float opacity);

// The grain itself: 256x256 BGRA pixels (opaque grey), generated once from a fixed seed. Four bytes per
// pixel and not an alpha-only texture -- see the note in the .cpp about what an A8 bitmap did to the frame.
//
// PURE and deterministic, which is what makes it testable: a texture that changed between runs would be
// a different material every launch, and one built from rand() would be a different material on every
// machine. Never null; the buffer belongs to the module.
int NoiseSize();
const unsigned char* NoisePixels();

} // namespace island
