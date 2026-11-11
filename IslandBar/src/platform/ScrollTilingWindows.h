// =====================================================================
//  ScrollTilingWindows.h -- the only code allowed to place other processes'
//  top-level windows for scroll tiling.
//
//  Domain owns order and target rectangles. This layer answers whether a
//  window can be managed at all, captures its WINDOWPLACEMENT, applies a
//  bounded batch with DeferWindowPos, verifies what the applications actually
//  accepted, and restores a window only when it has not been moved by the
//  user or another program since our last successful placement.
// =====================================================================
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "domain/ScrollTiling.h"
#include "platform/WindowList.h"

namespace island {

struct ScrollTilingWindowRecord {
    ScrollTilingWindowId id;
    HWND hwnd = nullptr;
    DWORD processId = 0;
    ULONGLONG processCreationTime = 0;
    WINDOWPLACEMENT originalPlacement = {};
    RECT originalRect = {};
    RECT previousAppliedRect = {};
    RECT lastAppliedRect = {};
};

struct ScrollTilingPlacement {
    HWND hwnd = nullptr;
    ScrollTilingRectPx rect = {};
};

struct ScrollTilingApplyResult {
    int requested = 0;
    int accepted = 0;
    int verified = 0;
    int failed = 0;
    unsigned long long durationMs = 0;
    std::uint64_t placedMask = 0;
    std::uint64_t verifiedMask = 0;
};

enum class ScrollTilingRestoreResult {
    Restored,
    ExternallyMoved,
    InvalidIdentity,
    Failed,
};

class ScrollTilingWindows {
public:
    // Returns ordinary top-level windows from the existing taskbar filter that
    // are currently safe to manage. The list is ordered and bounded here.
    static std::vector<ScrollTilingWindowRecord> Collect(
        const WindowList& windows, HMONITOR targetMonitor, int maximum,
        std::wstring* summary);
    static bool Capture(HWND hwnd, HMONITOR targetMonitor,
                        ScrollTilingWindowRecord* record,
                        std::wstring* reason);

    static bool StillSameWindow(const ScrollTilingWindowRecord& record);
    static bool IsSafeForLayout(const ScrollTilingWindowRecord& record);
    static bool IsOnCurrentVirtualDesktop(HWND hwnd);

    static bool CurrentRectMatches(HWND hwnd, const RECT& expected,
                                   int tolerancePx = 1,
                                   RECT* actual = nullptr);

    static bool Apply(std::span<const ScrollTilingPlacement> placements,
                      bool verify, ScrollTilingApplyResult* result);

    // Restores only when the window still has our last applied rectangle.
    // ExternallyMoved and InvalidIdentity are resolved outcomes: they must not
    // be retried later, because doing so would overwrite somebody else's move
    // or a reused HWND.
    static ScrollTilingRestoreResult Restore(const ScrollTilingWindowRecord& record);

    // Used only to compensate for a failed layout transaction. The caller is
    // still inside the operation that changed the windows, so a conditional
    // "was it moved by the user?" check is inappropriate here.
    static bool RestoreOriginal(const ScrollTilingWindowRecord& record);
};

} // namespace island
