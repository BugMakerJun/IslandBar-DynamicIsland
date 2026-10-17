// =====================================================================
//  Theme.h
//  The material, as data: the colours and edges the bar is drawn from, in a list the user can
//  switch between -- and can add to, by dropping a text file in `themes\`.
//
//  WHERE THIS CAME FROM. A review brief asked for "Liquid Glass": a translucent dark surface, a soft
//  highlight along the top, a hairline edge, no heavy borders -- and for the palette to be black,
//  deep blue and ice blue. Everything it asks for is drawing, and all of that drawing reads a handful
//  of colours, so the colours are one struct rather than a style pass: switching a theme re-colours
//  every surface at once and cannot leave two of them disagreeing.
//
//  WHETHER THE DESKTOP BEHIND THE BAR IS BLURRED IS ALSO A THEME FIELD (`backdrop`), and it was "no"
//  for a while because this project's window could not have a backdrop: acrylic blurs the whole window
//  rectangle, and that window is a full-width strip. The measurement that changed it is in HANDOFF
//  8.166: a SECOND, island-sized window of the same per-pixel-alpha kind shows the material, needs no
//  renderer at all, and can follow the island's geometry for under 1 ms a step. So a theme may now ask
//  for it; the drawing in this struct stays the material either way, which is what keeps the fallback
//  honest when the window cannot exist.
//
//  THE CONTRAST FLOOR IS PART OF THE FORMAT, not a note in a document: 'no over-transparency that
//  makes the text unreadable' is checked by ThemeInkContrast, and the self-test runs it over every
//  built-in theme. A user theme is loaded whatever it says -- it is the user's screen -- but the
//  number is in the log when it is below the floor.
// =====================================================================
#pragma once

#include <windows.h>

#include <d2d1_3.h>

#include <string>

