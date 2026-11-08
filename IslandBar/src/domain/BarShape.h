// =====================================================================
//  BarShape.h -- the bar's two collapsed shapes
//
//  WHY THIS EXISTS. Asked for on 2026-11-06 with BrimDeck's Notch/Capsule reference: the same island in
//  two forms. Capsule is what the bar has always been -- a floating stadium, one margin below the screen
//  edge, every corner rounded. Notch hangs from the screen's top edge: flush with it, its top corners cut
//  off by the display instead of rounded, its bottom corners still a stadium's.
//
//  HOW THE NOTCH IS DRAWN, because it is not a second geometry and the glass material is not touched:
//  the MATERIAL's box is the visible one plus one radius ABOVE the screen, and the display cuts it. A
//  stadium whose top circle is off-screen shows its straight sides and its bottom corners, which is
//  exactly a notch -- and the material layers, the theme, the lens and the shadow all keep working
//  because they are drawn from a rounded box as they always were. The VISIBLE box is unchanged, so the
//  content, the deck, the icon row, the card and every hit test still measure from it (see
//  BarMaterialBoxFor and its one caller, the frame loop).
//
//  THE SHAPE IS A SETTING, not a compile-time choice: it is stored in bar.txt beside theme.txt and
//  switched from the settings card (see app/BarSettings.cpp and the Settings level of the panel).
//
//  Pure by construction: this file is in domain/ and may never learn about a window.
// =====================================================================
#pragma once

#include "domain/Geometry.h"   // RoundedBox, kPillTopMarginDip

namespace island {

enum class BarShape { Capsule, Notch };

// What the settings card prints. The Chinese names are the user's own: 胶囊 / 刘海.
inline const wchar_t* BarShapeName(BarShape shape) {
    return shape == BarShape::Notch ? L"刘海" : L"胶囊";
}

// The token bar.txt stores, and the way back. Anything else is REFUSED rather than taken as the default:
// a typo in the file has to be visible in the log, not silently ignored.
inline const char* BarShapeKey(BarShape shape) {
    return shape == BarShape::Notch ? "notch" : "capsule";
}

inline bool BarShapeFromText(const char* text, BarShape* out) {
    if (!text || !out) return false;
    // Hand-rolled rather than _stricmp: this file is pure domain code and the comparison must not depend
    // on a locale the caller happens to have set.
    const auto equals = [](const char* a, const char* b) {
        while (*a && *b) {
            const char x = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a + ('a' - 'A')) : *a;
            const char y = (*b >= 'A' && *b <= 'Z') ? static_cast<char>(*b + ('a' - 'A')) : *b;
            if (x != y) return false;
            ++a;
            ++b;
        }
        return *a == '\0' && *b == '\0';
    };
    if (equals(text, "capsule")) {
        *out = BarShape::Capsule;
        return true;
    }
    if (equals(text, "notch")) {
        *out = BarShape::Notch;
        return true;
    }
    return false;
}

// Where the VISIBLE box starts, in DIPs below the screen's top edge: the capsule keeps the margin the bar
// has always had, the notch is flush.
inline float BarShapeTopMarginDip(BarShape shape) {
    return shape == BarShape::Notch ? 0.0f : kPillTopMarginDip;
}

// One radius for both shapes. A stadium's ends are true semicircles and the notch keeps the same bottom
// corners; what differs is where the shape starts, not how round it is.
inline float BarShapeRadiusDip(float heightDip) {
    return heightDip * 0.5f;
}

// The box the MATERIAL is drawn from, given the visible one. The notch's starts one radius above the
// screen, so the display cuts its top circle away (see the header). The capsule's is the visible box.
inline RoundedBox BarMaterialBoxFor(const RoundedBox& visible, BarShape shape) {
    RoundedBox box = visible;
    if (shape == BarShape::Notch) {
        box.rect.top -= BarShapeRadiusDip(visible.rect.bottom - visible.rect.top);
    }
    return box;
}

} // namespace island
