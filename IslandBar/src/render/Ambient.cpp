// =====================================================================
//  Ambient.cpp
//  The island's sky. See Ambient.h for why it exists and what decides it.
// =====================================================================
#include "render/Ambient.h"

#include "render/Theme.h"

#include <cmath>

namespace island {
namespace {

float Fract(float value) {
    return value - std::floor(value);
}

D2D1_COLOR_F WithAlpha(const D2D1_COLOR_F& colour, float alpha) {
    return D2D1::ColorF(colour.r, colour.g, colour.b, alpha);
}

// The card's sky, set once per frame by the renderer (see Ambient.h): the same kind and phase as the
// island's, so the two surfaces cannot show different weather.
AmbientKind g_cardAmbientKind = AmbientKind::ClearDay;
float g_cardAmbientPhase = 0.0f;

// A soft radial pool, clipped to the pill. Used for the sun, the afterglow's horizon, the moon and the
// cloud shadows: one brush, one fill, no particles.
void DrawGlow(ID2D1DeviceContext* context, const D2D1_ROUNDED_RECT& shape, float cx, float cy,
              float radiusFraction, const D2D1_COLOR_F& colour) {
    if (colour.a <= 0.001f) return;
    const float width = shape.rect.right - shape.rect.left;
    const float height = shape.rect.bottom - shape.rect.top;
    const float radius = (width > height ? width : height) * radiusFraction;

    const D2D1_GRADIENT_STOP stops[2] = {
        {0.0f, colour},
        {1.0f, D2D1::ColorF(colour.r, colour.g, colour.b, 0.0f)},
    };
    ID2D1GradientStopCollection* collection = nullptr;
    if (FAILED(context->CreateGradientStopCollection(stops, 2, &collection)) || !collection) return;
    ID2D1RadialGradientBrush* brush = nullptr;
    context->CreateRadialGradientBrush(
        D2D1::RadialGradientBrushProperties(
            D2D1::Point2F(shape.rect.left + width * cx, shape.rect.top + height * cy),
            D2D1::Point2F(0.0f, 0.0f), radius, radius),
        collection, &brush);
    collection->Release();
    if (brush) {
        context->FillRoundedRectangle(&shape, brush);
        brush->Release();
    }
}

} // namespace

AmbientKind AmbientKindFor(int weatherCode, int nowMinutes, int sunriseMinutes, int sunsetMinutes) {
    // Precipitation wins over the sky: a rainy night is still rain.
    if (weatherCode >= 95 && weatherCode <= 99) return AmbientKind::Thunder;
    if ((weatherCode >= 71 && weatherCode <= 77) || weatherCode == 85 || weatherCode == 86) {
        return AmbientKind::Snow;
    }
    if ((weatherCode >= 51 && weatherCode <= 67) || (weatherCode >= 80 && weatherCode <= 82)) {
        return AmbientKind::Rain;
    }
    if (weatherCode == 45 || weatherCode == 48) return AmbientKind::Fog;
    if (weatherCode == 2 || weatherCode == 3) return AmbientKind::Cloud;

    // A clear (or mainly clear) sky: the REAL sunset decides. The fallback is only for the first
    // minutes before the daily answer arrives.
    if (sunriseMinutes < 0 || sunsetMinutes < 0) {
        sunriseMinutes = kAmbientFallbackSunriseMinutes;
        sunsetMinutes = kAmbientFallbackSunsetMinutes;
    }
    const int sunsetStart = sunsetMinutes - 45;
    // The afterglow lasts longer than the sun does: on this machine the real sunset was 16:52 and the
    // clock said 17:24, so a +30 window had already turned the island to NIGHT while the sky outside was
    // still orange. +60 keeps the sunset through the afterglow and only then hands over to the stars.
    const int sunsetEnd = sunsetMinutes + 60;
    if (nowMinutes >= sunsetStart && nowMinutes <= sunsetEnd) return AmbientKind::Sunset;
    if (nowMinutes < sunriseMinutes || nowMinutes > sunsetEnd) return AmbientKind::Night;
    return AmbientKind::ClearDay;
}

void DrawAmbient(ID2D1DeviceContext* context, const D2D1_ROUNDED_RECT& shape, AmbientKind kind,
                 float phaseSeconds, float opacity, const Theme& theme) {
    if (!context || opacity <= 0.01f) return;
    const float width = shape.rect.right - shape.rect.left;
    const float height = shape.rect.bottom - shape.rect.top;
    if (width < 8.0f || height < 8.0f) return;

    // Everything is clipped to the capsule: a drop outside the rounded end would be a stray line on
    // the desktop, not weather.
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

    const D2D1_COLOR_F ink = theme.highlight;

    // THE RAIN, and it has to READ as motion: 30 streaks in two depths, slanted, falling. Two depths
    // rather than one because a single row of identical lines reads as a texture, not as weather.
    const auto rain = [&](float strength) {
        ID2D1SolidColorBrush* brush = nullptr;
        if (FAILED(context->CreateSolidColorBrush(WithAlpha(ink, 0.34f * opacity * strength),
                                                  &brush)) ||
            !brush) {
            return;
        }
        for (int i = 0; i < 30; ++i) {
            const float seed = static_cast<float>(i);
            const float depth = (i % 2) ? 1.0f : 1.55f;
            const float x = shape.rect.left + Fract(seed * 0.377f + phaseSeconds * 0.07f) * width;
            const float fall = Fract(seed * 0.713f + phaseSeconds * (0.55f + 0.20f * depth));
            const float y = shape.rect.top + fall * (height + 24.0f) - 12.0f;
            const float length = 5.0f + static_cast<float>(i % 4) * 1.8f * depth;
            context->DrawLine(D2D1::Point2F(x, y),
                              D2D1::Point2F(x - 1.6f * depth, y + length), brush, 0.9f * depth);
        }
        brush->Release();
    };

    switch (kind) {
        case AmbientKind::Rain:
            rain(1.0f);
            break;

        case AmbientKind::Thunder:
            rain(1.0f);
            {
                const float cycle = std::fmod(phaseSeconds, 4.0f);
                if (cycle < 0.09f) {
                    ID2D1SolidColorBrush* flash = nullptr;
                    if (SUCCEEDED(context->CreateSolidColorBrush(
                            D2D1::ColorF(0.85f, 0.90f, 1.0f, 0.14f * opacity), &flash)) &&
                        flash) {
                        context->FillRoundedRectangle(&shape, flash);
                        flash->Release();
                    }
                }
            }
            break;

        case AmbientKind::Snow: {
            ID2D1SolidColorBrush* flake = nullptr;
            if (SUCCEEDED(context->CreateSolidColorBrush(WithAlpha(ink, 0.55f * opacity), &flake)) &&
                flake) {
                for (int i = 0; i < 24; ++i) {
                    const float seed = static_cast<float>(i);
                    const float x = shape.rect.left +
                                    Fract(seed * 0.413f + 0.03f * std::sin(phaseSeconds * 0.5f + seed)) *
                                        width;
                    const float fall = Fract(seed * 0.617f + phaseSeconds * 0.10f);
                    const float y = shape.rect.top + fall * height;
                    const float r = 0.8f + static_cast<float>(i % 3) * 0.45f;
                    context->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), r, r), flake);
                }
                flake->Release();
            }
            break;
        }

        case AmbientKind::Sunset: {
            // FROM THE TOP, and across the whole pill: the sky's light comes from above, so the warm
            // band starts at the top edge and fades towards the bottom. It used to rise from the
            // bottom edge, which read as a coloured strip under the island rather than as light in it.
            const D2D1_GRADIENT_STOP stops[3] = {
                {0.0f, D2D1::ColorF(1.0f, 0.55f, 0.28f, 0.30f * opacity)},
                {0.45f, D2D1::ColorF(1.0f, 0.46f, 0.28f, 0.14f * opacity)},
                {1.0f, D2D1::ColorF(1.0f, 0.40f, 0.25f, 0.0f)},
            };
            ID2D1GradientStopCollection* collection = nullptr;
            if (SUCCEEDED(context->CreateGradientStopCollection(stops, 3, &collection)) && collection) {
                ID2D1LinearGradientBrush* brush = nullptr;
                context->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, shape.rect.top),
                                                        D2D1::Point2F(0.0f, shape.rect.bottom)),
                    collection, &brush);
                collection->Release();
                if (brush) {
                    context->FillRoundedRectangle(&shape, brush);
                    brush->Release();
                }
            }
            // The sun itself, low in the top-right corner.
            DrawGlow(context, shape, 0.74f, 0.02f, 0.52f,
                     D2D1::ColorF(1.0f, 0.72f, 0.38f, 0.22f * opacity));
            break;
        }

        case AmbientKind::Night: {
            ID2D1SolidColorBrush* deep = nullptr;
            if (SUCCEEDED(context->CreateSolidColorBrush(
                    D2D1::ColorF(0.05f, 0.09f, 0.20f, 0.20f * opacity), &deep)) &&
                deep) {
                context->FillRoundedRectangle(&shape, deep);
                deep->Release();
            }
            ID2D1SolidColorBrush* star = nullptr;
            if (SUCCEEDED(context->CreateSolidColorBrush(WithAlpha(ink, 0.55f * opacity), &star)) &&
                star) {
                for (int i = 0; i < 18; ++i) {
                    const float seed = static_cast<float>(i);
                    const float x = shape.rect.left + Fract(seed * 0.531f + 0.11f) * width;
                    const float y = shape.rect.top + Fract(seed * 0.317f + 0.07f) * height * 0.72f;
                    const float twinkle = 0.55f + 0.45f * std::sin(phaseSeconds * 0.9f + seed * 1.7f);
                    const float r = 0.7f + 0.5f * twinkle;
                    context->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), r, r), star);
                }
                star->Release();
            }
            DrawGlow(context, shape, 0.82f, 0.14f, 0.34f,
                     D2D1::ColorF(0.80f, 0.88f, 1.0f, 0.10f * opacity));
            break;
        }

        case AmbientKind::Cloud: {
            const float drift = std::sin(phaseSeconds * 0.15f) * 0.05f;
            DrawGlow(context, shape, 0.28f + drift, 0.24f, 0.42f,
                     D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f * opacity));
            DrawGlow(context, shape, 0.64f - drift, 0.30f, 0.50f,
                     D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.07f * opacity));
            break;
        }

        case AmbientKind::Fog: {
            const D2D1_GRADIENT_STOP stops[2] = {
                {0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
                {1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f * opacity)},
            };
            ID2D1GradientStopCollection* collection = nullptr;
            if (SUCCEEDED(context->CreateGradientStopCollection(stops, 2, &collection)) && collection) {
                ID2D1LinearGradientBrush* brush = nullptr;
                context->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, shape.rect.top),
                                                        D2D1::Point2F(0.0f, shape.rect.bottom)),
                    collection, &brush);
                collection->Release();
                if (brush) {
                    context->FillRoundedRectangle(&shape, brush);
                    brush->Release();
                }
            }
            break;
        }

        case AmbientKind::ClearDay:
        default: {
            const float breathe = 0.78f + 0.22f * std::sin(phaseSeconds * 0.35f);
            DrawGlow(context, shape, 0.80f, 0.12f, 0.40f,
                     D2D1::ColorF(1.0f, 0.86f, 0.55f, 0.13f * opacity * breathe));
            break;
        }
    }

    if (clip) {
        context->PopLayer();
        clip->Release();
    }
}

void SetCardAmbient(AmbientKind kind, float phaseSeconds) {
    g_cardAmbientKind = kind;
    g_cardAmbientPhase = phaseSeconds;
}

void DrawCardAmbient(ID2D1DeviceContext* context, const D2D1_ROUNDED_RECT& shape, float opacity,
                     const Theme& theme) {
    DrawAmbient(context, shape, g_cardAmbientKind, g_cardAmbientPhase, opacity, theme);
}

}  // namespace island

