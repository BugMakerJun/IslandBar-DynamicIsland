// =====================================================================
//  GlassMaterial.cpp -- see the header for what belongs here and what does not.
//
//  EVERY BRUSH IS CACHED (2026-10-17, the user's liquid-glass brief). The gradients are built in a UNIT
//  space -- a vertical 0..1 line, a circle at (0.5,0.5) -- and each draw moves them with SetTransform, so
//  an animated shape costs a matrix, not an allocation. The cache is keyed by the DEVICE and the THEME:
//  a device loss rebuilds it, a theme switch rebuilds it, and nothing else does.
// =====================================================================
#include "render/GlassMaterial.h"

#include "core/Log.h"

#include <algorithm>
#include <vector>

namespace island {
namespace {

using std::min;

constexpr int kNoiseSize = 256;
std::vector<unsigned char> g_noise;

// The grain, once. A 32-bit xorshift with a fixed seed: reproducible on every machine and every run.
void BuildNoise() {
    if (!g_noise.empty()) return;
    g_noise.resize(static_cast<size_t>(kNoiseSize) * kNoiseSize * 4);

    unsigned int state = 0x9E3779B9u;
    const auto next = [&state]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };

    unsigned long long sum = 0;
    const size_t pixels = static_cast<size_t>(kNoiseSize) * kNoiseSize;
    for (size_t i = 0; i < pixels; ++i) {
        const unsigned char grey = static_cast<unsigned char>(112 + (next() % 32));
        g_noise[i * 4 + 0] = grey;
        g_noise[i * 4 + 1] = grey;
        g_noise[i * 4 + 2] = grey;
        g_noise[i * 4 + 3] = 255;
        sum += grey;
    }
    LogLine(L"  glass: grain built, %dx%d, mean %llu (deterministic seed 0x9E3779B9)", kNoiseSize,
            kNoiseSize, sum / pixels);
}

ID2D1Bitmap1* g_noiseBitmap = nullptr;
ID2D1Device* g_noiseDevice = nullptr;

ID2D1Bitmap1* NoiseBitmap(ID2D1DeviceContext2* context) {
    if (!context) return nullptr;
    ID2D1Device* device = nullptr;
    context->GetDevice(&device);
    if (!device) return nullptr;

    if (g_noiseBitmap && g_noiseDevice == device) {
        device->Release();
        return g_noiseBitmap;
    }
    if (g_noiseBitmap) {
        g_noiseBitmap->Release();
        g_noiseBitmap = nullptr;
    }
    if (g_noiseDevice) {
        g_noiseDevice->Release();
        g_noiseDevice = nullptr;
    }

    BuildNoise();
    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    ID2D1Bitmap1* bitmap = nullptr;
    const HRESULT hr = context->CreateBitmap(D2D1::SizeU(kNoiseSize, kNoiseSize), g_noise.data(),
                                             kNoiseSize * 4, properties, &bitmap);
    if (FAILED(hr) || !bitmap) {
        LogLine(L"  glass: the grain bitmap could not be created (hr 0x%08lX); the surfaces stay flat",
                static_cast<unsigned long>(hr));
        device->Release();
        return nullptr;
    }
    g_noiseBitmap = bitmap;
    g_noiseDevice = device;
    return g_noiseBitmap;
}

// --- the brush cache ------------------------------------------------------------------------------

ID2D1LinearGradientBrush* MakeLinear(ID2D1DeviceContext2* context, const D2D1_GRADIENT_STOP* stops,
                                     UINT count) {
    ID2D1GradientStopCollection* collection = nullptr;
    if (FAILED(context->CreateGradientStopCollection(stops, count, D2D1_GAMMA_2_2,
                                                     D2D1_EXTEND_MODE_CLAMP, &collection)) ||
        !collection) {
        return nullptr;
    }
    ID2D1LinearGradientBrush* brush = nullptr;
    const D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES properties = {D2D1::Point2F(0.0f, 0.0f),
                                                              D2D1::Point2F(0.0f, 1.0f)};
    if (FAILED(context->CreateLinearGradientBrush(properties, collection, &brush))) {
        brush = nullptr;
    }
    collection->Release();
    return brush;
}

ID2D1RadialGradientBrush* MakeRadial(ID2D1DeviceContext2* context, const D2D1_GRADIENT_STOP* stops,
                                     UINT count) {
    ID2D1GradientStopCollection* collection = nullptr;
    if (FAILED(context->CreateGradientStopCollection(stops, count, D2D1_GAMMA_2_2,
                                                     D2D1_EXTEND_MODE_CLAMP, &collection)) ||
        !collection) {
        return nullptr;
    }
    ID2D1RadialGradientBrush* brush = nullptr;
    const D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES properties = {
        D2D1::Point2F(0.5f, 0.5f), D2D1::Point2F(0.5f, 0.5f), 0.5f, 0.5f};
    if (FAILED(context->CreateRadialGradientBrush(properties, collection, &brush))) {
        brush = nullptr;
    }
    collection->Release();
    return brush;
}

// A unit vertical line mapped onto the given band; the stops run from its top to its bottom.
void PlaceLinear(ID2D1LinearGradientBrush* brush, float left, float top, float bottom, float opacity) {
    if (!brush) return;
    brush->SetTransform(D2D1::Matrix3x2F::Scale(1.0f, bottom - top) *
                        D2D1::Matrix3x2F::Translation(left, top));
    brush->SetOpacity(opacity);
}

ID2D1LinearGradientBrush* MakeLinearHorizontal(ID2D1DeviceContext2* context,
                                               const D2D1_GRADIENT_STOP* stops, UINT count) {
    ID2D1GradientStopCollection* collection = nullptr;
    if (FAILED(context->CreateGradientStopCollection(stops, count, D2D1_GAMMA_2_2,
                                                     D2D1_EXTEND_MODE_CLAMP, &collection)) ||
        !collection) {
        return nullptr;
    }
    ID2D1LinearGradientBrush* brush = nullptr;
    const D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES properties = {D2D1::Point2F(0.0f, 0.0f),
                                                              D2D1::Point2F(1.0f, 0.0f)};
    if (FAILED(context->CreateLinearGradientBrush(properties, collection, &brush))) brush = nullptr;
    collection->Release();
    return brush;
}

void PlaceLinearHorizontal(ID2D1LinearGradientBrush* brush, float left, float right, float top,
                           float opacity) {
    if (!brush) return;
    brush->SetTransform(D2D1::Matrix3x2F::Scale(right - left, 1.0f) *
                        D2D1::Matrix3x2F::Translation(left, top));
    brush->SetOpacity(opacity);
}

// A unit circle mapped onto the ellipse (centre, radii).
void PlaceRadial(ID2D1RadialGradientBrush* brush, float centreX, float centreY, float radiusX,
                 float radiusY, float opacity) {
    if (!brush) return;
    brush->SetTransform(D2D1::Matrix3x2F::Scale(radiusX * 2.0f, radiusY * 2.0f) *
                        D2D1::Matrix3x2F::Translation(centreX - radiusX, centreY - radiusY));
    brush->SetOpacity(opacity);
}

struct MaterialCache {
    ID2D1Device* device = nullptr;
    const Theme* theme = nullptr;

