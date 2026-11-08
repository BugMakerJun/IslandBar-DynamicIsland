// =====================================================================
//  SettingsLayout.h -- pure geometry for IslandBar's separate settings window.
//
//  The window and renderer both consume this one result. A control therefore cannot be drawn in
//  one place and hit-tested in another, and every rectangle can be asserted without a desktop.
// =====================================================================
#pragma once

#include "domain/Geometry.h"

namespace island {

enum class SettingsPage {
    General = 0,
    Appearance,
    Performance,
    About,
    Count,
};

inline int SettingsPageIndex(SettingsPage page) {
    return static_cast<int>(page);
}

inline SettingsPage SettingsPageFromIndex(int index) {
    if (index < 0 || index >= static_cast<int>(SettingsPage::Count)) return SettingsPage::General;
    return static_cast<SettingsPage>(index);
}

enum class SettingsHitKind {
    None = 0,
    Close,
    Navigation,
    ShapeCapsule,
    ShapeNotch,
    Theme,
    ToggleAutoStart,
    Quit,
    OpenConfigFolder,
    OpenLogFolder,
};

struct SettingsHit {
    SettingsHitKind kind = SettingsHitKind::None;
    int index = -1;
};

inline bool operator==(const SettingsHit& a, const SettingsHit& b) {
    return a.kind == b.kind && a.index == b.index;
}

inline bool operator!=(const SettingsHit& a, const SettingsHit& b) {
    return !(a == b);
}

inline constexpr int kSettingsMaxThemes = 8;

struct SettingsLayout {
    RectF titleBar;
    RectF closeButton;
    RectF sidebar;
    RectF navigation[static_cast<int>(SettingsPage::Count)];
    RectF title;
    RectF subtitle;
    RectF statusRows[4];
    RectF startupLabel;
    RectF autoStartRow;
    RectF autoStartValue;
    RectF autoStartSwitch;
    RectF quitRow;
    RectF shapeLabel;
    RectF shapeSubtitle;
    RectF shapeCapsule;
    RectF shapeNotch;
    RectF themeLabel;
    RectF themeSubtitle;
    RectF themeButtons[kSettingsMaxThemes];
    RectF pathRows[2];
    int themeCount = 0;
};

inline SettingsLayout SettingsLayoutFor(float widthDip, float heightDip, int themeCount) {
    SettingsLayout layout;
    layout.themeCount = themeCount < 0 ? 0 : (themeCount > kSettingsMaxThemes ? kSettingsMaxThemes : themeCount);

    layout.titleBar = {0.0f, 0.0f, widthDip, 44.0f};
    layout.closeButton = {widthDip - 48.0f, 10.0f, widthDip - 16.0f, 34.0f};
    layout.sidebar = {0.0f, 44.0f, 180.0f, heightDip};

    constexpr float navTop = 92.0f;
    constexpr float navHeight = 38.0f;
    constexpr float navGap = 8.0f;
    for (int i = 0; i < static_cast<int>(SettingsPage::Count); ++i) {
        const float top = navTop + static_cast<float>(i) * (navHeight + navGap);
        layout.navigation[i] = {12.0f, top, 168.0f, top + navHeight};
    }

    const float left = 208.0f;
    const float right = widthDip - 28.0f;
    layout.title = {left, 66.0f, right, 98.0f};
    layout.subtitle = {left, 100.0f, right, 122.0f};

    float y = 142.0f;
    constexpr float rowHeight = 58.0f;
    constexpr float rowGap = 10.0f;
    for (int i = 0; i < 4; ++i) {
        layout.statusRows[i] = {left, y, right, y + rowHeight};
        y += rowHeight + rowGap;
    }
    layout.startupLabel = {left, 342.0f, right, 360.0f};
    layout.autoStartRow = {left, 368.0f, right, 416.0f};
    layout.autoStartValue = {right - 182.0f, layout.autoStartRow.top,
                             right - 70.0f, layout.autoStartRow.bottom};
    layout.autoStartSwitch = {right - 54.0f, layout.autoStartRow.top + 12.0f,
                              right - 16.0f, layout.autoStartRow.bottom - 12.0f};
    layout.quitRow = {left, 428.0f, right, 480.0f};

    layout.shapeLabel = {left, 142.0f, right, 164.0f};
    layout.shapeSubtitle = {left, 166.0f, right, 186.0f};
    layout.shapeCapsule = {left, 196.0f, left + 138.0f, 238.0f};
    layout.shapeNotch = {left + 150.0f, 196.0f, left + 288.0f, 238.0f};

    layout.themeLabel = {left, 266.0f, right, 288.0f};
    layout.themeSubtitle = {left, 290.0f, right, 310.0f};
    const float available = right - left;
    constexpr float themeGap = 12.0f;
    const float themeWidth = (available - themeGap) * 0.5f;
    for (int i = 0; i < layout.themeCount; ++i) {
        const int column = i % 2;
        const int row = i / 2;
        const float buttonLeft = left + static_cast<float>(column) * (themeWidth + themeGap);
        const float top = 322.0f + static_cast<float>(row) * 54.0f;
        layout.themeButtons[i] = {buttonLeft, top, buttonLeft + themeWidth, top + 44.0f};
    }

    layout.pathRows[0] = {left, 142.0f, right, 200.0f};
    layout.pathRows[1] = {left, 210.0f, right, 268.0f};
    return layout;
}

inline SettingsHit SettingsHitAt(const SettingsLayout& layout, SettingsPage page, float x, float y) {
    const auto inside = [x, y](const RectF& r) {
        return r.right > r.left && r.bottom > r.top && x >= r.left && x <= r.right &&
               y >= r.top && y <= r.bottom;
    };

    if (inside(layout.closeButton)) return {SettingsHitKind::Close, -1};
    for (int i = 0; i < static_cast<int>(SettingsPage::Count); ++i) {
        if (inside(layout.navigation[i])) return {SettingsHitKind::Navigation, i};
    }

    if (page == SettingsPage::General) {
        if (inside(layout.autoStartRow)) return {SettingsHitKind::ToggleAutoStart, -1};
        if (inside(layout.quitRow)) return {SettingsHitKind::Quit, -1};
    } else if (page == SettingsPage::Appearance) {
        if (inside(layout.shapeCapsule)) return {SettingsHitKind::ShapeCapsule, -1};
        if (inside(layout.shapeNotch)) return {SettingsHitKind::ShapeNotch, -1};
        for (int i = 0; i < layout.themeCount; ++i) {
            if (inside(layout.themeButtons[i])) return {SettingsHitKind::Theme, i};
        }
    } else if (page == SettingsPage::About) {
        if (inside(layout.pathRows[0])) return {SettingsHitKind::OpenConfigFolder, -1};
        if (inside(layout.pathRows[1])) return {SettingsHitKind::OpenLogFolder, -1};
    }
    return {};
}

// The keyboard traversal order: navigation, close, then the controls on the current page.
inline int SettingsFocusTargets(const SettingsLayout& layout, SettingsPage page, SettingsHit* out,
                                int capacity) {
    if (!out || capacity <= 0) return 0;
    int count = 0;
    const auto add = [&](SettingsHitKind kind, int index) {
        if (count < capacity) out[count++] = {kind, index};
    };
    for (int i = 0; i < static_cast<int>(SettingsPage::Count); ++i) add(SettingsHitKind::Navigation, i);
    add(SettingsHitKind::Close, -1);
    if (page == SettingsPage::General) {
        add(SettingsHitKind::ToggleAutoStart, -1);
        add(SettingsHitKind::Quit, -1);
    } else if (page == SettingsPage::Appearance) {
        add(SettingsHitKind::ShapeCapsule, -1);
        add(SettingsHitKind::ShapeNotch, -1);
        for (int i = 0; i < layout.themeCount; ++i) add(SettingsHitKind::Theme, i);
    } else if (page == SettingsPage::About) {
        add(SettingsHitKind::OpenConfigFolder, -1);
        add(SettingsHitKind::OpenLogFolder, -1);
    }
    return count;
}

} // namespace island
