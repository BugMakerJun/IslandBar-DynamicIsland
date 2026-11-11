// =====================================================================
//  ScrollTiling.h -- pure model of IslandBar's Niri-style horizontal strip.
//
//  A Workspace owns an ordered vector of Columns. A Window is a member of
//  a Column; this first version has one window per column, but the model
//  never confuses a window's screen rectangle with its logical position.
//  The viewport is an offset over the strip, not a pair of fixed slots.
// =====================================================================
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace island {

inline constexpr int kScrollTilingMaxColumns = 64;

struct ScrollTilingWindowId {
    std::uint64_t value = 0;

    friend bool operator==(const ScrollTilingWindowId& a, const ScrollTilingWindowId& b) {
        return a.value == b.value;
    }
    friend bool operator!=(const ScrollTilingWindowId& a, const ScrollTilingWindowId& b) {
        return !(a == b);
    }
    explicit operator bool() const { return value != 0; }
};

struct ScrollTilingRectPx {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    int Width() const { return right - left; }
    int Height() const { return bottom - top; }
    bool Empty() const { return right <= left || bottom <= top; }

    friend bool operator==(const ScrollTilingRectPx&, const ScrollTilingRectPx&) = default;
};

enum class ScrollTilingWidthMode {
    Proportion,
    Fixed,
};

struct ScrollTilingColumnWidth {
    ScrollTilingWidthMode mode = ScrollTilingWidthMode::Proportion;
    double value = 2.0 / 3.0;
};

struct ScrollTilingColumn {
    ScrollTilingWindowId window;
    ScrollTilingColumnWidth width;
};

struct ScrollTilingTarget {
    ScrollTilingWindowId id;
    ScrollTilingRectPx rect;
};

struct ScrollTilingLayout {
    std::vector<ScrollTilingTarget> targets;
    int activeIndex = 0;
    int viewportOffset = 0;
    int viewportTarget = 0;
    bool valid = false;
};

class ScrollTilingModel {
public:
    bool Configure(std::span<const ScrollTilingWindowId> windows,
                   ScrollTilingColumnWidth defaultWidth =
                       ScrollTilingColumnWidth{});
    bool AddAfterActive(ScrollTilingWindowId window,
                        ScrollTilingColumnWidth width =
                            ScrollTilingColumnWidth{});
    bool Remove(ScrollTilingWindowId window);
    bool Contains(ScrollTilingWindowId window) const;
    bool Focus(ScrollTilingWindowId window);
    bool FocusIndex(int index);
    bool ScrollBy(int delta);
    bool MoveColumn(int from, int to);
    bool SetWidth(int index, ScrollTilingColumnWidth width);

    int Count() const { return static_cast<int>(columns_.size()); }
    int ActiveIndex() const { return activeIndex_; }
    ScrollTilingWindowId At(int index) const;
    ScrollTilingColumn ColumnAt(int index) const;

    int ColumnWidthPx(int index, int viewportWidthPx, int paddingPx) const;
    int LogicalX(int index, int viewportWidthPx, int gapPx, int paddingPx) const;
    int MaxViewportOffset(int viewportWidthPx, int gapPx, int paddingPx) const;
    int ClampViewportOffset(int offset, int viewportWidthPx, int gapPx,
                            int paddingPx) const;
    int TargetViewportOffset(int viewportWidthPx, int gapPx, int paddingPx) const;
    int NearestColumnToOffset(int offset, int viewportWidthPx, int gapPx,
                              int paddingPx) const;
    void LayoutInto(ScrollTilingRectPx workArea, int viewportOffset,
                    int gapPx, int paddingPx,
                    ScrollTilingLayout* output) const;
    ScrollTilingLayout Layout(ScrollTilingRectPx workArea, int viewportOffset,
                              int gapPx = 8, int paddingPx = 8) const;

    void Clear();

private:
    int Position(ScrollTilingWindowId window) const;

    std::vector<ScrollTilingColumn> columns_;
    int activeIndex_ = 0;
};

} // namespace island