    ID2D1SolidColorBrush* solid = nullptr;
    ID2D1LinearGradientBrush* topWash = nullptr;
    ID2D1LinearGradientBrush* lensRim = nullptr;
    ID2D1LinearGradientBrush* bottomShade = nullptr;
    ID2D1LinearGradientBrush* rimLine = nullptr;
    ID2D1LinearGradientBrush* specularRim = nullptr;
    ID2D1LinearGradientBrush* causticRim = nullptr;
    ID2D1RadialGradientBrush* sheen = nullptr;
    ID2D1RadialGradientBrush* tintLeft = nullptr;
    ID2D1RadialGradientBrush* tintRight = nullptr;
    ID2D1LinearGradientBrush* edgeBand = nullptr;
    ID2D1LinearGradientBrush* dispersionCool = nullptr;
    ID2D1LinearGradientBrush* dispersionWarm = nullptr;
    ID2D1LinearGradientBrush* curveShade = nullptr;
    ID2D1LinearGradientBrush* caustic = nullptr;
    ID2D1BitmapBrush1* grain = nullptr;

    // The base fill depends on the theme AND on the wallpaper tint, so it is compared by colour.
    ID2D1LinearGradientBrush* fill = nullptr;
    D2D1_COLOR_F fillTop = {};
    D2D1_COLOR_F fillBottom = {};

    bool valid = false;
};

MaterialCache g_cache;

bool SameColour(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

void ReleaseCache() {
    if (g_cache.solid) g_cache.solid->Release();
    if (g_cache.topWash) g_cache.topWash->Release();
    if (g_cache.lensRim) g_cache.lensRim->Release();
    if (g_cache.bottomShade) g_cache.bottomShade->Release();
    if (g_cache.rimLine) g_cache.rimLine->Release();
    if (g_cache.specularRim) g_cache.specularRim->Release();
    if (g_cache.causticRim) g_cache.causticRim->Release();
    if (g_cache.sheen) g_cache.sheen->Release();
    if (g_cache.tintLeft) g_cache.tintLeft->Release();
    if (g_cache.tintRight) g_cache.tintRight->Release();
    if (g_cache.edgeBand) g_cache.edgeBand->Release();
    if (g_cache.dispersionCool) g_cache.dispersionCool->Release();
    if (g_cache.dispersionWarm) g_cache.dispersionWarm->Release();
    if (g_cache.curveShade) g_cache.curveShade->Release();
    if (g_cache.caustic) g_cache.caustic->Release();
    if (g_cache.grain) g_cache.grain->Release();
    if (g_cache.fill) g_cache.fill->Release();
    if (g_cache.device) g_cache.device->Release();
    g_cache = MaterialCache{};
}

// Rebuilds on a device change or a theme change, and only then. `CurrentTheme()` returns a reference into a
// list that lives for the process, so the pointer is a stable identity for the theme in force.
bool EnsureMaterial(ID2D1DeviceContext2* context, const Theme& theme) {
    if (!context) return false;
    ID2D1Device* device = nullptr;
    context->GetDevice(&device);
    if (!device) return false;

    if (g_cache.valid && g_cache.device == device && g_cache.theme == &theme) {
        device->Release();
        return true;
    }

    ReleaseCache();
    g_cache.device = device;   // kept
    g_cache.theme = &theme;

    context->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f), &g_cache.solid);

