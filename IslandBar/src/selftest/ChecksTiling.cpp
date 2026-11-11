#include "domain/ScrollTiling.h"

#include <cwchar>
#include <vector>

#include "app/IslandWindow.h"
#include "app/ScrollTilingViewport.h"
#include "platform/Monitor.h"
#include "platform/ScrollTilingGrabLayer.h"
#include "selftest/Checks.h"
#include "selftest/Harness.h"

namespace island {

void TestScrollTiling() {
    Section(L"scroll tiling model");

    std::vector<ScrollTilingWindowId> many;
    for (std::uint64_t i = 1; i <= 66; ++i) many.push_back({i});
    ScrollTilingModel cap;
    Check(cap.Configure(many), L"a large strip configures successfully");
    Check(cap.Count() == kScrollTilingMaxColumns,
          L"the practical column bound has no five-window limit");

    ScrollTilingModel model;
    const std::vector<ScrollTilingWindowId> initial = {{10}, {20}, {30}};
    Check(model.Configure(initial), L"the initial workspace accepts its columns");
    Check(model.Count() == 3 && model.ActiveIndex() == 0,
          L"the first configured window is the focused column");

    Check(model.AddAfterActive({40}, {}),
          L"a new window appends after the focused column");
    Check(model.At(0) == ScrollTilingWindowId{10} &&
              model.At(1) == ScrollTilingWindowId{40} &&
              model.At(2) == ScrollTilingWindowId{20} &&
              model.ActiveIndex() == 1,
          L"the new column is inserted to the right and becomes focused");

    const ScrollTilingRectPx work = {0, 40, 2560, 1440};
    const ScrollTilingLayout before = model.Layout(work, 0, 8, 8);
    const int oldTenWidth = before.targets[0].rect.Width();
    const int oldTwentyWidth = before.targets[2].rect.Width();
    Check(model.AddAfterActive({50}, {}),
          L"another window can append to the strip");
    const ScrollTilingLayout after = model.Layout(work, 0, 8, 8);
    Check(after.targets[0].rect.Width() == oldTenWidth &&
              after.targets[3].rect.Width() == oldTwentyWidth,
          L"adding a column does not resize any existing column");

    Check(model.SetWidth(0, {ScrollTilingWidthMode::Proportion, 0.5}),
          L"one column accepts an independent width");
    Check(model.ColumnAt(0).width.value == 0.5 &&
              model.ColumnAt(1).width.value == 2.0 / 3.0,
          L"changing one column width leaves the others untouched");

    Check(model.Focus({30}), L"a managed window can become focused");
    const ScrollTilingLayout focused = model.Layout(work, 0, 8, 8);
    Check(focused.valid && focused.activeIndex == model.ActiveIndex() &&
              focused.viewportTarget > 0,
          L"focus produces a nonzero viewport target");

    Check(model.ScrollBy(-100), L"scrolling clamps at the first column");
    Check(model.ActiveIndex() == 0, L"scroll-left stops at the first column");
    Check(model.ScrollBy(100), L"scrolling clamps at the last column");
    Check(model.ActiveIndex() == model.Count() - 1,
          L"scroll-right stops at the last column");

    const int stripMaximum = model.MaxViewportOffset(work.Width(), 8, 8);
    Check(stripMaximum > 0 &&
              model.ClampViewportOffset(stripMaximum + 900, work.Width(), 8, 8) ==
                  stripMaximum,
          L"dragging cannot move the viewport past the strip's right edge");
    Check(model.ClampViewportOffset(-900, work.Width(), 8, 8) == 0,
          L"dragging cannot move the viewport before the first column");
    Check(model.NearestColumnToOffset(0, work.Width(), 8, 8) == 0,
          L"the first column owns the left edge of the strip");
    Check(model.NearestColumnToOffset(stripMaximum, work.Width(), 8, 8) ==
              model.Count() - 1,
          L"the last column owns the right edge of the strip");
    Check(model.NearestColumnToOffset(900, work.Width(), 8, 8) == 1,
          L"a 900 px drag selects the next column");

    ScrollTilingLayout reusedLayout;
    reusedLayout.targets.reserve(kScrollTilingMaxColumns);
    model.LayoutInto(work, 0, 8, 8, &reusedLayout);
    const ScrollTilingTarget* firstTarget = reusedLayout.targets.data();
    model.LayoutInto(work, 900, 8, 8, &reusedLayout);
    Check(reusedLayout.valid && reusedLayout.targets.data() == firstTarget &&
              reusedLayout.viewportOffset == 900,
          L"the hot layout path reuses its target buffer");

    const int activeBeforeMove = model.ActiveIndex();
    Check(model.MoveColumn(activeBeforeMove, 1),
          L"a focused column can be reordered");
    Check(model.ActiveIndex() == 1,
          L"reordering follows the same focused window");

    const ScrollTilingWindowId active = model.At(model.ActiveIndex());
    Check(model.Remove(active), L"the focused column can be removed");
    Check(model.Count() == 4 && !model.Contains(active),
          L"removing a column preserves the remaining strip");

    ScrollTilingModel twenty;
    std::vector<ScrollTilingWindowId> twentyIds;
    for (std::uint64_t i = 1; i <= 20; ++i) twentyIds.push_back({100 + i});
    Check(twenty.Configure(twentyIds), L"a twenty-column strip configures");
    const ScrollTilingLayout twentyLayout = twenty.Layout(work, 0, 8, 8);
    Check(twentyLayout.valid && twentyLayout.targets.size() == 20,
          L"all twenty columns are represented in logical space");

    Check(!model.Layout(ScrollTilingRectPx{0, 0, 0, 0}, 0, 8, 8).valid,
          L"an empty work area has no layout");
    model.Clear();
    Check(model.Count() == 0, L"clearing the workspace removes every column");

    ScrollTilingViewport viewport;
    viewport.Reset(0);
    viewport.SetTarget(1000, 100, 160);
    int offset = 0;
    Check(viewport.Advance(180, &offset) && offset > 0 && offset < 1000,
          L"viewpoint advances through an intermediate position");
    viewport.SetTarget(2000, 180, 160);
    Check(viewport.Advance(260, &offset) && offset > 0 && offset < 2000,
          L"retargeting continues from the live position");
    Check(!viewport.Advance(340, &offset) && offset == 2000,
          L"the viewport converges on only the latest target");

    const MonitorGeometry monitor = QueryMonitor(nullptr);
    ScrollTilingGrabLayer grabLayer;
    POINT pointer = {};
    Check(GetCursorPos(&pointer) != FALSE,
          L"the grab layer can read the current pointer");
    SetLastError(ERROR_SUCCESS);
    const bool grabLayerShown = grabLayer.Show(monitor.screen);
    wchar_t grabLayerInfo[160] = {};
    std::swprintf(grabLayerInfo, 160,
                  L"hwnd=%p visible=%d error=%lu screen=(%ld,%ld,%ld,%ld)",
                  reinterpret_cast<void*>(grabLayer.Handle()),
                  grabLayer.Visible() ? 1 : 0,
                  static_cast<unsigned long>(GetLastError()),
                  monitor.screen.left, monitor.screen.top,
                  monitor.screen.right, monitor.screen.bottom);
    Info(L"grab layer", grabLayerInfo);
    Check(grabLayerShown,
          L"the temporary grab layer covers the current monitor");
    if (grabLayer.Visible()) {
        Check(grabLayer.CoversPointer(pointer),
              L"the grab layer is above every window at the pointer");
        grabLayer.Hide();
    }
    Check(!grabLayer.Visible(), L"the grab layer hides after the check");
}

} // namespace island
