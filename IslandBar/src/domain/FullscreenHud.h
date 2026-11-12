// =====================================================================
//  FullscreenHud.h -- the three things a full-screen game may see
//
//  A full-screen application is a separate product mode, not another
//  collapsed IslandState. Calls outrank music, and every other combination
//  is absent. Keeping the decision and its geometry pure makes the window
//  region, renderer and self-test consume the same answer.
// =====================================================================
#pragma once

#include "domain/BarShape.h"
#include "domain/Geometry.h"

namespace island {

enum class FullscreenHudMode : int {
    None = -1,      // not full screen: the ordinary IslandBar state machine owns the UI
    Hidden = 0,
    MusicNotch = 1,
    CallOrb = 2,
};

inline FullscreenHudMode FullscreenHudModeFor(bool fullscreen, bool callActive, bool musicPlaying) {
    if (!fullscreen) return FullscreenHudMode::None;
    if (callActive) return FullscreenHudMode::CallOrb;
    if (musicPlaying) return FullscreenHudMode::MusicNotch;
    return FullscreenHudMode::Hidden;
}

inline const wchar_t* FullscreenHudModeName(FullscreenHudMode mode) {
    switch (mode) {
        case FullscreenHudMode::None: return L"none";
        case FullscreenHudMode::Hidden: return L"hidden";
        case FullscreenHudMode::MusicNotch: return L"music-notch";
        case FullscreenHudMode::CallOrb: return L"call-orb";
    }
    return L"?";
}

inline constexpr float kFullscreenMusicWidthDip = 300.0f;
inline constexpr float kFullscreenMusicDefaultLineHeightDip = 14.0f;
inline constexpr float kFullscreenHudSidePadDip = 12.0f;
inline constexpr float kFullscreenMusicVerticalPadDip = 3.0f;
inline constexpr float kFullscreenCallLayoutDip = 30.0f;
inline constexpr float kFullscreenCallVisualDip = 26.0f;

struct FullscreenHudLayout {
    RoundedBox box;
    RoundedBox material;
    RectF lyric;
    RectF wave;
    BarShape shape = BarShape::Capsule;
    bool valid = false;
};

inline constexpr float kFullscreenMusicTextWidthDip =
    kFullscreenMusicWidthDip - 2.0f * kFullscreenHudSidePadDip;

// `screen` is the current monitor rectangle in DIP. Music uses the existing notch silhouette; the
// call uses a stadium whose equal width and height make it a circle.
inline FullscreenHudLayout FullscreenHudLayoutFor(FullscreenHudMode mode,
                                                  const RectF& screen,
                                                  float musicLineHeightDip = 0.0f) {
    FullscreenHudLayout layout;
    if (mode != FullscreenHudMode::MusicNotch && mode != FullscreenHudMode::CallOrb) return layout;

    const float centreX = (screen.left + screen.right) * 0.5f;
    if (mode == FullscreenHudMode::CallOrb) {
        layout.box.rect = {centreX - kFullscreenCallLayoutDip * 0.5f, screen.top,
                           centreX + kFullscreenCallLayoutDip * 0.5f,
                           screen.top + kFullscreenCallLayoutDip};
        layout.box.radiusX = kFullscreenCallLayoutDip * 0.5f;
        layout.box.radiusY = layout.box.radiusX;
        layout.shape = BarShape::Capsule;
        const float inset = (kFullscreenCallLayoutDip - kFullscreenCallVisualDip) * 0.5f;
        layout.material = layout.box;
        layout.material.rect.left += inset;
        layout.material.rect.top += inset;
        layout.material.rect.right -= inset;
        layout.material.rect.bottom -= inset;
        layout.material.radiusX = kFullscreenCallVisualDip * 0.5f;
        layout.material.radiusY = layout.material.radiusX;
    } else {
        const float measuredLineHeight = musicLineHeightDip > 0.0f
                                             ? musicLineHeightDip
                                             : kFullscreenMusicDefaultLineHeightDip;
        const float height = measuredLineHeight + 2.0f * kFullscreenMusicVerticalPadDip;
        layout.box.rect = {centreX - kFullscreenMusicWidthDip * 0.5f, screen.top,
                           centreX + kFullscreenMusicWidthDip * 0.5f,
                           screen.top + height};
        layout.box.radiusX = height * 0.5f;
        layout.box.radiusY = layout.box.radiusX;
        layout.shape = BarShape::Notch;
        layout.lyric = {layout.box.rect.left + kFullscreenHudSidePadDip,
                        layout.box.rect.top + kFullscreenMusicVerticalPadDip,
                        layout.box.rect.right - kFullscreenHudSidePadDip,
                        layout.box.rect.bottom - kFullscreenMusicVerticalPadDip};
        // The wave is the lyric's backdrop, not a second row of content.
        layout.wave = layout.lyric;
        layout.material = BarMaterialBoxFor(layout.box, layout.shape);
    }
    layout.valid = layout.box.rect.right > layout.box.rect.left;
    return layout;
}

} // namespace island