    const D2D1_GRADIENT_STOP topStops[2] = {
        {0.0f, ThemeHighlight(theme.gloss * 1.25f)},
        {1.0f, ThemeHighlight(0.0f)},
    };
    g_cache.topWash = MakeLinear(context, topStops, 2);

    // THE LENS RIM BAND: a 2.5 DIP band INSIDE the shape, whose gradient is bright at the top (0.30),
    // almost nothing at the sides (0.02) and a soft dark at the bottom (0.08). It follows the round ends,
    // which the horizontal top wash could not -- that is what looked unnatural on the caps. It is a band,
    // not a line: the stroke is inset by half its width so nothing paints outside the pill.
    const D2D1_GRADIENT_STOP lensRimStops[3] = {
        {0.0f, ThemeHighlight(0.30f)},
        {0.50f, ThemeHighlight(0.02f)},
        {1.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f)},
    };
    g_cache.lensRim = MakeLinear(context, lensRimStops, 3);

    const D2D1_GRADIENT_STOP bottomStops[2] = {
        {0.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f)},
        {1.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, theme.gloss * 0.6f)},
    };
    g_cache.bottomShade = MakeLinear(context, bottomStops, 2);

    const D2D1_GRADIENT_STOP rimStops[2] = {
        {0.0f, ThemeHighlight(theme.gloss * 0.95f)},
        {1.0f, ThemeHighlight(theme.gloss * 0.10f)},
    };
    g_cache.rimLine = MakeLinear(context, rimStops, 2);

    // THE THIN SPECULAR RIM: the bright contour the reference material has all the way round, brighter
    // at the top and again at the bottom where the glass catches the table. One DIP wide, so it reads as
    // the lit edge of the material rather than as a frame drawn round it.
    const D2D1_GRADIENT_STOP specularStops[3] = {
        {0.0f, ThemeHighlight(theme.rim * 1.55f)},
        {0.55f, ThemeHighlight(theme.rim * 0.35f)},
        {1.0f, ThemeHighlight(theme.rim * 1.10f)},
    };
    g_cache.specularRim = MakeLinear(context, specularStops, 3);

    // THE CAUSTIC: light gathered through the bottom of the glass, a narrow bright run just inside the
    // contour. It is what makes the pane look thick rather than drawn -- the reference's bottom edge has
    // exactly this.
    const D2D1_GRADIENT_STOP causticRimStops[3] = {
        {0.0f, ThemeHighlight(0.0f)},
        {0.58f, ThemeHighlight(theme.rim * 0.22f)},
        {1.0f, ThemeHighlight(theme.rim * 1.10f)},
    };
    g_cache.causticRim = MakeLinear(context, causticRimStops, 3);

    // THE TOP SHEEN: one soft brush, drawn as a few blobs at different places. Irregular by construction;
    // a horizontal band across the whole top is the "light bar" the brief forbids.
    const D2D1_GRADIENT_STOP sheenStops[3] = {
        {0.0f, ThemeHighlight(theme.gloss * 0.85f)},
        {0.55f, ThemeHighlight(theme.gloss * 0.22f)},
        {1.0f, ThemeHighlight(0.0f)},
    };
    g_cache.sheen = MakeRadial(context, sheenStops, 3);

    // THE EDGE REFRACTION TINTS: cool blue on one side, violet-blue on the other, at the strength the
    // theme's wallpaper `tint` already expresses. Soft ellipses pinned to each end, never reaching the
    // middle where the text is.
    const D2D1_GRADIENT_STOP leftStops[2] = {
        {0.0f, D2D1::ColorF(0.42f, 0.56f, 1.00f, theme.tint * 0.38f)},
        {1.0f, D2D1::ColorF(0.42f, 0.56f, 1.00f, 0.0f)},
    };
    g_cache.tintLeft = MakeRadial(context, leftStops, 2);

    const D2D1_GRADIENT_STOP rightStops[2] = {
        {0.0f, D2D1::ColorF(0.62f, 0.46f, 1.00f, theme.tint * 0.32f)},
        {1.0f, D2D1::ColorF(0.62f, 0.46f, 1.00f, 0.0f)},
    };
    g_cache.tintRight = MakeRadial(context, rightStops, 2);

    // THE EDGE BAND (round 56): 0.22 at the top, 0.10 at the sides, 0.015 at the bottom. A vertical
    // gradient over a 2.2 DIP stroke is the whole trick -- it turns the rim from a line into a curved
    // wall, without a second geometry or a layer.
    const D2D1_GRADIENT_STOP bandStops[3] = {
        {0.0f, ThemeHighlight(theme.rim * 0.23f)},
        {0.50f, ThemeHighlight(theme.rim * 0.10f)},
        {1.0f, ThemeHighlight(theme.rim * 0.02f)},
    };
    g_cache.edgeBand = MakeLinear(context, bandStops, 3);

    // DISPERSION: a cool hairline that lives along the TOP half and a violet one along the bottom half, so
    // neither becomes the blue ring both reviews warned about.
    const D2D1_GRADIENT_STOP coolStops[2] = {
        {0.0f, D2D1::ColorF(0.45f, 0.75f, 1.00f, theme.tint * 0.45f)},
        {1.0f, D2D1::ColorF(0.45f, 0.75f, 1.00f, 0.0f)},
    };
    g_cache.dispersionCool = MakeLinear(context, coolStops, 2);
    const D2D1_GRADIENT_STOP warmStops[2] = {
        {0.0f, D2D1::ColorF(0.85f, 0.45f, 1.00f, 0.0f)},
        {1.0f, D2D1::ColorF(0.85f, 0.45f, 1.00f, theme.tint * 0.28f)},
    };
    g_cache.dispersionWarm = MakeLinear(context, warmStops, 2);

    // CURVATURE: a wide, soft shade that grows toward the bottom, and a hairline that fades at both ends
    // (which is what keeps it from reading as a second frame).
    const D2D1_GRADIENT_STOP shadeStops[3] = {
        {0.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f)},
        {0.55f, D2D1::ColorF(0.0f, 0.0f, 0.0f, theme.rim * 0.055f)},
        {1.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, theme.rim * 0.11f)},
    };
    g_cache.curveShade = MakeLinear(context, shadeStops, 3);
    const D2D1_GRADIENT_STOP causticStops[3] = {
        {0.0f, ThemeHighlight(0.0f)},
        {0.50f, ThemeHighlight(theme.rim * 0.075f)},
        {1.0f, ThemeHighlight(0.0f)},
    };
    g_cache.caustic = MakeLinearHorizontal(context, causticStops, 3);

    if (ID2D1Bitmap1* noise = NoiseBitmap(context)) {
        const D2D1_BITMAP_BRUSH_PROPERTIES1 properties = D2D1::BitmapBrushProperties1(
            D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP, D2D1_INTERPOLATION_MODE_LINEAR);
        context->CreateBitmapBrush(noise, properties, &g_cache.grain);
    }

    g_cache.valid = true;
    return true;
}

}  // namespace

