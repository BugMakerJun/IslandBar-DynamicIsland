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
    ID2D1LinearGradientBrush* bottomShade = nullptr;
    ID2D1LinearGradientBrush* rimLine = nullptr;
    ID2D1LinearGradientBrush* specularRim = nullptr;
    ID2D1RadialGradientBrush* sweep = nullptr;
    ID2D1RadialGradientBrush* depth = nullptr;
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
    if (g_cache.bottomShade) g_cache.bottomShade->Release();
    if (g_cache.rimLine) g_cache.rimLine->Release();
    if (g_cache.specularRim) g_cache.specularRim->Release();
    if (g_cache.sweep) g_cache.sweep->Release();
    if (g_cache.depth) g_cache.depth->Release();
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
        {0.0f, ThemeHighlight(theme.rim * 1.00f)},
        {0.55f, ThemeHighlight(theme.rim * 0.45f)},
        {1.0f, ThemeHighlight(theme.rim * 0.70f)},
    };
    g_cache.specularRim = MakeLinear(context, specularStops, 3);

    const D2D1_GRADIENT_STOP sweepStops[3] = {
        {0.0f, ThemeHighlight(theme.sweep * 0.95f)},
        {0.30f, ThemeHighlight(theme.sweep * 0.30f)},
        {1.0f, ThemeHighlight(0.0f)},
    };
    g_cache.sweep = MakeRadial(context, sweepStops, 3);

    // THE SMOKED DEPTH: the centre of the glass is heavier than its rim. Blue-black rather than neutral
    // black, because a cold shadow is what smoke reads as. The strength rides on `rim`, so a theme that
    // asks for a material gets this layer and one that does not gets nothing.
    // With the pill's fill now almost transparent, the smoke is a whisper rather than a layer: it gives
    // the centre a little weight without turning the island back into a dark slab.
    const float depthStrength = theme.rim * 0.15f;
    const D2D1_GRADIENT_STOP depthStops[3] = {
        {0.0f, D2D1::ColorF(0.005f, 0.012f, 0.030f, depthStrength)},
        {0.62f, D2D1::ColorF(0.005f, 0.012f, 0.030f, depthStrength * 0.42f)},
        {1.0f, D2D1::ColorF(0.005f, 0.012f, 0.030f, 0.0f)},
    };
    g_cache.depth = MakeRadial(context, depthStops, 3);

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
    // which otherwise shows as a pale seam against the desktop.
    if (g_cache.solid) {
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

void DrawSmokedDepth(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape, const Theme& theme,
                     float opacity) {
    if (!context || opacity <= 0.01f || theme.rim <= 0.001f) return;
    const float width = shape.rect.right - shape.rect.left;
    const float height = shape.rect.bottom - shape.rect.top;
    if (width < 6.0f || height < 6.0f) return;
    if (!EnsureMaterial(context, theme) || !g_cache.depth) return;

    PlaceRadial(g_cache.depth, (shape.rect.left + shape.rect.right) * 0.5f,
                (shape.rect.top + shape.rect.bottom) * 0.5f, width * 0.5f, height * 0.5f, opacity);
    context->FillRoundedRectangle(&shape, g_cache.depth);
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
    } else if (g_cache.topWash) {
        PlaceLinear(g_cache.topWash, shape.rect.left, shape.rect.top, shape.rect.top + topBand, opacity);
        context->FillRoundedRectangle(&shape, g_cache.topWash);
    }
    if (g_cache.bottomShade) {
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
    // "悬停时只增强少量高光并进行轻微偏移").
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
    // ONE soft core, offset by the pointer. The wide wash in DrawSurfaceLighting is the reflection;
    // this is the brighter source inside it. It is narrow on purpose: it must die out well before the
    // caps, or the cap's vertical edge cuts it.
    const Blob blobs[3] = {
        {0.34f, 0.20f, 0.20f, 0.55f, 0.95f},
        {0.54f, 0.15f, 0.24f, 0.40f, 0.70f},
        {0.74f, 0.26f, 0.16f, 0.48f, 0.50f},
    };
    // The lens theme gets ONE narrow core inside the wide wash: three overlapping blobs read as lumps
    // along the top edge, and any of them reaching the cap is cut by the cap's vertical edge.
    const Blob lensBlob = {0.33f, 0.16f, 0.15f, 0.42f, 0.85f};
    const int blobCount = theme.lens ? 1 : 3;
    for (int i = 0; i < blobCount; ++i) {
        const Blob& blob = theme.lens ? lensBlob : blobs[i];
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
}

void DrawEdgeTints(ID2D1DeviceContext2* context, const D2D1_ROUNDED_RECT& shape, const Theme& theme,
                   float opacity) {
    if (!context || opacity <= 0.01f || theme.tint <= 0.001f) return;
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
    if (!EnsureMaterial(context, theme) || !g_cache.sweep) return;

    const float band = min(height * 0.45f, 20.0f);
    const float centreX = SpecularCentreX(shape.rect.left, shape.rect.right, pointerFraction);
    const float centreY = shape.rect.top + band * 0.5f;
    // A tighter, rounder pool than the old wide smear: 45 DIP is what reads as a drop of light on the
    // surface rather than as a band sliding along it.
    PlaceRadial(g_cache.sweep, centreX, centreY, std::min(width * 0.30f, 45.0f), band * 1.6f, opacity);
    context->FillRoundedRectangle(&shape, g_cache.sweep);
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

}  // namespace island
