// =====================================================================
//  NiriBarLayout.h -- the island plus its two attached groups
//
//  The final intended structure is:
//
//      [ current application ]   [ island ]   [ window ] [ window ] ...
//
//  The island itself carries search, time/date and notifications. There are
//  no separate overview/workspace/system groups at the screen edges.
// =====================================================================
#pragma once

#include "domain/Geometry.h"
#include "domain/BarShape.h"
#include "domain/Motion.h"
#include "domain/StartSearch.h"
#include "domain/TaskStripLayout.h"

#include <algorithm>
#include <cmath>

namespace island {

inline constexpr float kIslandSideGapDip = 8.0f;
inline constexpr float kIslandActiveAppWidthDip = 132.0f;
inline constexpr int kIslandMaxTaskIcons = kMaxTaskIcons;
// The inline search surface is inset equally on all four sides. The same-width
// slots at either end keep the text editor centred around the search mark.
inline constexpr float kNiriSearchInsetDip = 4.0f;
inline constexpr float kNiriSearchSideSlotDip = 24.0f;
inline constexpr float kNiriSearchIconCentreDip = 12.0f;
inline constexpr float kNiriSearchBorderInsetDip = 1.0f;
inline constexpr float kNiriSearchContentTravelDip = 28.0f;
inline constexpr float kNiriSearchResultsGapDip = 6.0f;
inline constexpr float kNiriSearchResultsPaddingDip = 8.0f;
inline constexpr float kNiriSearchResultsRowDip = 36.0f;
inline constexpr float kNiriSearchResultsRowGapDip = 2.0f;
inline constexpr float kNiriSearchResultsEmptyHeightDip = 52.0f;
inline constexpr float kNiriSearchResultsRadiusDip = 18.0f;
inline constexpr float kNiriMusicWaveCentreFromRightDip = 18.0f;
inline constexpr float kNiriMusicWaveHitDip = 30.0f;
inline constexpr float kNiriNotchExtentSeconds = 0.24f;
inline constexpr float kNiriSideGroupSlideDip = 20.0f;

struct NiriSearchLayout {
    RectF field;
    RectF editor;
    bool valid = false;
};

struct NiriSearchMotion {
    float contentShiftDip = 0.0f;
    float contentOpacity = 1.0f;
};

struct NiriSearchResultsLayout {
    RoundedBox panel;
    RectF rows[kStartSearchMaxResults];
    RectF status;
    int count = 0;
    bool pending = false;
    bool valid = false;
};

inline RectF NiriMusicWaveButtonFor(const RoundedBox& island) {
    const float centreX = island.rect.right - kNiriMusicWaveCentreFromRightDip;
    const float centreY = (island.rect.top + island.rect.bottom) * 0.5f;
    const float half = kNiriMusicWaveHitDip * 0.5f;
    return {centreX - half, centreY - half, centreX + half, centreY + half};
}

inline bool NiriNotchExtentStep(float* value, float target, float dtSeconds) {
    if (!value) return false;
    target = std::clamp(target, 0.0f, 1.0f);
    const float previous = *value;
    const float step = std::max(0.0f, dtSeconds) / kNiriNotchExtentSeconds;
    if (*value < target) {
        *value = std::min(target, *value + step);
    } else if (*value > target) {
        *value = std::max(target, *value - step);
    }
    return std::fabs(*value - previous) > 0.0001f ||
           std::fabs(*value - target) > 0.0001f;
}

// Both bar shapes show the side groups at rest. They stand down only while the pointer opens a card or
// inline search, and remain away for the whole interaction so they cannot flash back under the card.
inline float NiriSideGroupTarget(bool sideGroupsHidden, bool contentPresent) {
    return !sideGroupsHidden && contentPresent ? 1.0f : 0.0f;
}

inline float NiriSearchEase(float progress) {
    progress = std::clamp(progress, 0.0f, 1.0f);
    const float smooth = progress * progress * (3.0f - 2.0f * progress);
    return smooth * 0.65f + progress * 0.35f;
}

inline float NiriSideGroupVisualExtent(float extent) {
    return NiriSearchEase(extent);
}

inline float NiriSideGroupVisualOffsetDip(float extent) {
    return kNiriSideGroupSlideDip * (1.0f - NiriSideGroupVisualExtent(extent));
}

inline NiriSearchMotion NiriSearchMotionFor(float progress) {
    const float eased = NiriSearchEase(progress);
    NiriSearchMotion motion;
    motion.contentShiftDip = kNiriSearchContentTravelDip * eased;
    motion.contentOpacity = 1.0f - eased;
    return motion;
}

// One geometry for the renderer, the native editor and the hit-test boundary.
inline NiriSearchLayout NiriSearchLayoutFor(const RoundedBox& island, float progress) {
    NiriSearchLayout layout;
    progress = std::clamp(progress, 0.0f, 1.0f);
    const float eased = NiriSearchEase(progress);
    const float left = island.rect.left + kNiriSearchInsetDip;
    const float top = island.rect.top + kNiriSearchInsetDip;
    const float bottom = island.rect.bottom - kNiriSearchInsetDip;
    const float fullRight = island.rect.right - kNiriSearchInsetDip;
    const float collapsedWidth = bottom - top;
    const float width = collapsedWidth + (fullRight - left - collapsedWidth) * eased;
    if (width <= 0.0f) return layout;

    layout.field = {left, top, left + width, bottom};
    const float sideSlot = std::min(kNiriSearchSideSlotDip, width * 0.5f);
    const float textInset = kNiriSearchBorderInsetDip + sideSlot;
    layout.editor = {layout.field.left + textInset, layout.field.top + kNiriSearchBorderInsetDip,
                     layout.field.right - textInset,
                     layout.field.bottom - kNiriSearchBorderInsetDip};
    layout.valid = true;
    return layout;
}

inline float NiriSearchResultsFullHeight(int count) {
    if (count <= 0) return kNiriSearchResultsEmptyHeightDip;
    count = std::min(count, kStartSearchMaxResults);
    return kNiriSearchResultsPaddingDip * 2.0f +
           static_cast<float>(count) * kNiriSearchResultsRowDip +
           static_cast<float>(count - 1) * kNiriSearchResultsRowGapDip;
}

inline NiriSearchResultsLayout NiriSearchResultsLayoutFor(const RoundedBox& island, int count,
                                                          bool pending, float reveal) {
    NiriSearchResultsLayout layout;
    reveal = std::clamp(reveal, 0.0f, 1.0f);
    if (island.rect.right <= island.rect.left || reveal <= 0.001f) return layout;

    layout.count = std::clamp(count, 0, kStartSearchMaxResults);
    layout.pending = pending;
    const float top = island.rect.bottom + kNiriSearchResultsGapDip;
    const float fullHeight = NiriSearchResultsFullHeight(layout.count);
    const float visibleHeight = fullHeight * NiriSearchEase(reveal);
    layout.panel.rect = {island.rect.left, top, island.rect.right, top + visibleHeight};
    layout.panel.radiusX =
        std::min(kNiriSearchResultsRadiusDip, std::max(0.0f, visibleHeight * 0.5f));
    layout.panel.radiusY = layout.panel.radiusX;

    const float left = island.rect.left + kNiriSearchResultsPaddingDip;
    const float right = island.rect.right - kNiriSearchResultsPaddingDip;
    float rowTop = top + kNiriSearchResultsPaddingDip;
    for (int i = 0; i < layout.count; ++i) {
        layout.rows[i] = {left, rowTop, right, rowTop + kNiriSearchResultsRowDip};
        rowTop += kNiriSearchResultsRowDip + kNiriSearchResultsRowGapDip;
    }
    layout.status = {left, top + kNiriSearchResultsPaddingDip, right,
                     top + kNiriSearchResultsPaddingDip + kNiriSearchResultsRowDip};
    layout.valid = visibleHeight > 0.001f;
    return layout;
}

inline int NiriSearchResultAt(const NiriSearchResultsLayout& layout, float x, float y) {
    if (!layout.valid || y > layout.panel.rect.bottom) return -1;
    for (int i = 0; i < layout.count; ++i) {
        const RectF& row = layout.rows[i];
        if (x >= row.left && x <= row.right && y >= row.top && y <= row.bottom) return i;
    }
    return -1;
}

inline RoundedBox NiriCardMorphFor(const RoundedBox& animated, const RoundedBox& restingPill,
                                   float finalWidth, float finalHeight, bool collapsing = false) {
    if (animated.rect.right <= animated.rect.left || finalWidth <= 0.0f ||
        finalHeight <= 0.0f) {
        return restingPill;
    }
    const float pillWidth = restingPill.rect.right - restingPill.rect.left;
    const float pillHeight = restingPill.rect.bottom - restingPill.rect.top;
    const float widthT = std::clamp(
        ((animated.rect.right - animated.rect.left) - pillWidth) /
            std::max(1.0f, kExpandedWidthDip - pillWidth),
        0.0f, 1.0f);
    const float heightT = std::clamp(
        ((animated.rect.bottom - animated.rect.top) - pillHeight) /
            std::max(1.0f, kExpandedHeightDip - pillHeight),
        0.0f, 1.0f);
    // On the way closed the card belongs to the island again immediately. The
    // remaining movement is vertical, so no shallow card-wide band can survive
    // under the task icons and read as a leftover strip.
    const float shapedWidthT = collapsing ? 0.0f : widthT;
    const float width = pillWidth + (finalWidth - pillWidth) * shapedWidthT;
    const float height = pillHeight + (finalHeight - pillHeight) * heightT;
    const float centreX = (restingPill.rect.left + restingPill.rect.right) * 0.5f;

    RoundedBox out;
    out.rect.left = centreX - width * 0.5f;
    out.rect.right = centreX + width * 0.5f;
    out.rect.top = restingPill.rect.top;
    out.rect.bottom = out.rect.top + height;
    out.radiusX = kCornerRadiusDip + (kCardRadiusDip - kCornerRadiusDip) * heightT;
    out.radiusY = out.radiusX;
    return out;
}

// The side groups follow the card's animated WIDTH but stay on the bar's vertical band.
inline RoundedBox NiriDeckBoxFor(const RoundedBox& animated, const RoundedBox& restingPill,
                                 float finalWidth, float finalHeight, bool collapsing = false) {
    RoundedBox out =
        NiriCardMorphFor(animated, restingPill, finalWidth, finalHeight, collapsing);
    out.rect.top = restingPill.rect.top;
    out.rect.bottom = restingPill.rect.bottom;
    return out;
}

// The card's lower column may KEEP an already open dashboard open, but it may not start one.
// Otherwise the pointer entering the old card area during retraction immediately reverses the close.
inline bool NiriDashboardWanted(bool pointerInside, bool pointerInIslandCentre,
                                bool pointerInCardColumn, bool dashboardAlreadyOpen) {
    return pointerInside &&
           (pointerInIslandCentre || (pointerInCardColumn && dashboardAlreadyOpen));
}

inline IslandState NiriNoticeStateFor(IslandState legacyState) {
    return legacyState == IslandState::NoticeStrip ? IslandState::Collapsed : legacyState;
}

struct NiriBarLayout {
    RoundedBox activeApp;
    RoundedBox center;
    RectF taskRow;
    RectF taskButtons[kIslandMaxTaskIcons];
    int taskCapacity = 0;
    int taskCount = 0;
};

inline NiriBarLayout NiriBarLayoutFor(float monitorWidthDip, const RoundedBox& centerPill,
                                      int taskCount) {
    NiriBarLayout layout;
    layout.center = centerPill;
    const float centreY = (centerPill.rect.top + centerPill.rect.bottom) * 0.5f;

    layout.activeApp.rect.right = centerPill.rect.left - kIslandSideGapDip;
    layout.activeApp.rect.left = layout.activeApp.rect.right - kIslandActiveAppWidthDip;
    layout.activeApp.rect.top = centreY - kTaskIconButtonDip * 0.5f;
    layout.activeApp.rect.bottom = centreY + kTaskIconButtonDip * 0.5f;
    layout.activeApp.radiusX = kTaskIconButtonDip * 0.5f;
    layout.activeApp.radiusY = kTaskIconButtonDip * 0.5f;

    const float taskLeftLimit = centerPill.rect.right + kIslandSideGapDip;
    const float taskRightLimit = monitorWidthDip - kRowRightMarginDip;
    const float available = taskRightLimit - taskLeftLimit;
    const float slot = kTaskIconButtonDip + kTaskIconGapDip;
    const int capacity = available > kTaskIconButtonDip
                             ? std::max(1, static_cast<int>((available + kTaskIconGapDip) / slot))
                             : 0;
    layout.taskCapacity = std::min(capacity, kIslandMaxTaskIcons);
    layout.taskCount = std::clamp(taskCount, 0, layout.taskCapacity);
    const float rowWidth =
        static_cast<float>(layout.taskCount) * kTaskIconButtonDip +
        static_cast<float>(std::max(0, layout.taskCount - 1)) * kTaskIconGapDip;
    layout.taskRow = {taskLeftLimit, centreY - kTaskIconButtonDip * 0.5f,
                      taskLeftLimit + rowWidth, centreY + kTaskIconButtonDip * 0.5f};
    for (int i = 0; i < layout.taskCount; ++i) {
        layout.taskButtons[i] = {
            layout.taskRow.left + static_cast<float>(i) * slot,
            layout.taskRow.top,
            layout.taskRow.left + static_cast<float>(i) * slot + kTaskIconButtonDip,
            layout.taskRow.bottom};
    }
    return layout;
}

inline int NiriTaskAt(const NiriBarLayout& layout, float x, float y) {
    for (int i = 0; i < layout.taskCount; ++i) {
        const RectF& box = layout.taskButtons[i];
        if (x >= box.left && x <= box.right && y >= box.top && y <= box.bottom) return i;
    }
    return -1;
}

inline int NiriTaskAtVisual(const NiriBarLayout& layout, float x, float y,
                            float rightExtent) {
    const float shift = NiriSideGroupVisualOffsetDip(rightExtent);
    for (int i = 0; i < layout.taskCount; ++i) {
        const RectF& box = layout.taskButtons[i];
        if (x >= box.left + shift && x <= box.right + shift &&
            y >= box.top && y <= box.bottom) {
            return i;
        }
    }
    return -1;
}

} // namespace island