int NoiseSize() { return kNoiseSize; }

const unsigned char* NoisePixels() {
    BuildNoise();
    return g_noise.data();
}

void DrawSmokedBase(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape, const Theme& theme,
                    const D2D1_COLOR_F& top, const D2D1_COLOR_F& bottom, float opacity) {
    if (!context || opacity <= 0.001f) return;
    if (shape.rect.right <= shape.rect.left || shape.rect.bottom <= shape.rect.top) return;
    if (!EnsureMaterial(context, theme)) return;

    if (!g_cache.fill || !SameColour(g_cache.fillTop, top) || !SameColour(g_cache.fillBottom, bottom)) {
        const D2D1_GRADIENT_STOP stops[2] = {{0.0f, top}, {1.0f, bottom}};
        if (g_cache.fill) g_cache.fill->Release();
        g_cache.fill = MakeLinear(context, stops, 2);
        g_cache.fillTop = top;
        g_cache.fillBottom = bottom;
    }
    if (g_cache.fill) {
        PlaceLinear(g_cache.fill, shape.rect.left, shape.rect.top, shape.rect.bottom, opacity);
        context->FillRoundedRectangle(&shape, g_cache.fill);
    }

    // The hairline in the fill colour covers the antialiased fringe a plain fill leaves along the curve,
    // which otherwise shows as a pale seam against the desktop. It is a 1px STROKE, though, and on the
    // clear lens it read as a line along the top edge (the user's screenshot); the lens skips it and lets
    // the fill's own antialiasing carry the fringe.
    if (g_cache.solid && !theme.lens) {
        g_cache.solid->SetColor(top);
        g_cache.solid->SetOpacity(opacity);
        context->DrawRoundedRectangle(&shape, g_cache.solid, 1.0f);
    }
}

void DrawSoftShadow(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape, float opacity,
                    float spreadDip) {
    if (!context || opacity <= 0.001f || spreadDip <= 0.01f) return;
    if (shape.rect.right <= shape.rect.left || shape.rect.bottom <= shape.rect.top) return;
    if (!EnsureMaterial(context, CurrentTheme()) || !g_cache.solid) return;

    constexpr int kSteps = 6;
    const float stepDip = spreadDip / static_cast<float>(kSteps);
    for (int step = kSteps; step >= 1; --step) {
        const float grow = stepDip * static_cast<float>(step);
        const float t = static_cast<float>(step) / static_cast<float>(kSteps);
        const float alpha = opacity * (1.0f - t) * (1.0f - t) / static_cast<float>(kSteps) * 2.2f;
        if (alpha <= 0.001f) continue;

        g_cache.solid->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, alpha));
        g_cache.solid->SetOpacity(1.0f);
        const D2D1_ROUNDED_RECT grown = {
            D2D1::RectF(shape.rect.left - grow, shape.rect.top - grow, shape.rect.right + grow,
                        shape.rect.bottom + grow),
            shape.radiusX + grow * 0.6f, shape.radiusY + grow * 0.6f};
        context->FillRoundedRectangle(&grown, g_cache.solid);
    }
}

