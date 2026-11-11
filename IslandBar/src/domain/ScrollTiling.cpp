#include "domain/ScrollTiling.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace island {

bool ScrollTilingModel::Configure(std::span<const ScrollTilingWindowId> windows,
                                  ScrollTilingColumnWidth defaultWidth) {
    Clear();
    for (ScrollTilingWindowId id : windows) {
        if (!id || Position(id) >= 0 ||
            columns_.size() >= static_cast<size_t>(kScrollTilingMaxColumns)) {
            continue;
        }
        columns_.push_back({id, defaultWidth});
    }
    activeIndex_ = 0;
    return !columns_.empty();
}

bool ScrollTilingModel::AddAfterActive(ScrollTilingWindowId window,
                                       ScrollTilingColumnWidth width) {
    if (!window || Position(window) >= 0 ||
        columns_.size() >= static_cast<size_t>(kScrollTilingMaxColumns)) {
        return false;
    }
    const int index = columns_.empty() ? 0 : activeIndex_ + 1;
    columns_.insert(columns_.begin() + index, {window, width});
    activeIndex_ = index;
    return true;
}

bool ScrollTilingModel::Remove(ScrollTilingWindowId window) {
    const int index = Position(window);
    if (index < 0) return false;
    columns_.erase(columns_.begin() + index);
    if (columns_.empty()) {
        activeIndex_ = 0;
    } else if (index < activeIndex_) {
        --activeIndex_;
    } else if (index == activeIndex_) {
        activeIndex_ = std::clamp(index == 0 ? 0 : index - 1, 0, Count() - 1);
    }
    return true;
}

bool ScrollTilingModel::Contains(ScrollTilingWindowId window) const {
    return Position(window) >= 0;
}

bool ScrollTilingModel::Focus(ScrollTilingWindowId window) {
    const int index = Position(window);
    if (index < 0) return false;
    activeIndex_ = index;
    return true;
}

bool ScrollTilingModel::FocusIndex(int index) {
    if (index < 0 || index >= Count()) return false;
    activeIndex_ = index;
    return true;
}

bool ScrollTilingModel::ScrollBy(int delta) {
    if (Count() <= 0 || delta == 0) return false;
    const int next = std::clamp(activeIndex_ + delta, 0, Count() - 1);
    if (next == activeIndex_) return false;
    activeIndex_ = next;
    return true;
}

bool ScrollTilingModel::MoveColumn(int from, int to) {
    if (from < 0 || from >= Count() || to < 0 || to >= Count() || from == to) {
        return false;
    }
    ScrollTilingWindowId active = At(activeIndex_);
    ScrollTilingColumn moved = columns_[static_cast<size_t>(from)];
    columns_.erase(columns_.begin() + from);
    columns_.insert(columns_.begin() + to, moved);
    activeIndex_ = Position(active);
    return activeIndex_ >= 0;
}

bool ScrollTilingModel::SetWidth(int index, ScrollTilingColumnWidth width) {
    if (index < 0 || index >= Count()) return false;
    if (width.mode == ScrollTilingWidthMode::Proportion) {
        width.value = std::clamp(width.value, 0.05, 1.0);
    } else {
        width.value = std::max(1.0, width.value);
    }
    columns_[static_cast<size_t>(index)].width = width;
    return true;
}

ScrollTilingWindowId ScrollTilingModel::At(int index) const {
    if (index < 0 || index >= Count()) return {};
    return columns_[static_cast<size_t>(index)].window;
}

ScrollTilingColumn ScrollTilingModel::ColumnAt(int index) const {
    if (index < 0 || index >= Count()) return {};
    return columns_[static_cast<size_t>(index)];
}

int ScrollTilingModel::ColumnWidthPx(int index, int viewportWidthPx,
                                     int paddingPx) const {
    if (index < 0 || index >= Count()) return 0;
    const ScrollTilingColumnWidth& policy =
        columns_[static_cast<size_t>(index)].width;
    const int available = std::max(1, viewportWidthPx - paddingPx * 2);
    if (policy.mode == ScrollTilingWidthMode::Proportion) {
        return std::clamp(
            static_cast<int>(std::lround(available * policy.value)), 1, available);
    }
    return std::clamp(static_cast<int>(std::lround(policy.value)), 1, available);
}

int ScrollTilingModel::LogicalX(int index, int viewportWidthPx, int gapPx,
                                int paddingPx) const {
    int x = std::max(0, paddingPx);
    for (int i = 0; i < index && i < Count(); ++i) {
        x += ColumnWidthPx(i, viewportWidthPx, paddingPx) + std::max(0, gapPx);
    }
    return x;
}

