// =====================================================================
//  Motion.cpp
// =====================================================================
#include "domain/Motion.h"

#include <cmath>

namespace island {
namespace {

float Clamp01(float t) {
    if (t < 0.0f) return 0.0f;
    if (t > 1.0f) return 1.0f;
    return t;
}

} // namespace

const wchar_t* StateName(IslandState state) {
    switch (state) {
        case IslandState::Collapsed:  return L"collapsed";
        case IslandState::Hover:      return L"hover";
        case IslandState::Expanded:   return L"expanded";
        case IslandState::MusicStrip: return L"music-strip";
        case IslandState::FullBar:    return L"full-bar";
        case IslandState::NoticeStrip: return L"notice-strip";
        case IslandState::SearchStrip: return L"search-strip";
    }
    return L"unknown";
}

IslandState StateFromName(const wchar_t* name) {
    if (!name) return IslandState::Collapsed;
    if (_wcsicmp(name, L"collapsed") == 0) return IslandState::Collapsed;
    if (_wcsicmp(name, L"hover") == 0) return IslandState::Hover;
    if (_wcsicmp(name, L"expanded") == 0) return IslandState::Expanded;
    if (_wcsicmp(name, L"music-strip") == 0) return IslandState::MusicStrip;
    if (_wcsicmp(name, L"full-bar") == 0) return IslandState::FullBar;
    if (_wcsicmp(name, L"notice-strip") == 0) return IslandState::NoticeStrip;
    if (_wcsicmp(name, L"search-strip") == 0) return IslandState::SearchStrip;
    return IslandState::Collapsed;
}

// The size a state settles at.
//
// Every state except two is the pill, and saying it that way is the point: the constants that used to
// give Hover and MusicStrip their own widths existed only so the outline could jitter as the pointer
// moved and as music started. "The island does not need to grow sideways any more" removes them, and
// with one size for all of them the transition between them is zero distance -- nothing to animate,
// rather than an animation that happens to be imperceptible.
SizeF SettledSizeFor(IslandState state) {
    switch (state) {
        case IslandState::Expanded:
            return SizeF{kExpandedWidthDip, kExpandedHeightDip, kCardRadiusDip};
        case IslandState::FullBar:
            // The width is filled in by BoxForState, which is the only place that knows the monitor's
            // width. A zero here means "as wide as the monitor"; the radius is small rather than a
            // stadium because a 2560-wide semicircle would be a lens shape, not a bar.
            return SizeF{kFullBarWidthDip, kFullBarHeightDip, kFullBarRadiusDip};
        case IslandState::Collapsed:
        case IslandState::Hover:
        case IslandState::MusicStrip:
            break;
        case IslandState::SearchStrip:
            return SizeF{kSearchStripWidthDip, kCollapsedHeightDip,
                         StadiumRadiusForHeight(kCollapsedHeightDip)};
        case IslandState::NoticeStrip:
            // The one shape between the pill and the card: wider, not taller. The height stays the pill's so
            // the island still reads as the island, and so the strip cannot collide with the deck, which is
            // laid out vertically centred on it.
            return SizeF{kNoticeStripWidthDip, kNoticeStripHeightDip,
                         StadiumRadiusForHeight(kNoticeStripHeightDip)};
    }
    return SizeF{kCollapsedWidthDip, kCollapsedHeightDip,
                 StadiumRadiusForHeight(kCollapsedHeightDip)};
}

SizeF StateSize(IslandState state) { return SettledSizeFor(state); }

// Whether two shapes are the same rectangle, to within the precision the layout works at.
bool SameBox(const RoundedBox& a, const RoundedBox& b) {
    constexpr float kEpsilon = 0.01f;
    return std::fabs(a.rect.left - b.rect.left) < kEpsilon &&
           std::fabs(a.rect.top - b.rect.top) < kEpsilon &&
           std::fabs(a.rect.right - b.rect.right) < kEpsilon &&
           std::fabs(a.rect.bottom - b.rect.bottom) < kEpsilon;
}

float EaseOutCubic(float t) {
    const float u = 1.0f - Clamp01(t);
    return 1.0f - u * u * u;
}

float EaseInOutCubic(float t) {
    const float x = Clamp01(t);
    if (x < 0.5f) {
        return 4.0f * x * x * x;
    }
    const float u = -2.0f * x + 2.0f;
    return 1.0f - (u * u * u) * 0.5f;
}

float TransitionSeconds(IslandState from, IslandState to, const MotionTiming& timing) {
    if (from == to) return 0.0f;

    const bool outward = static_cast<int>(to) > static_cast<int>(from);
    // A transition that touches the card is a height change and is given its own
    // duration; a purely horizontal spread is quicker because less is moving.
    if (from == IslandState::Expanded || to == IslandState::Expanded) {
        return outward ? timing.toExpanded : timing.fromExpanded;
    }
    return outward ? timing.collapseToHover : timing.hoverToCollapse;
}

float (*CurveFor(IslandState from, IslandState to))(float) {
    // One curve, and it is an EASE-OUT. Not a pair-specific choice any more.
    //
    // Every animated transition in this program now involves the card: Collapsed, Hover and MusicStrip
    // are all the same rectangle, so a switch between them is not a transition at all, and the only
    // other shape (FullBar) has always used this curve.
    //
    // The card's transitions used to get a symmetric S-curve instead, on the theory that a height
    // change should not appear to settle early. Two versions were tried -- EaseInOutCubic, reported as
    // stiff, then EaseInOutQuint, reported as still stiff -- and the family itself is the problem, so no
    // member of it fixes this. Measured at the bar's real cadence (164 fps, so 59 frames across a
    // 0.36 s slide of 214 DIP):
    //
    //      progress     EaseInOutQuint          EaseOutCubic
    //      0- 30%       0.0, 0.2, 1.2 DIP/frame 9.8, 7.9, 6.1
    //      40- 60%     12.2, 12.2, 4.5          4.6, 3.3, 2.2
    //      70-100%      1.2, 0.2, 0.0           1.3, 0.7, 0.3
    //
    // The S-curve sits almost still for the first third, crosses a third of the screen in the middle
    // third, and stops dead at the end: a stall, then a lurch, then another stall. That is the shape a
    // "stiff" and "janky" report describes, and it is why changing its duration never helped.
    //
    // An ease-out has one speed profile -- its first frame is its fastest and it decelerates to a stop --
    // so there is nothing to lurch, and the movement starts on the frame the panel is asked for. That is
    // what a flyout should do: appear at once, then settle.
    (void)from;
    (void)to;
    return &EaseOutCubic;
}

IslandAnchor AnchorForWindow(float originX, float originY, float monitorWidthDip,
                             float bandHeightDip, BarShape shape) {
    (void)originY;   // the vertical anchor is intentionally independent of it

    IslandAnchor anchor;
    // Horizontally: the middle of the monitor, which is also the middle of the
    // full-width window, so the window origin has to be subtracted.
    anchor.centerX = monitorWidthDip * 0.5f - originX;

    // Vertically: where the shape starts, and NOTHING else.
    //
    // The window's own top edge is placed at the top of the screen (see
    // IslandWindow::Create), so "one margin below the edge" is already
    // expressed in window-local coordinates. An earlier version also subtracted
    // originY here, which double-counted the offset and put the pill 26 DIP above
    // the top of its own window -- invisible on screen, and caught only because
    // the self-test asserts the pill lies inside the window it was laid out for.
    //
    // 2026-11-06: the margin is the SHAPE's now -- the capsule keeps the one margin the bar has always had,
    // the notch starts flush with the screen's top edge (see domain/BarShape.h).
    anchor.topY = BarShapeTopMarginDip(shape);

    // The band the shell's taskbar claims, when the caller measured one. Clamped rather than trusted:
    // a work area that says the band is nothing (which happens on a machine whose taskbar is on
    // another edge) would make the full bar invisible, and the state exists to cover that band.
    anchor.bandHeightDip = (bandHeightDip >= kMinBandHeightDip &&
                            bandHeightDip <= kMaxBandHeightDip)
                               ? bandHeightDip
                               : kFullBarHeightDip;
    anchor.shape = shape;
    return anchor;
}

RoundedBox BoxForState(IslandState state, const IslandAnchor& anchor) {
    SizeF size = StateSize(state);

    // The full bar is exactly as tall as the band it exists to fill -- see IslandAnchor::bandHeightDip.
    // Taken here rather than in StateSize because StateSize has no anchor, and the anchor is where the
    // measurement lives.
    if (state == IslandState::FullBar) {
        size.height = anchor.bandHeightDip;
    }

    // A width of zero means "the whole monitor", which only this function can resolve: it is the one
    // place holding the anchor, and the anchor carries the monitor's centre.
    //
    // The bar is centred on the monitor's centre -- not on the window -- so filling the width cannot
    // depend on where the window happens to be.
    if (size.width <= 0.0f) {
        size.width = anchor.centerX * 2.0f;
    }

    RoundedBox box;
    if (state == IslandState::SearchStrip) {
        box.rect.left = anchor.centerX - kCollapsedWidthDip * 0.5f;
        box.rect.right = box.rect.left + size.width;
    } else {
        box.rect.left  = anchor.centerX - size.width * 0.5f;
        box.rect.right = anchor.centerX + size.width * 0.5f;
    }
    // Top-anchored: every state starts at the same edge and only the height grows,
    // so the shapes are nested rather than centred on each other.
    box.rect.top    = anchor.topY;
    box.rect.bottom = anchor.topY + size.height;
    box.radiusX     = size.radius;
    box.radiusY     = size.radius;
    return box;
}

RoundedBox LerpBox(const RoundedBox& a, const RoundedBox& b, float t) {
    const float x = Clamp01(t);
    const auto mix = [x](float p, float q) { return p + (q - p) * x; };

    RoundedBox out;
    out.rect.left   = mix(a.rect.left,   b.rect.left);
    out.rect.top    = mix(a.rect.top,    b.rect.top);
    out.rect.right  = mix(a.rect.right,  b.rect.right);
    out.rect.bottom = mix(a.rect.bottom, b.rect.bottom);

    // Blend the radii. An earlier version re-derived them from the interpolated
    // height, which is right only while both ends are stadiums: mixing a stadium
    // with the card produced radii that belonged to neither.
    out.radiusX = mix(a.radiusX, b.radiusX);
    out.radiusY = mix(a.radiusY, b.radiusY);
    return out;
}

namespace {
RoundedBox StagedBox(const RoundedBox& a, const RoundedBox& b, IslandState from,
                     IslandState to, float t) {
    const float x = Clamp01(t);
    float widthT = x;
    float heightT = x;
    if (to == IslandState::Expanded) {
        widthT = Clamp01(x / 0.48f);
        heightT = Clamp01((x - 0.44f) / 0.56f);
    } else if (from == IslandState::Expanded) {
        heightT = Clamp01(x / 0.56f);
        widthT = Clamp01((x - 0.52f) / 0.48f);
    }
    const auto mix = [](float p, float q, float amount) {
        return p + (q - p) * amount;
    };
    RoundedBox out;
    out.rect.left = mix(a.rect.left, b.rect.left, widthT);
    out.rect.right = mix(a.rect.right, b.rect.right, widthT);
    out.rect.top = mix(a.rect.top, b.rect.top, heightT);
    out.rect.bottom = mix(a.rect.bottom, b.rect.bottom, heightT);
    out.radiusX = mix(a.radiusX, b.radiusX, heightT);
    out.radiusY = mix(a.radiusY, b.radiusY, heightT);
    return out;
}
} // namespace

// --- Motion ----------------------------------------------------------

void Motion::ScaleDurations(float factor) {
    if (factor <= 0.0f) return;
    timing_.toExpanded       *= factor;
    timing_.fromExpanded     *= factor;
    timing_.collapseToHover  *= factor;
    timing_.hoverToCollapse  *= factor;
    // A transition already in flight keeps the duration it started with: the caller sets this before
    // the loop begins, and re-deriving it mid-flight would show up as a jump in the shape.
}

void Motion::SetAnchor(const IslandAnchor& anchor) {
    anchor_ = anchor;
    hasAnchor_ = true;

    // Re-derive the shapes for the new layout context without touching the state
    // or the animation progress: a DPI change mid-transition must not restart
    // the motion.
    from_ = BoxForState(fromState_, anchor_);
    to_   = BoxForState(target_, anchor_);

    if (animating_) {
        current_ = stagedExpansion_
                       ? StagedBox(from_, to_, fromState_, target_,
                                   CurveFor(fromState_, target_)(progress_))
                       : LerpBox(from_, to_, CurveFor(fromState_, target_)(progress_));
    } else {
        current_ = to_;
        from_ = current_;
    }
}

bool Motion::SetTarget(IslandState state) {
    if (!hasAnchor_) return false;
    if (state == target_) return false;

    // A state that resolves to the SAME SHAPE is not a transition.
    //
    // Collapsed, Hover and MusicStrip are all the pill now -- the outline no longer responds to the
    // pointer or to music. Without this the bar would run a 160 ms animation between two identical
    // rectangles, which is invisible but not free: it keeps the frame loop drawing, sets the window
    // region again and reports itself as animating. "Nothing to animate" should mean nothing runs.
    const RoundedBox candidate = BoxForState(state, anchor_);
    if (SameBox(current_, candidate)) {
        target_ = state;
        fromState_ = state;
        to_ = candidate;
        animating_ = false;
        elapsed_ = 0.0f;
        progress_ = 1.0f;
        return false;
    }

    // Continue from wherever the shape currently is, so reversing direction
    // mid-flight never jumps.
    from_ = current_;
    fromState_ = target_;
    target_ = state;
    to_ = candidate;

    duration_ = TransitionSeconds(fromState_, target_, timing_);
    elapsed_ = 0.0f;
    progress_ = 0.0f;

    if (duration_ <= 0.0f) {
        current_ = to_;
        progress_ = 1.0f;
        animating_ = false;
        return true;
    }

    animating_ = true;

    // Produce the first frame immediately. Requesting a state and then waiting a
    // tick to see anything would show one stale frame at the old geometry.
    current_ = stagedExpansion_
                   ? StagedBox(from_, to_, fromState_, target_,
                               CurveFor(fromState_, target_)(0.0f))
                   : LerpBox(from_, to_, CurveFor(fromState_, target_)(0.0f));
    return true;
}

bool Motion::Tick(float dtSeconds) {
    if (!animating_) return false;

    elapsed_ += dtSeconds;
    if (duration_ <= 0.0f || elapsed_ >= duration_) {
        current_ = to_;
        progress_ = 1.0f;
        animating_ = false;
        return false;   // this call produced the final frame
    }

    progress_ = elapsed_ / duration_;
    current_ = stagedExpansion_
                   ? StagedBox(from_, to_, fromState_, target_,
                               CurveFor(fromState_, target_)(progress_))
                   : LerpBox(from_, to_, CurveFor(fromState_, target_)(progress_));
    return true;
}

void Motion::SnapTo(IslandState state) {
    target_ = state;
    fromState_ = state;
    animating_ = false;
    elapsed_ = 0.0f;
    progress_ = 1.0f;
    if (hasAnchor_) {
        current_ = BoxForState(state, anchor_);
    }
    from_ = current_;
    to_ = current_;
}

} // namespace island