void DrawSurfaceLighting(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape,
                         const Theme& theme, float opacity) {
    if (!context || opacity <= 0.01f) return;
    const float height = shape.rect.bottom - shape.rect.top;
    const float width = shape.rect.right - shape.rect.left;
    if (height < 6.0f || width < 6.0f || theme.gloss <= 0.001f) return;
    if (!EnsureMaterial(context, theme)) return;

    const float topBand = min(height * 0.45f, 20.0f);
    const float bottomBand = min(height * 0.35f, 26.0f);

    // THE TOP LIGHT fades out BEFORE the caps. A full-width linear wash ran into the shape's vertical
    // edge at the right cap and stopped there, which read as the highlight being cut with a knife.
    // A wide, flat ellipse falls off in every direction, so the light never reaches the rim. Only the
    // lens theme takes it: the other themes keep the linear wash their own pixel baselines were built on.
    if (theme.lens && g_cache.sheen) {
        PlaceRadial(g_cache.sheen, (shape.rect.left + shape.rect.right) * 0.5f, shape.rect.top + 2.0f,
                    width * 0.46f, topBand * 0.95f, opacity);
        context->FillRoundedRectangle(&shape, g_cache.sheen);
        // A NARROW specular just under the top edge: the wet gleam that makes the pane read as glass
        // even when the desktop behind it is as bright as the surface. Narrow on purpose -- a wide one
        // is the fog the user rejected.
        PlaceRadial(g_cache.sheen, (shape.rect.left + shape.rect.right) * 0.5f, shape.rect.top + 1.5f,
                    width * 0.40f, 4.5f, opacity * 0.55f);
        context->FillRoundedRectangle(&shape, g_cache.sheen);
    } else if (g_cache.topWash) {
        PlaceLinear(g_cache.topWash, shape.rect.left, shape.rect.top, shape.rect.top + topBand, opacity);
        context->FillRoundedRectangle(&shape, g_cache.topWash);
    }
    // The clear lens takes no bottom shade: a black gradient over the lower half is exactly the grey
    // layer the user sees on an otherwise transparent pane.
    if (!theme.lens && g_cache.bottomShade) {
        PlaceLinear(g_cache.bottomShade, shape.rect.left, shape.rect.bottom - bottomBand,
                    shape.rect.bottom, opacity);
        context->FillRoundedRectangle(&shape, g_cache.bottomShade);
    }

}

void DrawTopSheen(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape, const Theme& theme,
                  float pointerFraction, float opacity) {
    if (!context || opacity <= 0.01f || theme.gloss <= 0.001f) return;
    const float width = shape.rect.right - shape.rect.left;
    const float height = shape.rect.bottom - shape.rect.top;
    if (width < 40.0f || height < 8.0f) return;
    if (!EnsureMaterial(context, theme) || !g_cache.sheen) return;

    // Three blobs, not one band. Their places and sizes are fixed fractions of the shape -- deterministic,
    // so a screenshot is the same every run -- and the pointer only SHIFTS them a little (the brief's
    // "鎮仠鏃跺彧澧炲己灏戦噺楂樺厜骞惰繘琛岃交寰亸绉?).
    const float band = min(height * 0.45f, 20.0f);
    const float t = pointerFraction < 0.0f ? 0.0f : (pointerFraction > 1.0f ? 1.0f : pointerFraction);
    const float shift = (t - 0.5f) * width * 0.035f;
    struct Blob {
        float x;
        float y;
        float radiusX;
        float radiusY;
        float alpha;
    };
    // Three soft cores, offset by the pointer. The wide wash in DrawSurfaceLighting is the reflection;
    // these are the brighter sources inside it. Narrow on purpose: they must die out well before the
    // caps, or the cap's vertical edge cuts them. The lens theme does not use them -- it has the thick
    // rim band (DrawLensRimBand) instead, because a blob at a fixed x read as a patch of light.
    const Blob blobs[3] = {
        {0.34f, 0.20f, 0.20f, 0.55f, 0.95f},
        {0.54f, 0.15f, 0.24f, 0.40f, 0.70f},
        {0.74f, 0.26f, 0.16f, 0.48f, 0.50f},
    };
    for (int i = 0; i < 3; ++i) {
        const Blob& blob = blobs[i];
        PlaceRadial(g_cache.sheen, shape.rect.left + width * blob.x + shift,
                    shape.rect.top + band * blob.y, width * blob.radiusX, height * blob.radiusY,
                    opacity * blob.alpha);
        context->FillRoundedRectangle(&shape, g_cache.sheen);
    }
}