int ScrollTilingModel::MaxViewportOffset(int viewportWidthPx, int gapPx,
                                         int paddingPx) const {
    if (Count() <= 0 || viewportWidthPx <= 0) return 0;
    const int padding = std::max(0, paddingPx);
    const int gap = std::max(0, gapPx);
    int contentWidth = padding * 2;
    for (int i = 0; i < Count(); ++i) {
        contentWidth += ColumnWidthPx(i, viewportWidthPx, padding);
        if (i + 1 < Count()) contentWidth += gap;
    }
    return std::max(0, contentWidth - viewportWidthPx);
}

int ScrollTilingModel::ClampViewportOffset(int offset, int viewportWidthPx,
                                           int gapPx, int paddingPx) const {
    return std::clamp(std::max(0, offset), 0,
                      MaxViewportOffset(viewportWidthPx, gapPx, paddingPx));
}

int ScrollTilingModel::TargetViewportOffset(int viewportWidthPx, int gapPx,
                                            int paddingPx) const {
    if (Count() <= 0) return 0;
    const int width = ColumnWidthPx(activeIndex_, viewportWidthPx, paddingPx);
    const int logicalX = LogicalX(activeIndex_, viewportWidthPx, gapPx, paddingPx);
    const int centred = logicalX + width / 2 - viewportWidthPx / 2;
    return ClampViewportOffset(centred, viewportWidthPx, gapPx, paddingPx);
}

int ScrollTilingModel::NearestColumnToOffset(int offset, int viewportWidthPx,
                                             int gapPx, int paddingPx) const {
    if (Count() <= 0) return -1;
    const int clamped =
        ClampViewportOffset(offset, viewportWidthPx, gapPx, paddingPx);
    int nearest = 0;
    int nearestDistance = std::numeric_limits<int>::max();
    for (int i = 0; i < Count(); ++i) {
        const int width = ColumnWidthPx(i, viewportWidthPx, paddingPx);
        const int logicalX = LogicalX(i, viewportWidthPx, gapPx, paddingPx);
        const int centred = logicalX + width / 2 - viewportWidthPx / 2;
        const int target =
            ClampViewportOffset(centred, viewportWidthPx, gapPx, paddingPx);
        const int distance = std::abs(target - clamped);
        if (distance < nearestDistance) {
            nearest = i;
            nearestDistance = distance;
        }
    }
    return nearest;
}

void ScrollTilingModel::LayoutInto(ScrollTilingRectPx workArea,
                                   int viewportOffset, int gapPx,
                                   int paddingPx,
                                   ScrollTilingLayout* output) const {
    if (!output) return;
    ScrollTilingLayout& layout = *output;
    layout.targets.clear();
    layout.activeIndex = 0;
    layout.viewportOffset = 0;
    layout.viewportTarget = 0;
    layout.valid = false;
    if (Count() <= 0 || workArea.Empty()) return;

    gapPx = std::max(0, gapPx);
    paddingPx = std::max(0, paddingPx);
    layout.activeIndex = activeIndex_;
    layout.viewportOffset =
        ClampViewportOffset(viewportOffset, workArea.Width(), gapPx, paddingPx);
    layout.viewportTarget =
        TargetViewportOffset(workArea.Width(), gapPx, paddingPx);
    layout.targets.reserve(columns_.size());

    const int top = workArea.top + paddingPx;
    const int height = std::max(1, workArea.Height() - paddingPx * 2);
    for (int i = 0; i < Count(); ++i) {
        const int width = ColumnWidthPx(i, workArea.Width(), paddingPx);
        const int x = LogicalX(i, workArea.Width(), gapPx, paddingPx) -
                      layout.viewportOffset;
        layout.targets.push_back({
            columns_[static_cast<size_t>(i)].window,
            {workArea.left + x, top, workArea.left + x + width,
             top + height},
        });
    }
    layout.valid = true;
}

ScrollTilingLayout ScrollTilingModel::Layout(ScrollTilingRectPx workArea,
                                              int viewportOffset, int gapPx,
                                              int paddingPx) const {
    ScrollTilingLayout layout;
    LayoutInto(workArea, viewportOffset, gapPx, paddingPx, &layout);
    return layout;
}

void ScrollTilingModel::Clear() {
    columns_.clear();
    activeIndex_ = 0;
}

int ScrollTilingModel::Position(ScrollTilingWindowId window) const {
    if (!window) return -1;
    for (int i = 0; i < Count(); ++i) {
        if (columns_[static_cast<size_t>(i)].window == window) return i;
    }
    return -1;
}

} // namespace island
