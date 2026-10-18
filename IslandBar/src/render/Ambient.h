// =====================================================================
//  Ambient.h
//  The island's sky: rain that falls across the glass, a sunset that
//  glows from the bottom, stars at night, a slow sun in the afternoon.
//
//  WHY. The island has a real weather code and the site's real sunrise and
//  sunset, and a glass surface that shows nothing of either. The request was
//  "can it change with the weather and the time -- rain on the island when it rains, an afterglow at
//  dusk when it is clear".
//  WHAT DECIDES THE LOOK, in one pure function so it can be asserted:
//  the WMO code first (precipitation wins over the sky -- rain at night is
//  still rain), then the clear sky is split by the REAL sunset window into
//  Sunset / Night / ClearDay. The fallback sunrise/sunset (06:00/18:00) is
//  only used until the daily answer arrives.
//
//  The layer draws INSIDE the pill, under the status line, at the strength
//  the caller passes (the bar's contentOpacity), so the effects arrive and
//  leave with the rest of the content and never cover a word.
// =====================================================================
#pragma once

#include <d2d1_3.h>

namespace island {

struct Theme;

enum class AmbientKind {
    ClearDay,
    Sunset,
    Night,
    Rain,
    Snow,
    Cloud,
    Fog,
    Thunder,
};

inline constexpr int kAmbientFallbackSunriseMinutes = 6 * 60;
inline constexpr int kAmbientFallbackSunsetMinutes = 18 * 60;

// The pure decision: the WMO weather code and the site's own sunrise/sunset (minutes since local
// midnight) -> what the island should show. A negative sunrise or sunset means "not fetched yet" and
// uses the fallback.
AmbientKind AmbientKindFor(int weatherCode, int nowMinutes, int sunriseMinutes, int sunsetMinutes);

// The ambient layer. `phaseSeconds` is a free-running clock for the particles; `opacity` is the
// content strength (0 hides the layer). Drawn clipped to `shape`.
void DrawAmbient(ID2D1DeviceContext* context, const D2D1_ROUNDED_RECT& shape, AmbientKind kind,
                 float phaseSeconds, float opacity, const Theme& theme);

// The card's sky: the SAME kind and phase as the island's, set once per frame by the renderer. The cards
// are the same glass and the user asked for the same weather in them -- "改成和灵动岛一样根据天气 时间
// 变化" -- so the effect is drawn inside the card's shape, under its facets and words, instead of a fixed
// decoration that would disagree with the bar two inches above it.
void SetCardAmbient(AmbientKind kind, float phaseSeconds);
void DrawCardAmbient(ID2D1DeviceContext* context, const D2D1_ROUNDED_RECT& shape, float opacity,
                     const Theme& theme);

}  // namespace island