void DrawThinRim(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape, const Theme& theme,
                 float opacity) {
    if (!context || opacity <= 0.01f || theme.rim <= 0.001f) return;
    if (shape.rect.right <= shape.rect.left || shape.rect.bottom <= shape.rect.top) return;
    if (!EnsureMaterial(context, theme) || !g_cache.specularRim) return;
    PlaceLinear(g_cache.specularRim, shape.rect.left, shape.rect.top, shape.rect.bottom, opacity);
    context->DrawRoundedRectangle(&shape, g_cache.specularRim, 1.05f);
    if (theme.lens && g_cache.causticRim) {
        const float inset = 1.8f;
        const D2D1_ROUNDED_RECT inner = {
            D2D1::RectF(shape.rect.left + inset, shape.rect.top + inset, shape.rect.right - inset,
                        shape.rect.bottom - inset),
            std::max(0.0f, shape.radiusX - inset), std::max(0.0f, shape.radiusY - inset)};
        PlaceLinear(g_cache.causticRim, shape.rect.left, shape.rect.top, shape.rect.bottom, opacity);
        context->DrawRoundedRectangle(&inner, g_cache.causticRim, 1.0f);
    }
}

void DrawEdgeTints(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape, const Theme& theme,
                   float opacity) {
    if (!context || opacity <= 0.01f || theme.tint <= 0.001f) return;
    // No tinted haze at the ends of a clear lens; it was the other half of the grey.
    if (theme.lens) return;
    const float width = shape.rect.right - shape.rect.left;
    const float height = shape.rect.bottom - shape.rect.top;
    if (width < 40.0f || height < 8.0f) return;
    if (!EnsureMaterial(context, theme)) return;

    const float midY = (shape.rect.top + shape.rect.bottom) * 0.5f;
    if (g_cache.tintLeft) {
        PlaceRadial(g_cache.tintLeft, shape.rect.left + width * 0.06f, midY, width * 0.17f,
                    height * 0.85f, opacity);
        context->FillRoundedRectangle(&shape, g_cache.tintLeft);
    }
    if (g_cache.tintRight) {
        PlaceRadial(g_cache.tintRight, shape.rect.right - width * 0.07f, midY, width * 0.15f,
                    height * 0.80f, opacity);
        context->FillRoundedRectangle(&shape, g_cache.tintRight);
    }
}

void DrawGrain(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape, float opacity) {
    if (!context || opacity <= 0.001f) return;
    if (shape.rect.right <= shape.rect.left || shape.rect.bottom <= shape.rect.top) return;
    if (!EnsureMaterial(context, CurrentTheme()) || !g_cache.grain) return;
    g_cache.grain->SetOpacity(opacity);
    context->FillRoundedRectangle(&shape, g_cache.grain);
}

void DrawSpecularSweep(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape, const Theme& theme,
                       float pointerFraction, float opacity) {
    if (!context || opacity <= 0.01f || theme.sweep <= 0.001f) return;
    const float width = shape.rect.right - shape.rect.left;
    const float height = shape.rect.bottom - shape.rect.top;
    if (width < 24.0f || height < 6.0f) return;
    if (!EnsureMaterial(context, theme) || !g_cache.sheen) return;
    const float centreX = SpecularCentreX(shape.rect.left, shape.rect.right, pointerFraction);

    // A faint pool in the body: the surface tension. The old 45x26 pool at 0.95 read as a torch, so this
    // is half the height and a third of the strength.
    PlaceRadial(g_cache.sheen, centreX, shape.rect.top + height * 0.55f,
                std::min(width * 0.22f, 38.0f), std::min(height * 0.40f, 14.0f), opacity * 0.40f);
    context->FillRoundedRectangle(&shape, g_cache.sheen);

    // THE VISIBLE PART: a small glint on the top rim at the pointer's x -- the glass edge catching the
    // light as the pointer moves. No vertical bar, no body-wide blob.
    PlaceRadial(g_cache.sheen, centreX, shape.rect.top + 1.0f, std::min(width * 0.12f, 26.0f),
                std::min(height * 0.12f, 4.5f), opacity * 0.90f);
    context->FillRoundedRectangle(&shape, g_cache.sheen);
}

void DrawLensBottomCaustic(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape, const Theme& theme,
                           float opacity) {
    if (!context || opacity <= 0.01f) return;
    const float width = shape.rect.right - shape.rect.left;
    const float height = shape.rect.bottom - shape.rect.top;
    if (width < 24.0f || height < 6.0f) return;
    if (!EnsureMaterial(context, theme) || !g_cache.sheen) return;
    // Just inside the bottom edge, centred. The inner shade is already in the refracted image; this is the
    // light gathered above it, which is what makes the pane look thick rather than tinted.
    PlaceRadial(g_cache.sheen, (shape.rect.left + shape.rect.right) * 0.5f, shape.rect.bottom - 2.0f,
                width * 0.35f, 5.5f, opacity * 0.55f);
    context->FillRoundedRectangle(&shape, g_cache.sheen);
}

void DrawLensRimBand(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape, const Theme& theme,
                     float opacity) {
    if (!context || opacity <= 0.01f) return;
    const float width = shape.rect.right - shape.rect.left;
    const float height = shape.rect.bottom - shape.rect.top;
    if (width < 24.0f || height < 6.0f) return;
    if (!EnsureMaterial(context, theme) || !g_cache.lensRim) return;
    // A 2.5 DIP band INSIDE the shape: inset the stroke by half its width, so its outer edge is exactly
    // the pill and nothing paints outside. Following the rounded rect is what makes the caps natural; the
    // horizontal top wash could not. Bright top, almost nothing at the sides, soft dark at the bottom.
    constexpr float kBand = 2.5f;
    const float inset = kBand * 0.5f;
    const D2D1_ROUNDED_RECT inner = {
        D2D1::RectF(shape.rect.left + inset, shape.rect.top + inset, shape.rect.right - inset,
                    shape.rect.bottom - inset),
        std::max(0.0f, shape.radiusX - inset), std::max(0.0f, shape.radiusY - inset)};
    PlaceLinear(g_cache.lensRim, shape.rect.left, shape.rect.top, shape.rect.bottom, opacity);
    context->DrawRoundedRectangle(&inner, g_cache.lensRim, kBand);
}

