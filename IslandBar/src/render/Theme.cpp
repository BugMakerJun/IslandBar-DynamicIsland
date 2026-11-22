// =====================================================================
//  Theme.cpp -- see the header for what a theme is and what it is not.
// =====================================================================
#include "render/Theme.h"

#include <cmath>

namespace island {
namespace {

// The bar has one material. It remains a Theme value because the renderer already consumes these
// colours through one small data structure; there is no second theme, file format or selector.
const Theme kNiriTheme = {
    L"Niri", {1.00f, 1.00f, 1.00f, 1.00f}, {1.00f, 1.00f, 1.00f, 1.00f}, 0.00f,
    {0.96f, 0.95f, 0.98f, 0.98f}, 0.09f, {0.42f, 0.30f, 0.72f, 0.96f},
    {0.12f, 0.10f, 0.17f, 1.0f}, 0.06f, 0.0f, 0.0f, 0.0f, 0.0f,
    {1.0f, 1.0f, 1.0f, 1.0f}, false};
const Theme* g_themeForSelfTest = nullptr;

// --- helpers -----------------------------------------------------------------------------------

float Clamp01(float value) { return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value); }

// The relative luminance of a colour, on the sRGB curve the WCAG contrast rule is written in.
float Luminance(const D2D1_COLOR_F& colour) {
    const auto channel = [](float c) {
        c = Clamp01(c);
        return c <= 0.03928f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    return 0.2126f * channel(colour.r) + 0.7152f * channel(colour.g) + 0.0722f * channel(colour.b);
}

} // namespace

// --- the theme in force ------------------------------------------------------------------------

const Theme& CurrentTheme() { return g_themeForSelfTest ? *g_themeForSelfTest : kNiriTheme; }

void SetThemeForSelfTest(const Theme* theme) { g_themeForSelfTest = theme; }

D2D1_COLOR_F ThemeInk(float alpha) {
    const D2D1_COLOR_F ink = CurrentTheme().ink;
    return D2D1::ColorF(ink.r, ink.g, ink.b, Clamp01(alpha) * Clamp01(ink.a));
}

namespace {
// 1 = light backdrop, so the ink is the theme's dark colour; 0 = dark backdrop, so it is the highlight.
float g_pillInkMix = 1.0f;
float g_pillInkTarget = 1.0f;
ULONGLONG g_pillInkAt = 0;

// The smoothing advances on READ, not on Set: the test (and any caller) sets the backdrop, waits, then
// asks for the colour; moving the mix only inside the setter froze it at the value it had when the
// backdrop last changed, which left white words on a bright card.
void AdvancePillInk() {
    const ULONGLONG now = GetTickCount64();
    if (g_pillInkAt == 0) {
        g_pillInkAt = now;
        return;
    }
    const float dt = static_cast<float>(now - g_pillInkAt) / 1000.0f;
    if (dt <= 0.0f) return;
    g_pillInkAt = now;
    g_pillInkMix += (g_pillInkTarget - g_pillInkMix) * Clamp01(dt * 4.0f);
}
}

void SetPillInkBackdrop(float luminance) {
    // The crossover sits low on purpose: a mid-tone desktop (a blue sky, a photograph) still takes the
    // theme's dark ink, which is the light-glass look; only genuinely dark content switches to the light
    // ink. White text on a mid-blue sky reads as washed out, not as contrast.
    g_pillInkTarget = Clamp01((luminance - 0.20f) / 0.15f);
}

D2D1_COLOR_F PillInk(float alpha) {
    const Theme& theme = CurrentTheme();
    if (!theme.lens) return ThemeInk(alpha);
    AdvancePillInk();
    // A theme with a substantial body (above ~0.15 alpha) has its own surface: the ink is the theme's
    // dark ink, not the adaptive one. The adaptive crossover exists for the clear lens, where the
    // backdrop IS the surface and a mixed dark/light photo can make a single ink wrong on half the text.
    if (theme.pillTop.a > 0.15f || theme.pillBottom.a > 0.15f) return ThemeInk(alpha);
    const float m = Clamp01(g_pillInkMix);
    return D2D1::ColorF(theme.ink.r * m + theme.highlight.r * (1.0f - m),
                        theme.ink.g * m + theme.highlight.g * (1.0f - m),
                        theme.ink.b * m + theme.highlight.b * (1.0f - m), Clamp01(alpha));
}

namespace {
float g_cardInkMix = 1.0f;
float g_cardInkTarget = 1.0f;
ULONGLONG g_cardInkAt = 0;

void AdvanceCardInk() {
    const ULONGLONG now = GetTickCount64();
    if (g_cardInkAt == 0) {
        g_cardInkAt = now;
        return;
    }
    const float dt = static_cast<float>(now - g_cardInkAt) / 1000.0f;
    if (dt <= 0.0f) return;
    g_cardInkAt = now;
    g_cardInkMix += (g_cardInkTarget - g_cardInkMix) * Clamp01(dt * 4.0f);
}
}

void SetCardInkBackdrop(float luminance) {
    // The same crossover the pill uses -- a mid-tone desktop keeps the theme's dark ink -- but fed from
    // the CARD's own sample: the cards hang below the bar, over a different part of the desktop.
    g_cardInkTarget = Clamp01((luminance - 0.20f) / 0.15f);
}

D2D1_COLOR_F CardInk(float alpha) {
    const Theme& theme = CurrentTheme();
    if (!theme.lens) return ThemeInk(alpha);
    AdvanceCardInk();
    if (theme.pillTop.a > 0.15f || theme.pillBottom.a > 0.15f) return ThemeInk(alpha);
    const float m = Clamp01(g_cardInkMix);
    return D2D1::ColorF(theme.ink.r * m + theme.highlight.r * (1.0f - m),
                        theme.ink.g * m + theme.highlight.g * (1.0f - m),
                        theme.ink.b * m + theme.highlight.b * (1.0f - m), Clamp01(alpha));
}

D2D1_COLOR_F ThemeHighlight(float alpha) {
    const D2D1_COLOR_F& colour = CurrentTheme().highlight;
    return D2D1::ColorF(colour.r, colour.g, colour.b, Clamp01(colour.a * alpha));
}

float ThemeInkContrast(const Theme& theme) {
    // The card's fill over a BRIGHT desktop (0.9 grey): the card is translucent, so this is what the
    // text is actually read against on the wallpaper that gives it the least help.
    const float a = Clamp01(theme.cardFill.a);
    const float backdrop = 0.9f;
    const D2D1_COLOR_F effective = D2D1::ColorF(theme.cardFill.r * a + backdrop * (1.0f - a),
                                               theme.cardFill.g * a + backdrop * (1.0f - a),
                                               theme.cardFill.b * a + backdrop * (1.0f - a), 1.0f);
    const float first = Luminance(theme.ink);
    const float second = Luminance(effective);
    const float lighter = first > second ? first : second;
    const float darker = first > second ? second : first;
    return (lighter + 0.05f) / (darker + 0.05f);
}

// --- the liquid-glass layers: the wallpaper's colour, and the clamp that keeps the text readable -------

namespace {

// What the ink would be read against if a surface of this colour were the card: the same rule
// ThemeInkContrast uses, so the clamp below and the floor the self-test asserts are the same question.
// `backdrop` is what shows through the surface where it is translucent: the card's floor is defined
// against a BRIGHT desktop, and the tray menu asks the same question against the worst desktop that
// sheet can meet (see TrayMenuInkContrast).
float ContrastAgainst(const D2D1_COLOR_F& surface, const D2D1_COLOR_F& ink, float backdrop = 0.9f) {
    const float a = Clamp01(surface.a);
    const D2D1_COLOR_F effective = D2D1::ColorF(surface.r * a + backdrop * (1.0f - a),
                                              surface.g * a + backdrop * (1.0f - a),
                                              surface.b * a + backdrop * (1.0f - a), 1.0f);
    const float first = Luminance(ink);
    const float second = Luminance(effective);
    const float lighter = first > second ? first : second;
    const float darker = first > second ? second : first;
    return (lighter + 0.05f) / (darker + 0.05f);
}

D2D1_COLOR_F Mix(const D2D1_COLOR_F& from, const D2D1_COLOR_F& to, float amount) {
    const float t = Clamp01(amount);
    return D2D1::ColorF(from.r + (to.r - from.r) * t, from.g + (to.g - from.g) * t,
                        from.b + (to.b - from.b) * t, from.a + (to.a - from.a) * t);
}

}  // namespace

bool CardInkIsLight() {
    const D2D1_COLOR_F ink = CardInk(1.0f);
    return Luminance(ink) > 0.5f;
}

D2D1_COLOR_F TrayMenuBody(const D2D1_COLOR_F& ink, float opacity) {
    // The two bodies: dark under light words, light under dark words. The alphas are what
    // TrayMenuInkContrast is computed against, so if one moves the other has to be re-measured.
    //
    // The flip happens at the ink's own crossover, which takes ~250 ms: for that moment the words are
    // mid-grey while the body has already changed tone, exactly as the card's own words already are.
    const D2D1_COLOR_F dark = D2D1::ColorF(0.05f, 0.06f, 0.09f, Clamp01(0.92f * opacity));
    const D2D1_COLOR_F light = D2D1::ColorF(0.97f, 0.98f, 1.00f, Clamp01(0.90f * opacity));
    return Luminance(ink) > 0.5f ? dark : light;
}

float TrayMenuInkContrast(const D2D1_COLOR_F& ink) {
    // The worst desktop each sheet can meet: the dark sheet is translucent over whatever is bright, the
    // light one over whatever is dark. Same reasoning as ThemeInkContrast, one surface further in.
    const float backdrop = Luminance(ink) > 0.5f ? 0.90f : 0.05f;
    return ContrastAgainst(TrayMenuBody(ink, 1.0f), ink, backdrop);
}

D2D1_COLOR_F TintedFill(const Theme& theme, const D2D1_COLOR_F& fill, const D2D1_COLOR_F& wallpaper,
                        float amount) {
    const float wanted = Clamp01(amount) * Clamp01(theme.tint);
    if (wanted <= 0.001f) return fill;

    // WALKED BACK, not clamped in luminance: the strength of the mix is what gives way, so a bright
    // wallpaper tints the pill a little instead of being refused outright. Four halvings are enough for
    // any wallpaper this side of white, and the loop is bounded rather than a bisection because the
    // answer only has to be good enough to pass a floor -- not exact.
    float applied = wanted;
    for (int step = 0; step < 4; ++step) {
        D2D1_COLOR_F candidate = Mix(fill, wallpaper, applied);
        // Tinting changes the COLOUR, not how much of the surface there is: a fill at alpha 0 stays
        // invisible (the glass theme), and a nearly clear one stays nearly clear. Mixing the wallpaper's
        // alpha 1 in raised a transparent fill to 0.25 -- a flat grey-blue scrim across the whole pill,
        // which is the "雾蒙蒙" the lens was hidden under.
        candidate.a = fill.a;
        if (ContrastAgainst(candidate, theme.ink) >= kInkContrastFloor) return candidate;
        applied *= 0.5f;
    }
    return fill;
}

namespace {
D2D1_COLOR_F g_wallpaperTint = D2D1::ColorF(0.5f, 0.5f, 0.5f, 1.0f);
bool g_wallpaperKnown = false;
}  // namespace

void SetWallpaperTint(const D2D1_COLOR_F& tint, bool fromWallpaper) {
    g_wallpaperTint = tint;
    g_wallpaperKnown = fromWallpaper;
}

const D2D1_COLOR_F& WallpaperTint() { return g_wallpaperTint; }
bool WallpaperTintIsKnown() { return g_wallpaperKnown; }

} // namespace island