namespace island {

struct Theme {
    wchar_t name[32] = {};
    // The bar's own surface. Two stops rather than one, because a flat fill at 92% is what the bar
    // had and it reads as a black rectangle: the plan's "reflected light" is what the second stop and
    // the gloss below are for.
    D2D1_COLOR_F pillTop = {};
    D2D1_COLOR_F pillBottom = {};
    float gloss = 0.0f;             // 0..1: strength of the soft highlight along the pill's top
    // The cards -- the system panel, the tray page, the music card, a notification. One surface for
    // all of them, which is what makes them read as the same object at different sizes.
    D2D1_COLOR_F cardFill = {};
    float cardEdge = 0.0f;          // 0..1: the hairline around a card, as an alpha of the ink
    D2D1_COLOR_F accent = {};       // "on" tiles, slider fills, the meters
    D2D1_COLOR_F ink = {};          // text and glyphs -- the colour everything is written in
    // The two layers of the material that are neither colour nor edge (GlassMaterial.h): how far the soft
    // shadow reaches under the shape, and how strong the grain over it is. Both are 0 in 默认, so the
    // built-in that existed before themes keeps its exact pixels -- a theme opts into a material.
    float shadow = 0.0f;            // 0..1: opacity of the shadow at its darkest
    float grain = 0.0f;             // 0..1: the grain's own alpha
    // --- the liquid-glass layers (round 53, the user's "苹果那种液态玻璃") -----------------------
    //
    // THREE LAYERS, all of them drawn rather than sampled. What Apple's material actually does is refract
    // the content behind it, and that needs pixels from behind the window -- which this window cannot have
    // (three APIs measured, see the note on 玻璃 below and HANDOFF 8.163). So the three things that CAN be
    // drawn are named here, each driven by its own field so a theme opts in and 默认 keeps its pixels.
    //
    //   sweep  a specular highlight along the top edge whose centre FOLLOWS THE POINTER across the pill
    //          (the one part of the Apple material that is about the pointer rather than the background);
    //   rim    a double edge: a bright hairline just inside the shape and a faint dark line on the
    //          outside, which is what reads as the THICKNESS of a piece of glass;
    //   tint   how much of the WALLPAPER's own average colour is mixed into the surface -- read ONCE from
    //          the wallpaper file itself (shell/WallpaperTint.*), never captured, so a solid-colour
    //          wallpaper still gives the bar something of its own colour to sit on.
    float sweep = 0.0f;             // 0..1
    float rim = 0.0f;               // 0..1
    float tint = 0.0f;              // 0..1
    // Whether the island sits on the REAL blurred desktop (app/GlassWindow) or only on the painted
    // material above. Off by default: 默认/冰蓝/石墨 keep their exact pixels and cost no second window.
    // Its fill alphas MUST be low enough for a backdrop to show through -- 默认's 0.92/0.98 would hide
    // the material completely, which is why only 玻璃 turns this on.
    bool backdrop = false;
};

// The theme in force.
//
// Process-wide, and that is a deliberate simplification: the material is one decision for the whole
// bar, every drawing call reads it, none of them owns it, and threading it through the renderer's
// interfaces would add a parameter to twenty signatures to express a value that cannot differ
// between them.
const Theme& CurrentTheme();

// The list the 外观 tile walks: the built-ins first, then whatever `themes\*.txt` holds.
int ThemeCount();
const Theme& ThemeAt(int index);
int CurrentThemeIndex();

// Applies the next theme in that list and returns it (wrapping at the end).
const Theme& CycleTheme();

// Where the choice is remembered: `<exe dir>\theme.txt`, the same place and the same idea as
// weather.txt. An unknown name leaves the current theme alone and says so.
bool SaveThemeChoice(const wchar_t* name);
void ApplyRememberedTheme();

// The ink at an alpha, which is what most of the drawing asks for: a hundred call sites say "white at
// 0.72" and the theme is what decides which colour that is.
D2D1_COLOR_F ThemeInk(float alpha);

// The contrast floor the ink has to keep against its own surface (WCAG's 4.5:1 for body text). Named so
// that the theme loader's warning, the tint clamp and the self-test all quote one number.
inline constexpr float kInkContrastFloor = 4.5f;

// WCAG contrast between the ink and the card, as it would appear over a BRIGHT desktop -- the worst
// case for light text on a translucent surface, and the one the plan's "do not over-transparent the
// background" is about.
float ThemeInkContrast(const Theme& theme);

// A surface colour with the WALLPAPER's colour mixed into it, CLAMPED so the ink keeps its contrast floor.
//
// The clamp is the whole reason this is a function and not one lerp at the call site. `theme.tint` lets a
// bright wallpaper lighten the pill, and a light enough pill makes the bar's own text unreadable -- which
// is the failure the contrast floor exists to prevent (see ThemeInkContrast). So the mix is walked BACK
// toward the theme's own colour until the contrast passes again; a wallpaper that would break it simply
// tints less. Pure, so the self-test can assert it against a deliberately bright and a deliberately dark
// wallpaper.
D2D1_COLOR_F TintedFill(const Theme& theme, const D2D1_COLOR_F& fill, const D2D1_COLOR_F& wallpaper,
                        float amount);

// The wallpaper's average colour, read once at startup (shell/WallpaperTint.*) and handed here so the
// renderer needs no new parameter: one decision for the whole bar, the same rule the theme follows.
void SetWallpaperTint(const D2D1_COLOR_F& tint, bool fromWallpaper);
const D2D1_COLOR_F& WallpaperTint();
bool WallpaperTintIsKnown();

// Reads a theme from the text of a `themes\*.txt`. PURE, so the format is testable without files:
// one `key=value` per line, `#` for comments, components 0..1 and either three (RGB) or four (RGBA).
bool ThemeFromText(const char* utf8, Theme* out, std::string* why);

// The names of the built-in themes, for the self-test and the log.
int BuiltInThemeCount();
const Theme& BuiltInTheme(int index);

} // namespace island