void DrawGlassRim(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape, const Theme& theme,
                  float opacity) {
    if (!context || opacity <= 0.01f || theme.rim <= 0.001f) return;
    if (shape.rect.right <= shape.rect.left || shape.rect.bottom <= shape.rect.top) return;
    if (!EnsureMaterial(context, theme) || !g_cache.solid) return;

    // THE OUTSIDE LINE, faint and dark: on the desktop side of the edge it separates the glass from
    // whatever is behind it, and that separation is half of what reads as thickness.
    g_cache.solid->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, theme.rim * 0.22f * opacity));
    g_cache.solid->SetOpacity(1.0f);
    context->DrawRoundedRectangle(&shape, g_cache.solid, 1.0f);

    // THE INNER DARK EDGE: a second, darker hairline further in. Without it the bright line is a frame
    // drawn on a surface; with it there is a lit edge, a dark thickness, and then the face of the glass.
    const float darkInset = 3.1f;
    const D2D1_ROUNDED_RECT thickness = {
        D2D1::RectF(shape.rect.left + darkInset, shape.rect.top + darkInset,
                    shape.rect.right - darkInset, shape.rect.bottom - darkInset),
        shape.radiusX > darkInset ? shape.radiusX - darkInset : 0.0f,
        shape.radiusY > darkInset ? shape.radiusY - darkInset : 0.0f};
    g_cache.solid->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, theme.rim * 0.30f * opacity));
    context->DrawRoundedRectangle(&thickness, g_cache.solid, 1.0f);
}

void DrawEdgeRefraction(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape, const Theme& theme,
                        float opacity) {
    if (!context || opacity <= 0.01f || theme.rim <= 0.001f) return;
    if (shape.rect.right <= shape.rect.left || shape.rect.bottom <= shape.rect.top) return;
    if (!EnsureMaterial(context, theme)) return;

    // The lit wall: a 2.2 DIP stroke whose vertical gradient is the whole effect. It sits where the old
    // single bright line was, so the rim does not become two lines.
    const float inset = 1.75f;
    const D2D1_ROUNDED_RECT band = {
        D2D1::RectF(shape.rect.left + inset, shape.rect.top + inset,
                    shape.rect.right - inset, shape.rect.bottom - inset),
        shape.radiusX > inset ? shape.radiusX - inset : 0.0f,
        shape.radiusY > inset ? shape.radiusY - inset : 0.0f};
    if (g_cache.edgeBand) {
        PlaceLinear(g_cache.edgeBand, shape.rect.left, shape.rect.top, shape.rect.bottom, opacity);
        context->DrawRoundedRectangle(&band, g_cache.edgeBand, 2.2f);
    }

    // Dispersion: the same contour, nudged half a DIP up-left and half a DIP down-right. The gradients put
    // the cool fringe on the top half and the violet one on the bottom half, so no full blue ring appears.
    const auto shifted = [&band](float dx, float dy) {
        D2D1_ROUNDED_RECT out = band;
        out.rect.left += dx;
        out.rect.right += dx;
        out.rect.top += dy;
        out.rect.bottom += dy;
        return out;
    };
    if (g_cache.dispersionCool) {
        const D2D1_ROUNDED_RECT cool = shifted(-0.5f, -0.5f);
        PlaceLinear(g_cache.dispersionCool, shape.rect.left, shape.rect.top, shape.rect.bottom, opacity);
        context->DrawRoundedRectangle(&cool, g_cache.dispersionCool, 0.75f);
    }
    if (g_cache.dispersionWarm) {
        const D2D1_ROUNDED_RECT warm = shifted(0.5f, 0.5f);
        PlaceLinear(g_cache.dispersionWarm, shape.rect.left, shape.rect.top, shape.rect.bottom, opacity);
        context->DrawRoundedRectangle(&warm, g_cache.dispersionWarm, 0.75f);
    }
}

void DrawCurvature(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape, const Theme& theme,
                   float opacity) {
    if (!context || opacity <= 0.01f || theme.rim <= 0.001f) return;
    if (shape.rect.right <= shape.rect.left || shape.rect.bottom <= shape.rect.top) return;
    if (!EnsureMaterial(context, theme)) return;

    // A wide, soft shade inside the wall, growing toward the bottom: the surface turning away from the
    // light. 5 DIP wide at inset 7, so it never touches the rim itself.
    const float shadeInset = 7.0f;
    const D2D1_ROUNDED_RECT shade = {
        D2D1::RectF(shape.rect.left + shadeInset, shape.rect.top + shadeInset,
                    shape.rect.right - shadeInset, shape.rect.bottom - shadeInset),
        shape.radiusX > shadeInset ? shape.radiusX - shadeInset : 0.0f,
        shape.radiusY > shadeInset ? shape.radiusY - shadeInset : 0.0f};
    if (g_cache.curveShade) {
        PlaceLinear(g_cache.curveShade, shape.rect.left, shape.rect.top, shape.rect.bottom, opacity);
        context->DrawRoundedRectangle(&shade, g_cache.curveShade, 5.0f);
    }

    // The internal reflection: a 0.6 DIP line that fades out at both ends, so it reads as a vein in the
    // glass rather than as a second outline.
    const float lineInset = 4.2f;
    const D2D1_ROUNDED_RECT line = {
        D2D1::RectF(shape.rect.left + lineInset, shape.rect.top + lineInset,
                    shape.rect.right - lineInset, shape.rect.bottom - lineInset),
        shape.radiusX > lineInset ? shape.radiusX - lineInset : 0.0f,
        shape.radiusY > lineInset ? shape.radiusY - lineInset : 0.0f};
    if (g_cache.caustic) {
        PlaceLinearHorizontal(g_cache.caustic, shape.rect.left, shape.rect.right, shape.rect.top,
                              opacity);
        context->DrawRoundedRectangle(&line, g_cache.caustic, 0.6f);
    }
}

void DrawCardCrystal(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape, float opacity) {
    if (!context || opacity <= 0.01f) return;
    const float width = shape.rect.right - shape.rect.left;
    const float height = shape.rect.bottom - shape.rect.top;
    if (width < 16.0f || height < 16.0f) return;

    ID2D1Factory* factory = nullptr;
    context->GetFactory(&factory);
    ID2D1RoundedRectangleGeometry* clip = nullptr;
    if (factory) {
        factory->CreateRoundedRectangleGeometry(shape, &clip);
        factory->Release();
    }
    if (clip) {
        context->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), clip,
                                                  D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                           nullptr);
    }

    const auto fillGradient = [&](const D2D1_GRADIENT_STOP* stops, UINT32 count,
                                  D2D1_POINT_2F from, D2D1_POINT_2F to) {
        ID2D1GradientStopCollection* collection = nullptr;
        if (FAILED(context->CreateGradientStopCollection(stops, count, &collection)) || !collection) {
            return;
        }
        ID2D1LinearGradientBrush* brush = nullptr;
        context->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(from, to), collection,
                                           &brush);
        collection->Release();
        if (brush) {
            context->FillRoundedRectangle(&shape, brush);
            brush->Release();
        }
    };

    // 1. THE TOP FACET -- the reference's `inset 0 4px 8px -2px rgba(255,255,255,0.8)`: a bright band
    //    just inside the top edge, gone by 6 DIP. On a 2% body this is the highlight that makes the
    //    surface read as a sheet of crystal rather than as a tinted rectangle.
    {
        const D2D1_GRADIENT_STOP stops[2] = {
            {0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.80f * opacity)},
            {1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
        };
        fillGradient(stops, 2, D2D1::Point2F(shape.rect.left, shape.rect.top),
                     D2D1::Point2F(shape.rect.left, shape.rect.top + 6.0f));
    }

    // 2. THE BOTTOM THICKNESS -- `inset 0 -4px 8px -2px rgba(0,0,0,0.1)`: the shadow a thick sheet
    //    gathers inside its bottom edge, which is what stops the body from looking like a flat tint.
    {
        const D2D1_GRADIENT_STOP stops[2] = {
            {0.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f * opacity)},
            {1.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f)},
        };
        fillGradient(stops, 2, D2D1::Point2F(shape.rect.left, shape.rect.bottom),
                     D2D1::Point2F(shape.rect.left, shape.rect.bottom - 6.0f));
    }

    // 3. THE DIAGONAL SHEEN -- the reference's one sharp band: transparent until 40%, a 22% shoulder,
    //    a 60% core at the middle, the shoulder again, transparent by 60%. Corner to corner, so the
    //    band lies on the diagonal without a transform.
    {
        const D2D1_GRADIENT_STOP stops[6] = {
            {0.00f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
            {0.40f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
            {0.48f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f * opacity)},
            {0.50f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.60f * opacity)},
            {0.52f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f * opacity)},
            {0.60f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
        };
        fillGradient(stops, 6, D2D1::Point2F(shape.rect.left, shape.rect.top),
                     D2D1::Point2F(shape.rect.right, shape.rect.bottom));
    }

    if (clip) {
        context->PopLayer();
        clip->Release();
    }

    // 4. THE EDGE -- `border: 1px solid rgba(255,255,255,0.4)`. After the layer, so its own antialiased
    //    fringe is not clipped away; this is the crisp outline the reference has, and the island's
    //    2.5 DIP band deliberately is not (it is right at 36 DIP and a coloured strip at card size).
    ID2D1SolidColorBrush* edge = nullptr;
    if (SUCCEEDED(context->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.40f * opacity),
                                                 &edge)) &&
        edge) {
        context->DrawRoundedRectangle(&shape, edge, 1.0f);
        edge->Release();
    }
}

}  // namespace island

