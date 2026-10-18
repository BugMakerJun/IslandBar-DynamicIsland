// =====================================================================
//  IslandWindow.cpp
// =====================================================================
#include "app/IslandWindow.h"
#include "app/AppBar.h"
#include "domain/TaskStripLayout.h"
#include "core/Log.h"
#include "platform/Monitor.h"   // ShellTopBandThicknessPx: the band, once the work area no longer carries it

#include "domain/CardLayout.h"   // HitsTransportButton, for click routing

#include <dwmapi.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM

#include <algorithm>
#include <cmath>
#include <unordered_map>

#pragma comment(lib, "dwmapi.lib")

namespace island {
namespace {

std::unordered_map<HWND, IslandWindow*> g_windows;

// The Start card's hover target, spelled out for the log: the names are the enum's own, so a reader can
// follow one from the trace straight back into the code that produced it.
const wchar_t* StartHitTargetName(IslandWindow::StartHitTarget target) {
    switch (target) {
        case IslandWindow::StartHitTarget::Pin:         return L"pin";
        case IslandWindow::StartHitTarget::PickerRow:   return L"picker row";
        case IslandWindow::StartHitTarget::PowerRow:    return L"power row";
        case IslandWindow::StartHitTarget::PowerButton: return L"power button";
        case IslandWindow::StartHitTarget::EmptyGrid:   return L"empty grid";
    case IslandWindow::StartHitTarget::AllApps:     return L"all apps";
        case IslandWindow::StartHitTarget::None:        break;
    }
    return L"none";
}

// How often the pointer is re-checked, in milliseconds.
//
// This is a poll, not an event, and that is a deliberate trade for phase 1. With
// The window is not click-through; WM_NCHITTEST decides per point and therefore receives no
// WM_MOUSEMOVE at all while the pointer is in the click-through region, so
// "the pointer arrived" has to be discovered rather than delivered.
//
// 8 ms is under one frame at 120 Hz, so the expand still reads as instant. The
// handler early-outs when the pointer has not moved, so the steady-state cost is
// one GetCursorPos per tick.
constexpr UINT kPointerPollMs = 8;

constexpr UINT_PTR kPointerTimerId = 1;

// DPI-context helpers, loaded dynamically so the bar still starts on a build of
// Windows older than the per-monitor-v2 API, degrading to system DPI instead of
// failing to launch.
struct DpiApi {
    typedef DPI_AWARENESS_CONTEXT (WINAPI *SetThreadCtxFn)(DPI_AWARENESS_CONTEXT);
    typedef UINT (WINAPI *GetForWindowFn)(HWND);

    SetThreadCtxFn setThreadContext = nullptr;
    GetForWindowFn getForWindow = nullptr;

    DpiApi() {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (!user32) return;
        setThreadContext = reinterpret_cast<SetThreadCtxFn>(
            reinterpret_cast<void*>(GetProcAddress(user32, "SetThreadDpiAwarenessContext")));
        getForWindow = reinterpret_cast<GetForWindowFn>(
            reinterpret_cast<void*>(GetProcAddress(user32, "GetDpiForWindow")));
    }

    bool available() const { return setThreadContext != nullptr; }
};

const DpiApi& Dpi() {
    static const DpiApi api;
    return api;
}

// Pins the calling thread to per-monitor-v2 for the duration of a scope, so the
// rects handed to CreateWindowEx are interpreted in the target monitor's DPI.
class ScopedPerMonitorV2 {
public:
    ScopedPerMonitorV2() {
        if (Dpi().available()) {
            previous_ = Dpi().setThreadContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }
    ~ScopedPerMonitorV2() {
        if (Dpi().available() && previous_) {
            Dpi().setThreadContext(previous_);
        }
    }
private:
    DPI_AWARENESS_CONTEXT previous_ = nullptr;
};

} // namespace

IslandWindow::~IslandWindow() {
    Destroy();
}

bool IslandWindow::Create(const MonitorGeometry& monitor, bool show) {
    if (hwnd_) return false;

    monitor_ = monitor;
    if (monitor_.dpi == 0) monitor_.dpi = 96;
    scale_ = static_cast<float>(monitor_.dpi) / kDipPerInch;

    ScopedPerMonitorV2 pin;

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &IslandWindow::WndProcThunk;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;   // every pixel comes from the compositor
    wc.lpszClassName = kClassName;

    // A second registration for the same class fails with
    // ERROR_CLASS_ALREADY_EXISTS, which is not an error: it is process-wide.
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    const LONG heightPx = DipToPixels(kWindowHeightDip, monitor_.dpi);

    // Anchored to the TOP OF THE SCREEN, and it must stay that way. See SyncToMonitor for why
    // using the work area here closes a feedback loop with AppBar registration: reserving the
    // strip changes the work area, and a window that follows the work area then moves itself.
    rect_.left   = monitor_.screen.left;
    rect_.right  = monitor_.screen.right;
    rect_.top    = monitor_.screen.top;
    rect_.bottom = rect_.top + heightPx;

    const DWORD exStyle = WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW |
                          WS_EX_NOREDIRECTIONBITMAP;

    hwnd_ = CreateWindowExW(
        exStyle, kClassName, L"IslandBar",
        WS_POPUP,
        rect_.left, rect_.top, rect_.right - rect_.left, rect_.bottom - rect_.top,
        nullptr, nullptr, GetModuleHandleW(nullptr), this);

    if (!hwnd_) return false;

    g_windows[hwnd_] = this;

    // Confirm the DPI Windows actually associated with the window. A manifest
    // that was not picked up shows up here and would silently skew layout.
    if (Dpi().getForWindow) {
        const UINT windowDpi = Dpi().getForWindow(hwnd_);
        if (windowDpi) {
            monitor_.dpi = windowDpi;
            scale_ = static_cast<float>(windowDpi) / kDipPerInch;

            const LONG h = DipToPixels(kWindowHeightDip, monitor_.dpi);

            // screen.top, NOT work.top -- the same anchoring rule as everywhere else in this file.
            //
            // This is a DPI-correction path: it only runs when Windows reports a different DPI for the
            // window than for the monitor, which is rare enough that the mistake stayed hidden. With the
            // shell's taskbar showing, work.top is 32, so the strip was placed at y=32 instead of y=0 --
            // and an AppBar reservation moves the work area, so a window positioned from it is a window
            // a bar can push around. That is the same closed loop as the runaway reservation, in the
            // window rather than in the bar.
            rect_.top    = monitor_.screen.top;
            rect_.bottom = rect_.top + h;
        }
    }

    BOOL excluded = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_EXCLUDED_FROM_PEEK, &excluded, sizeof(excluded));

    // NO NON-CLIENT RENDERING. The window is a borderless composition surface; the only thing DWM's
    // non-client pass can add is a frame and a drop shadow -- and that shadow follows the window REGION,
    // which is a rectangle around the island, so it shows up as a soft box under the pill. Disabling it
    // removes that box without touching the client content (all of which is composed by the renderer).
    const DWMNCRENDERINGPOLICY noNonClient = DWMNCRP_DISABLED;
    DwmSetWindowAttribute(hwnd_, DWMWA_NCRENDERING_POLICY, &noNonClient, sizeof(noNonClient));

    Layout();

    // Start click-through: at rest the bar accepts no clicks until the pointer
    // actually enters the pill.

    pointerTimer_ = SetTimer(hwnd_, kPointerTimerId, kPointerPollMs, nullptr);

    // Reported, because everything about pointer entry depends on it.
    //
    // SetTimer returns 0 on failure, and every consequence of that failure is silent: no pointer
    // polling, so `pointerInside_` stays false forever, so the shape never promotes and no click
    // is ever accepted. The bar would look perfect and be completely inert, which is exactly the
    // symptom being chased. This line turns that into a fact.
    LogAndPrint("  pointer timer: %s (id %p, every %u ms)",
                pointerTimer_ ? "running" : "FAILED TO START",
                reinterpret_cast<void*>(static_cast<UINT_PTR>(pointerTimer_)),
                static_cast<unsigned>(kPointerPollMs));
    if (!pointerTimer_) {
        LogAndPrint("  pointer timer: hover and clicking CANNOT work without it");
    }

    ApplyInputRegion();

    shown_ = show;
    if (show) {
        SetWindowPos(hwnd_, HWND_TOPMOST,
                     rect_.left, rect_.top,
                     rect_.right - rect_.left, rect_.bottom - rect_.top,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        UpdatePointerState();
    }

    return true;
}

void IslandWindow::Show(bool show) {
    if (!hwnd_ || shown_ == show) return;

    shown_ = show;

    // SW_SHOWNOACTIVATE, never SW_SHOW: taking activation would pull the foreground away from
    // an application that is going full-screen, and the bar must never be the foreground window.
    ShowWindow(hwnd_, show ? SW_SHOWNOACTIVATE : SW_HIDE);

    // Re-asserted on the way back, because the Z order can be lost while hidden.
    if (show) {
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

void IslandWindow::Destroy() {
    if (pointerTimer_ && hwnd_) {
        KillTimer(hwnd_, pointerTimer_);
        pointerTimer_ = 0;
    }
    if (hwnd_) {
        g_windows.erase(hwnd_);
        if (IsWindow(hwnd_)) {
            DestroyWindow(hwnd_);
        }
        hwnd_ = nullptr;
    }
    destroyed_ = true;
}

// The band the shell's own taskbar keeps at the top of the screen, in DIPs.
//
// ONE function for it, because four places need the same number and they have to agree: the layout
// (which sizes the full-screen bar), the input region (a region shorter than the drawing CLIPS it),
// the anchor the frame loop reads, and the hit test. Measured from the work area rather than assumed,
// because the number is the user's setting (Settings > Personalisation > Taskbar).
float IslandWindow::BandHeightDip() const {
    const LONG inset = monitor_.work.top - monitor_.screen.top;
    if (inset > 0) return PixelsToDip(inset, monitor_.dpi);

    // NO INSET, AND THAT IS THE ORDINARY STATE WHILE THE BAR RUNS. The strip is released so that a
    // maximized window can reach the top of the screen (NativeTaskbar::ReleaseWorkArea), which puts the
    // work area at the whole monitor -- and this function used to read its number from exactly there.
    // The band itself has not changed: it is the strip the shell's taskbar occupies, and the taskbar
    // window still reports it while it is hidden. Reading it from the taskbar instead is what keeps the
    // four callers above agreeing with each other; a band of 0 would clamp to kMinBandHeightDip and the
    // full-screen bar would be 8 DIP tall instead of the strip.
    return PixelsToDip(island::ShellTopBandThicknessPx(monitor_.monitor), monitor_.dpi);
}

void IslandWindow::Layout() {
    if (!hwnd_ && rect_.right == 0) return;

    const float originX    = PixelsToDip(rect_.left, monitor_.dpi);
    const float originY    = PixelsToDip(rect_.top, monitor_.dpi);
    const float widthDip   = PixelsToDip(rect_.right - rect_.left, monitor_.dpi);

    // The full-screen state is exactly as tall as the band; see IslandAnchor::bandHeightDip.
    anchor_ = AnchorForWindow(originX, originY, widthDip, BandHeightDip());
    motion_.SetAnchor(anchor_);
}

bool IslandWindow::SetState(IslandState state) {
    return motion_.SetTarget(state);
}

bool IslandWindow::Tick(float dtSeconds) {
    return motion_.Tick(dtSeconds);
}

bool IslandWindow::TakePointerInsideChanged(bool* pointerInside) {
    if (!pointerInsideChanged_) return false;
    pointerInsideChanged_ = false;
    if (pointerInside) *pointerInside = pointerInside_;
    return true;
}

bool IslandWindow::TakeLayoutChanged() {
    if (!layoutChanged_) return false;
    layoutChanged_ = false;
    return true;
}

bool IslandWindow::TakeClick() {
    if (pendingClicks_ <= 0) return false;
    --pendingClicks_;
    return true;
}

bool IslandWindow::TakeRightClick() {
    if (pendingRightClicks_ <= 0) return false;
    --pendingRightClicks_;
    return true;
}

void IslandWindow::SetStartState(bool pageOpen, int page, int pinCount, int listCount,
                                 int pickerScrollRow) {
    startPageOpen_ = pageOpen;
    startPage_ = page;
    startPinCount_ = pinCount > 0 ? pinCount : 0;
    startListCount_ = listCount > 0 ? listCount : 0;
    startPickerScroll_ = pickerScrollRow > 0 ? pickerScrollRow : 0;
}

void IslandWindow::ScrollStartPicker(int notches) {
    if (notches == 0) return;
    int moved = startPickerScroll_ - notches;
    if (moved < 0) moved = 0;

    // How many rows fit: the same layout the drawing and the hit test use, from the card this window is
    // told the position of.
    int rowsOnScreen = 1;
    if (cardLeftDip_ > 0.0f) {
        const RoundedBox card =
            PlacedCardFor(BoxForState(IslandState::Collapsed, CurrentAnchor()), cardLeftDip_);
        const island::StartPageLayout layout = island::StartPageFor(card);
        if (layout.valid) {
            rowsOnScreen =
                static_cast<int>((layout.grid.bottom - layout.grid.top) / island::kStartRowHeight);
        }
    }
    const int lastFirstRow = startListCount_ - (rowsOnScreen + 1);
    if (lastFirstRow > 0 && moved > lastFirstRow) moved = lastFirstRow;
    if (startListCount_ <= rowsOnScreen + 1) moved = 0;
    startPickerScroll_ = moved;

    // ... and the answer to "which row is under the pointer" is recomputed HERE rather than left for the
    // next mouse move, because a wheel turn does not move the hand. The list slides under a stationary
    // pointer, so the row that was under it is now a different application: a stale answer highlights the
    // wrong row until the pointer happens to move again.
    POINT cursor = {};
    if (GetCursorPos(&cursor)) startHit_ = StartHitAtScreen(cursor);
}

IslandWindow::StartHit IslandWindow::StartHitAtScreen(POINT screenPoint) const {
    if (!hwnd_ || !startPageOpen_ || cardLeftDip_ <= 0.0f) return {};

    float localX = 0.0f;
    float localY = 0.0f;
    ToLocalDip(screenPoint, &localX, &localY);
    const RoundedBox card =
        PlacedCardFor(BoxForState(IslandState::Collapsed, CurrentAnchor()), cardLeftDip_);
    const island::StartPageLayout layout = island::StartPageFor(card);
    StartHit hit;
    if (island::StartRectHasPoint(layout.powerButton, localX, localY)) {
        hit.target = StartHitTarget::PowerButton;
        return hit;
    }
    if (startPage_ == 2) {
        const int row = island::StartPowerRowAt(card, localX, localY);
        if (row >= 0) {
            hit.target = StartHitTarget::PowerRow;
            hit.index = row;
        }
        return hit;
    }
    if (startPage_ == 1) {
        const int row =
            island::StartPickerRowAt(card, startPickerScroll_, startListCount_, localX, localY);
        if (row >= 0) {
            hit.target = StartHitTarget::PickerRow;
            hit.index = row;
        }
        return hit;
    }
    const int pin = island::StartPinCellAt(layout, startPinCount_, localX, localY);
    if (pin >= 0) {
        hit.target = StartHitTarget::Pin;
        hit.index = pin;
    } else if (island::StartRectHasPoint(layout.grid, localX, localY)) {
        hit.target = StartHitTarget::EmptyGrid;
    }
    return hit;
}

bool IslandWindow::TakePress(POINT* screenPoint) {
    if (pendingPresses_ <= 0) return false;
    --pendingPresses_;
    if (screenPoint) *screenPoint = lastPress_;
    return true;
}

RECT IslandWindow::InputRegionFor(const MonitorGeometry& monitor, IslandState state,
                                  const IslandAnchor& anchor, const RoundedBox& current,
                                  float cardLeftDip, int taskIconCount,
                                  float statusStripWidthDip, int trayIconCount,
                                  bool noticeStripUp) {
    // The bounding box of what the bar actually DRAWS, in window pixels.
    //
    // Horizontally it runs from the left of the state's own shape to the right of the longest icon row
    // that fits, so the row is inside however many applications are open.
    const float widthDip = PixelsToDip(monitor.screen.right - monitor.screen.left, monitor.dpi);
    const float scale = static_cast<float>(monitor.dpi) / kDipPerInch;

    // The island's own box for the state the bar is in, from the same function the renderer draws with,
    // so the region and the picture cannot disagree about where the bar is.
    const RoundedBox island = BoxForState(state, anchor);

    // The region must cover the ANIMATED shape, not just the state it is heading for.
    //
    // This is the bug that produced "it has turned into this": a wide flat dark bar with no content in
    // it. The width was taken from the target state, so while the card was collapsing the region stayed
    // 520 wide -- and the height was taken from the RESTING island, so it stayed 44. The result was a
    // 520 x 44 window region, which clipped the still-large card into exactly that shape.
    //
    // Measured on screen, at rest: row 12 showed the 212 px pill and row 40 showed a 520 px card, which
    // is a region whose two dimensions came from two different shapes.
    //
    // So the current box is used when the caller supplies one: the region then covers whatever is being
    // drawn, at every frame of a transition. It is passed in rather than read here because an animated
    // box belongs to the window that owns the motion, and this function is static so its geometry can be
    // asserted without a window.
    // A default-constructed box has zero width, which is the signal that no animated shape was
    // supplied: RoundedBox carries no validity flag of its own.
    const RoundedBox shape = (current.rect.right > current.rect.left) ? current : island;

    // The card hangs BELOW the bar, and the region has to follow it down.
    //
    // The renderer moves an outer card's top to just under the bar; without the same move here the
    // region would stop at the card's un-shifted top and the panel's lower half would be unclickable --
    // a region that is too small clips input exactly as it clips painting.
    //
    // The card's top comes from the RESTING pill, NOT from `island`.
    //
    // `island` is the box for the state the bar is heading to, and while a card is up that state is
    // Expanded -- a box that is itself 250 DIP tall. So `island.rect.bottom` was the card's own BOTTOM,
    // and the card's top was derived 214 DIP below where the renderer puts it. The derived box was then
    // only used for its left and right edges: the bottom of the region came from `shape` (the animated
    // box) instead, which left a region 42 DIP SHORT of the card the renderer paints.
    //
    // Measured on screen with the panel open: the panel ended in a hard straight cut through its last
    // row -- no rounded bottom, and the settings button, which the layout pins to the card's bottom, was
    // never painted at all. Region and painting have to agree, and this is the fourth time that rule has
    // had to be written in this file.
    const RoundedBox restingBar = BoxForState(IslandState::Collapsed, anchor);
    const float cardTopDip = restingBar.rect.bottom + kCardTopGapDip;
    // The card's FULL height rather than the animated one.
    //
    // Two reasons, both of them defects in the version that followed the animation. A region that grows
    // with the card is a SetWindowRgn call on every frame of the transition -- a window-manager call,
    // for an animation that has ~50 frames. And it cannot help: a point that is inside the region but
    // outside the drawn shapes is still answered HTTRANSPARENT by WM_NCHITTEST, so a generous region
    // costs nothing and passes the click through anyway.
    const float cardBottomDip = cardTopDip + kExpandedHeightDip;
    const bool cardUp = (cardLeftDip > 0.0f);
    RoundedBox shapeForRegion = shape;
    if (cardUp) {
        shapeForRegion.rect.top = cardTopDip;
        shapeForRegion.rect.bottom = cardBottomDip;
    }

    float left = shapeForRegion.rect.left;
    float right = shapeForRegion.rect.right;

    // The system panel's card reaches LEFT of the deck it hangs from.
    //
    // Its right edge is pinned to the island and its body extends 520 DIP to the left, so it starts at
    // roughly x=642 while the system button starts at 948. Taking the region's left edge from the button
    // -- which is what "cover the deck" means when no card is up -- therefore cut 306 DIP off the card:
    // measured on screen, only the RIGHTMOST tile column was visible and the rest of the panel was gone.
    //
    // Painted and regioned have to agree, and this is the third time that rule has had to be written in
    // this file. The card's own left edge is the only correct answer while a card is up.
    if (cardLeftDip > 0.0f) {
        const float cardLeft = cardLeftDip;
        if (cardLeft < left) left = cardLeft;
    }

    // ... AND THE CARD'S RIGHT EDGE, which is the same defect at the other end of the bar -- and the one
    // that took a photograph of the tray page to find.
    //
    // The right edge above came from `shape`, and `shape` is not the card: the card is PINNED to the
    // control that was pressed, while the shape is the island's own animated box. For the system panel,
    // which hangs under the button at the LEFT end, the card's right edge happens to fall inside the icon
    // row and nothing showed. The tray button is at the RIGHT end, its card is the same 520 DIP wide, and
    // with a single application window open the row ends before the card does.
    //
    // Measured on screen with the tray page open: the region was 670..1568 while the card ran from 1130
    // to 1650 -- 82 DIP of the card outside the region. The region clips PAINTING as well as input, so
    // the card came out with a hard square right edge, no rounded corner, and the rightmost tray icons
    // could not be clicked either. Reported as a screenshot of the tray page with "有个bug".
    //
    // The card's own rectangle is the only correct answer while a card is up, and this is the sixth time
    // that sentence has had to be written in this file.
    if (cardLeftDip > 0.0f) {
        const float cardRight = cardLeftDip + island::kExpandedWidthDip;
        if (cardRight > right) right = cardRight;
    }

    // The LEFT deck: the Start button and the system panel's toggle, which sit immediately to the left
    // of the island. Both have to be inside the region or they cannot be clicked at all -- and the
    // region is what decides that, not the hit test.
    //
    // FROM THE SURFACE THEY STAND ON, not from the system button: that surface is one capsule covering the
    // deck, the island and the tray button, and it reaches a pad FURTHER LEFT than the system button does
    // (see BarClusterFor). Measured here instead, the region stopped 4 DIP short of what is painted -- and
    // the region CLIPS, so the left end of the capsule came out with a square bite taken out of it where the
    // round end should be. Reported as a photograph of the bar with "看一下 这个地方缺了一角" round the
    // bottom-left of the deck.
    {
        const RoundedBox cluster = BarClusterFor(island, trayIconCount > 0);
        if (cluster.rect.left < left) left = cluster.rect.left;
    }

    // The STATUS STRIP reaches further left still, while it is up.
    //
    // It stands in the band to the left of the system button -- the date, the weekday, the time and the
    // weather, shown while the island is occupied (see StatusStripFor). It is drawn on the island's own
    // colour, so a strip outside the region is not a strip that is half missing, it is a capsule with a
    // hard bite taken out of its left end.
    //
    // Only while it is up: a zero width is "the island is idle" and leaves the band to the desktop
    // exactly as it was before this existed. Nothing is lost by covering it either way -- a point inside
    // the region but off the bar is still answered HTTRANSPARENT by WM_NCHITTEST, so clicks pass through
    // to whatever is underneath (see the note on the card's region below).
    if (statusStripWidthDip > 0.0f) {
        const RectF strip = StatusStripFor(island).rect;
        if (strip.left < left) left = strip.left;
    }
    // A strip that would start left of the screen is cut by the display anyway; keeping the region on
    // screen keeps its arithmetic honest.
    if (left < 0.0f) left = 0.0f;

    // The icon row starts past the system panel's card when one is open, and at the resting pill's right
    // edge otherwise -- the same rule the renderer lays the row out with, so the two agree by
    // construction rather than by margin.
    //
    // This has to be here and not only in the renderer. The region decides what can be CLICKED, so a row
    // that is drawn clear of the card but regioned underneath it is a row of icons that look right and
    // cannot be pressed.
    // The same notice-aware box the hit test uses (see ShouldAcceptPointer): while a call or a message is
    // being announced the bar's own box is the 420 DIP strip in every state, so the row, the deck and this
    // region cannot disagree with what is drawn -- with or without a card open.
    const RoundedBox restingIsland =
        IslandBoxFor(widthDip, noticeStripUp ? island::kNoticeStripWidthDip : kCollapsedWidthDip,
                     kCollapsedHeightDip);
    // Where the ICON ROW is measured from: the pill, or the notice strip while one is up. The row has to
    // start clear of whatever shape is beside it, and the region is what decides whether the icons can be
    // pressed at all.
    const RoundedBox rowIsland = RowAnchorBox(current, restingIsland);
    // ... and the tray button stands between the island and the row, so the row starts one button further
    // along (see RowAnchorWithTray). Without this the region stops short of the icons -- the row and the
    // region would disagree about where the row begins, and an icon outside the region cannot be clicked.
    const RoundedBox rowWithTray = RowAnchorWithTray(rowIsland, trayIconCount > 0);
    // A negative count means "size it for the most that could fit", which is what a caller with no
    // window list wants: the region must not change as windows open and close, or the resize is a
    // window-manager call per change for nothing.
    const int wanted = taskIconCount < 0 ? kMaxTaskIcons : taskIconCount;
    const int fitting = TaskIconsThatFit(widthDip, wanted);
    const float cardWidthDip = (cardLeftDip > 0.0f) ? kExpandedWidthDip : 0.0f;
    const float rowLeft =
        TaskRowFor(rowWithTray, fitting > 0 ? fitting : 1, cardLeftDip, cardWidthDip).left;
    // The row's right edge at its WIDEST, not at rest.
    //
    // This used the resting width of every button, which is 30 DIP each -- but a button the pointer is on
    // opens to 132 to show its window's title, and pushes the ones after it along. That is 102 DIP of row
    // that this calculation did not cover, so the piece that fell outside the window's region was the
    // OPENED BUTTON'S OWN TITLE, cut off by a hard vertical edge. Reported exactly that way: "the icons on
    // the right of the island are cut off when I point at them".
    //
    // Asked through the same function the hit test asks (TaskIconButtonAt with the last button opened),
    // so the clickable area and the painted area cannot be different sizes: the hit test already allowed
    // for the widened button, and it was the drawing side that was short.
    float rowRight = rowLeft;
    if (fitting > 0) {
        // From the row's own box, which the tray button has moved along -- the same box the icons are
        // DRAWN from, so the region cannot stop short of them.
        //
        // THE SAME BOX AS `rowLeft`, which is the whole point and was not true: the left edge above is
        // measured from `rowWithTray`, which follows the notice strip, while this end was measured from the
        // RESTING PILL. While a strip was up the two ends were therefore 100 DIP apart -- the region
        // stopped short of the row it is supposed to cover, and the region CLIPS, so the right-hand end of
        // the row and the tray icons beside it were never painted at all. Reported as "右侧还有少一部分",
        // with a photograph of the bar in exactly that state.
        rowRight = TaskIconButtonAt(rowWithTray, fitting - 1, fitting, fitting - 1, 1.0f).right;
    }
    if (rowRight > right) right = rowRight;

    // THE TRAY, at the monitor's right-hand margin.
    //
    // The fifth time this file has had to say it, and the first time it was learned the hard way on this
    // feature: the region is what decides which pixels this window owns, so a region that stops short of
    // the tray CLIPS it. Measured: the icons were found (16 of them), the frame was handed them, and the
    // bar drew nothing -- because the tray's strip is 238 DIP to the right of everything else the region
    // covered. See InputRegionFor's other notes for the same rule applied to the card, the deck and the
    // status strip.
    // The tray BUTTON needs no line of its own here: it stands between the island's right edge and the
    // row, both of which are already inside the region. (Its version at the window's right margin did need
    // one -- that is what the note above is about.)

    RECT region = {};
    region.left = static_cast<LONG>(left * scale);
    region.top = 0;
    region.right = static_cast<LONG>(right * scale);

    // As tall as what is DRAWN in the current state, and no more.
    //
    // The window is 240 DIP tall to give the card room, but nothing is drawn below the bottom of the
    // shape on screen. Sizing the region by the window made the whole strip belong to the bar, and the
    // ~200 DIP below the resting pill became a band that looked like empty desktop and swallowed every
    // click -- reported as "the area below the island cannot be clicked", with the region drawn on a
    // screenshot so there was no doubt which band was meant.
    //
    // Taken from the CURRENT state rather than from the tallest state: the card only exists while it is
    // open, and reserving its height permanently would leave the same defect with a smaller number.
    const TaskIconRow row = TaskRowFor(island, 1);
    float drawnBottomDip = cardUp ? cardBottomDip : shape.rect.bottom;
    if (row.valid && row.bottom > drawnBottomDip) drawnBottomDip = row.bottom;
    // A few DIP of slack so the antialiased bottom edge of the pill is inside the region.
    drawnBottomDip += 2.0f;

    region.bottom = static_cast<LONG>(drawnBottomDip * scale);

    // Never hand back an empty region: that would make the window unclickable everywhere, including on
    // the pill. A monitor too narrow for the arithmetic still gets a usable strip.
    if (region.right <= region.left) region.right = region.left + 1;
    if (region.bottom <= region.top) region.bottom = region.top + 1;
    return region;
}

void IslandWindow::ApplyInputRegion() {
    if (!hwnd_) return;

    const float originX = PixelsToDip(rect_.left, monitor_.dpi);
    const float originY = PixelsToDip(rect_.top, monitor_.dpi);
    const float widthDip = PixelsToDip(rect_.right - rect_.left, monitor_.dpi);
    // With the band, so that the region and the full-screen bar it must cover are the same height --
    // the only state whose height comes from the monitor rather than from a constant.
    const IslandAnchor anchor = AnchorForWindow(originX, originY, widthDip, BandHeightDip());

    // The card's left edge is passed so the region can follow the icon row past it. It is the value the
    // renderer was given, held on the window for exactly this reason: a plan for where the card goes that
    // only the renderer knows about is a plan the clickable area cannot keep up with.
    const RECT region =
        InputRegionFor(monitor_, motion_.Target(), anchor, motion_.Current(), cardLeftDip_,
                       taskButtonCount_, statusStripWidthDip_, trayIconCount_, noticeStripUp_);

    // Skipped when it is the same rectangle as last time.
    //
    // SetWindowRgn is a window-manager call, and the region only changes when the STATE changes -- not
    // per frame. Without this guard the region would have to be updated from a state-change hook, and a
    // missed hook would leave a stale region; with it, the caller can ask on every frame and the cost is
    // four integer comparisons.
    if (region.left == appliedRegion_.left && region.top == appliedRegion_.top &&
        region.right == appliedRegion_.right && region.bottom == appliedRegion_.bottom) {
        return;
    }

    HRGN rgn = CreateRectRgn(region.left, region.top, region.right, region.bottom);
    if (!rgn) return;

    // SetWindowRgn takes ownership of the region on success, and does NOT on failure -- so the failure
    // path deletes it. Getting this wrong leaks a GDI object per call.
    if (SetWindowRgn(hwnd_, rgn, TRUE) == 0) {
        DeleteObject(rgn);
        return;
    }
    appliedRegion_ = region;
    // Counted because a region that moves during an animation means a window-manager call per frame,
    // and that is invisible from the outside: the bar looks the same either way, and only this number
    // says whether the transition is paying for it. Reported in the frame-interval summary.
    ++regionApplications_;
}

void IslandWindow::ToLocalDip(POINT screenPoint, float* x, float* y) const {
    *x = PixelsToDip(screenPoint.x - rect_.left, monitor_.dpi);
    *y = PixelsToDip(screenPoint.y - rect_.top, monitor_.dpi);
}

// The island box the bar's two ends are laid out from, in window-local DIP.
//
// One helper because both ends must agree about where the middle is: the Start button hangs off the
// left edge and the icon row off the right, and a second derivation of the island could disagree with
// the first about where those edges are.
RoundedBox IslandWindow::LayoutIslandFor() const {
    const float widthDip = PixelsToDip(rect_.right - rect_.left, monitor_.dpi);
    // The RESTING pill in every state but the notice strip, which is genuinely wider and pushes the deck and
    // the icon row out with it. Asked through one rule (RowAnchorBox) so the region, the hit tests and the
    // drawing cannot disagree about where those controls are -- the failure mode being a button painted in
    // one place and clickable in another.
    const RoundedBox resting = IslandBoxFor(widthDip, kCollapsedWidthDip, kCollapsedHeightDip);
    return RowAnchorBox(motion_.Current(), resting);
}

bool IslandWindow::StartAtScreen(POINT screenPoint) const {
    if (!hwnd_) return false;

    float localX = 0.0f;
    float localY = 0.0f;
    ToLocalDip(screenPoint, &localX, &localY);
    return StartButtonHit(LayoutIslandFor(), localX, localY);
}

bool IslandWindow::PointerOverControl(POINT screenPoint) const {
    if (!hwnd_) return false;

    // Every one of these is the SAME predicate the click handler uses to decide what a press means, which is
    // the whole point: a hand that appears where a click would do nothing is worse than no hand at all.
    if (IconAtScreen(screenPoint) >= 0) return true;
    if (StartAtScreen(screenPoint) || SystemButtonAtScreen(screenPoint)) return true;

    // ... AND THE TRAY BUTTON IS ONE OF THE BUTTONS.
    //
    // Found by the check written beside it in Diagnostics: the tray was a control for the CLICK and not for
    // the CURSOR, so the three dots that open the tray page showed a plain arrow -- the same defect the
    // media card's transport buttons had, reported as "这些按钮 鼠标移上去 应该变成小手".
    if (TrayButtonAtScreen(screenPoint)) return true;

    // THE CARD'S CONTROLS ARE THE CONTROLS OF THE CARD THAT IS UP.
    //
    // Gated on the owner, and that gate is not tidiness: the panel's tiles and the media card's transport
    // buttons are both laid out in this window at all times (the frame loop fills the panel's text whether or
    // not the panel is showing), so the media card's own cover answered HAND -- a panel tile was behind that
    // point. Measured with the pointer parked on the cover, before this gate existed.
    //
    // The sound page is the panel's second level and its own hit test already answers None unless it is up.
    if (cardOwner_ == CardOwner::Music) {
        if (MusicControlAtScreen(screenPoint) != CardControl::None) return true;
    } else if (cardOwner_ == CardOwner::SystemPanel) {
        if (SystemPanelHitAtScreen(screenPoint).target != SystemPanelTarget::None) return true;
        if (SoundPageHitAtScreen(screenPoint).target != SoundPageTarget::None) return true;
    }
    return false;
}

int IslandWindow::IconAtScreen(POINT screenPoint) const {
    if (RowButtonCount() <= 0 || !hwnd_) return -1;

    // The same island geometry the pointer poll and the renderer use. Derived here rather than
    // remembered so it cannot go stale if the monitor or DPI changed.
    const float widthDip = PixelsToDip(rect_.right - rect_.left, monitor_.dpi);
    const RoundedBox island = IslandBoxFor(widthDip, kCollapsedWidthDip, kCollapsedHeightDip);
    // ... the row is measured from the same box the drawing uses: see LayoutIslandFor.
    // ... AND THE TRAY BUTTON MOVES IT ALONG, exactly as the drawing does: the row is drawn one button
    // further right and this is the hit test that has to agree with it. It did not, and the symptom was
    // precise: pointing at the FIRST icon opened the SECOND, because the hit test still thought the row
    // began where it used to. Reported as "鼠标移上去 第二个图片展开了".
    const RoundedBox rowIsland = RowAnchorWithTray(RowAnchorBox(motion_.Current(), island),
                                                   trayIconCount_ > 0);

    float localX = 0.0f;
    float localY = 0.0f;
    ToLocalDip(screenPoint, &localX, &localY);

    // The resting row, for the reason in the header: a button that has opened around the pointer must
    // not change the answer to "which icon did I click".
    const int icon = TaskIconUnderPointer(rowIsland, RowButtonCount(), localX, localY);

    // Reported on every click that reaches here.
    //
    // The row's hit test is the same one the pointer poll uses and it is known to work, so a click
    // that arrives and produces no icon points at the numbers rather than at the geometry -- which is
    // why they are printed. A click at a plausible x that answers -1 looks identical in the log to a
    // click that was never routed at all, and the two need different fixes.
    if (icon < 0) {
        LogLine(L"  icon hit test: MISS at local (%.0f,%.0f) DIP, %d icons, row starts at %.0f",
                localX, localY, RowButtonCount(), TaskRowFor(island, RowButtonCount()).left);
    }

    return icon;
}

bool IslandWindow::HitTestScreen(POINT screenPoint) const {
    return ShouldAcceptPointer(monitor_, rect_, screenPoint, motion_.Target(),
                               RowButtonCount(), cardLeftDip_, fullscreenApp_, trayIconCount_,
                               motion_.Current(), noticeStripUp_);
}

void IslandWindow::SetFullscreenApp(bool fullscreenApp) {
    fullscreenApp_ = fullscreenApp;
}

void IslandWindow::SetTrayIconCount(int count) {
    trayIconCount_ = count < 0 ? 0 : count;
}

void IslandWindow::OpenTrayMenu(int cell, int entryCount) {
    if (cell < 0 || trayIconCount_ <= 0 || cardLeftDip_ <= 0.0f) {
        CloseTrayMenu();
        return;
    }
    const RoundedBox card =
        PlacedCardFor(BoxForState(IslandState::Collapsed, CurrentAnchor()), cardLeftDip_);
    const island::TrayPageLayout page = TrayPageFor(card, trayIconCount_);
    if (cell >= page.count) {
        CloseTrayMenu();
        return;
    }
    // From the bottom-right corner of the cell: the menu hangs off the icon that was right clicked, the way
    // a context menu hangs off its object, and TrayMenuFor keeps it inside the card from there.
    const island::RectF& icon = page.cells[cell];
    trayMenuLayout_ = island::TrayMenuFor(card, icon.right, icon.bottom, entryCount);
    trayMenuOpen_ = trayMenuLayout_.count > 0;
    trayMenuRowHovered_ = -1;
}

void IslandWindow::SetSystemPanel(const TextRenderer::SystemPanelText& panel, float cardLeftDip) {
    systemPanel_ = panel;
    cardLeftDip_ = cardLeftDip;
}

void IslandWindow::SetStatusStripWidth(float widthDip) {
    if (widthDip < 0.0f) widthDip = 0.0f;
    if (widthDip == statusStripWidthDip_) return;
    statusStripWidthDip_ = widthDip;
    // Applied here rather than left to the frame loop's own call, because the region only changes when
    // the answer changes and this is where the answer changed. The frame loop calls it too; the second
    // call returns immediately when the rectangle is the same (see ApplyInputRegion).
    ApplyInputRegion();
}

SystemPanelHit IslandWindow::SystemPanelHitAtScreen(POINT screenPoint) const {
    SystemPanelHit hit;
    if (systemPanel_.tileCount <= 0 || !hwnd_) return hit;
    if (motion_.Target() != IslandState::Expanded) return hit;

    float localX = 0.0f;
    float localY = 0.0f;
    ToLocalDip(screenPoint, &localX, &localY);

    // The card as it was PLACED: the same rectangle the renderer composes the panel into.
    //
    // Both of its axes have been wrong here, one at a time, and each time the symptom was the same --
    // a tile that looks right and opens the wrong thing, or nothing. The LEFT edge was moved but the
    // TOP was left as the motion box's, which is the PILL's top (6 DIP) while the card hangs from the
    // pill's bottom (50): every painted tile was tested 44 DIP above itself, so the first row of tiles
    // reported the second row and the settings row at the bottom reported nothing.
    //
    // The arithmetic now lives in PlacedCardFor, because the music card's transport controls had the same
    // fault in both axes (HANDOFF 7.86) and a second copy would have been a second chance to differ.
    const RoundedBox card =
        PlacedCardFor(BoxForState(IslandState::Collapsed, CurrentAnchor()), cardLeftDip_);
    // The slider count comes from the panel that was last BUILT, not from a constant: the brightness row
    // exists only when the display answered for it, and a hit test that assumed two rows would put a press
    // between the sliders on the brightness track of a card that has none.
    return SystemPanelHitFor(card, systemPanel_.tileCount, systemPanel_.hasBrightness ? 2 : 1,
                             systemPanel_.pageIndex, systemPanel_.pageCount, localX, localY);
}

void IslandWindow::SetSoundPage(const TextRenderer::SoundPageText& page, float scroll) {
    soundPage_ = page;
    soundPageOpen_ = true;
    SetSoundPageScroll(scroll);
}

void IslandWindow::CloseSoundPage() {
    if (!soundPageOpen_) return;
    soundPageOpen_ = false;
    soundPage_ = TextRenderer::SoundPageText{};
    soundScroll_ = 0.0f;
}

void IslandWindow::SetSoundPageScroll(float scroll) {
    // Clamped HERE, from the same layout the drawing uses: a scroll the content cannot support would leave
    // the page blank with no way back, which is what "the page scrolled off the top" looks like.
    const RoundedBox card =
        PlacedCardFor(BoxForState(IslandState::Collapsed, CurrentAnchor()), cardLeftDip_);
    const float limit = SoundPageScrollLimit(card, soundPage_.deviceCount, soundPage_.mixCount);
    if (scroll < 0.0f) scroll = 0.0f;
    if (scroll > limit) scroll = limit;
    soundScroll_ = scroll;
}

bool IslandWindow::TakeSoundWheel(int* notches) {
    if (!notches) return false;
    const int pending = soundWheelNotches_;
    soundWheelNotches_ = 0;
    if (pending == 0) return false;
    *notches = pending;
    return true;
}

SoundPageHit IslandWindow::SoundPageHitAtScreen(POINT screenPoint) const {
    SoundPageHit hit;
    if (!soundPageOpen_ || !hwnd_) return hit;
    if (motion_.Target() != IslandState::Expanded) return hit;

    float localX = 0.0f;
    float localY = 0.0f;
    ToLocalDip(screenPoint, &localX, &localY);

    const RoundedBox card =
        PlacedCardFor(BoxForState(IslandState::Collapsed, CurrentAnchor()), cardLeftDip_);
    return SoundPageHitFor(card, soundPage_.deviceCount, soundPage_.mixCount, soundScroll_, localX,
                           localY);
}

int IslandWindow::SoundScrollJumpFor(POINT screenPoint) const {
    if (!soundPageOpen_) return 0;

    float localX = 0.0f;
    float localY = 0.0f;
    ToLocalDip(screenPoint, &localX, &localY);

    // The same layout the click was hit-tested against, so the thumb the press is compared with is the thumb
    // that was drawn.
    const RoundedBox card =
        PlacedCardFor(BoxForState(IslandState::Collapsed, CurrentAnchor()), cardLeftDip_);
    const SoundPageLayout layout =
        SoundPageFor(card, soundPage_.deviceCount, soundPage_.mixCount, soundScroll_);

    if (localY < layout.scrollThumb.top) {
        return -1;
    }
    if (localY > layout.scrollThumb.bottom) {
        return 1;
    }
    return 0;   // on the thumb: a drag, which this does not do
}

bool IslandWindow::PanelSliderFractionForDrag(int row, POINT screenPoint, float* fraction) const {
    if (!fraction || systemPanel_.tileCount <= 0) return false;
    float localX = 0.0f;
    float localY = 0.0f;
    ToLocalDip(screenPoint, &localX, &localY);

    const RoundedBox card =
        PlacedCardFor(BoxForState(IslandState::Collapsed, CurrentAnchor()), cardLeftDip_);
    // WHICH row, because there are two now: 0 is the volume and 1 the brightness, the same numbering the
    // hit test uses, and the row's own track is what the pointer is followed along.
    const SystemPanelLayout layout =
        SystemPanelFor(card, systemPanel_.tileCount, systemPanel_.hasBrightness ? 2 : 1,
                                                    systemPanel_.pageCount > 1,
                                                    systemPanel_.pageIndex, systemPanel_.pageCount);
    if (row < 0 || row >= layout.sliderCount) return false;

    const float width = layout.sliders[row].right - layout.sliders[row].left;
    if (width <= 1.0f) return false;
    float value = (localX - layout.sliders[row].left) / width;
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    *fraction = value;
    return true;
}

bool IslandWindow::MixerSliderFractionForDrag(int row, POINT screenPoint, float* fraction) const {
    if (!fraction || !soundPageOpen_ || row < 0) return false;
    float localX = 0.0f;
    float localY = 0.0f;
    ToLocalDip(screenPoint, &localX, &localY);

    const RoundedBox card =
        PlacedCardFor(BoxForState(IslandState::Collapsed, CurrentAnchor()), cardLeftDip_);
    const SoundPageLayout layout =
        SoundPageFor(card, soundPage_.deviceCount, soundPage_.mixCount, soundScroll_);
    if (row >= layout.sessionCount) return false;

    const float width = layout.mixerTracks[row].right - layout.mixerTracks[row].left;
    if (width <= 1.0f) return false;
    float value = (localX - layout.mixerTracks[row].left) / width;
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    *fraction = value;
    return true;
}

CardControl IslandWindow::MusicControlAtScreen(POINT screenPoint) const {
    // Only the expanded card has transport controls; the resting shapes do not.
    if (motion_.Target() != IslandState::Expanded) return CardControl::None;
    if (!HitTestScreen(screenPoint)) return CardControl::None;

    float localX = 0.0f;
    float localY = 0.0f;
    ToLocalDip(screenPoint, &localX, &localY);

    // Where the card is DRAWN, not where the motion's box is.
    //
    // `motion_.Current()` is centred on the screen with its top at the pill's top; the card is drawn
    // hanging below the bar with its left edge pinned to the control that opened it. Measured on a
    // 2560 monitor: drawn at x 853..1373 y 50..300, tested at x 1020..1540 y 6..256 -- so every transport
    // button was 167 DIP to the right and 44 DIP up from where it appears, and the controls could not be
    // pressed at all. See PlacedCardFor.
    const RoundedBox card =
        PlacedCardFor(BoxForState(IslandState::Collapsed, CurrentAnchor()), cardLeftDip_);
    return MusicControlAt(card, localX, localY);
}

IslandAnchor IslandWindow::CurrentAnchor() const {
    const float originX = PixelsToDip(rect_.left, monitor_.dpi);
    const float originY = PixelsToDip(rect_.top, monitor_.dpi);
    const float widthDip = PixelsToDip(rect_.right - rect_.left, monitor_.dpi);
    // The band goes with it: the frame loop reads this anchor to lay the bar out, and an anchor that
    // disagreed with the window's own would put the full-screen bar at a different height from the
    // region that has to cover it.
    return AnchorForWindow(originX, originY, widthDip, BandHeightDip());
}

bool IslandWindow::ShouldAcceptPointer(const MonitorGeometry& monitor, const RECT& windowRect,
                                       POINT screenPoint, IslandState state, int iconCount,
                                       float cardLeftDip, bool fullscreenApp, int trayIconCount,
                                       const RoundedBox& currentShape, bool noticeStripUp) {
    // THE BAR IS NOT THERE WHILE A FULL-SCREEN APPLICATION IS IN FRONT.
    //
    // Nothing of it is drawn in that state (see Renderer::RenderFrame), so there is nothing to point at
    // -- and accepting the pointer anyway would mean the pointer crossing the top of the screen during a
    // game opened the media card over it, which is the second half of what was reported: "如果音乐的时候也
    // 显示那就鼠标放灵动岛的时候 不要 显示出音乐卡片". A call or a message is a different state (the notice
    // strip), and that one is answered normally below.
    if (state == IslandState::FullBar) return false;

    const float originX    = PixelsToDip(windowRect.left, monitor.dpi);
    const float originY    = PixelsToDip(windowRect.top, monitor.dpi);
    const float widthDip   = PixelsToDip(windowRect.right - windowRect.left, monitor.dpi);

    // With the band, like the layout and the region: in the full-screen state the box the pointer has
    // to be tested against is the band's height, and a hit test that assumed the fallback constant
    // would answer for a bar that is not the one on screen.
    const IslandAnchor anchor = AnchorForWindow(
        originX, originY, widthDip,
        PixelsToDip(monitor.work.top - monitor.screen.top, monitor.dpi));

    const float localX = PixelsToDip(screenPoint.x - windowRect.left, monitor.dpi);
    const float localY = PixelsToDip(screenPoint.y - windowRect.top, monitor.dpi);

    // A point counts as "on the bar" if it is inside the current shape OR inside the resting pill
    // OR on one of the application icons.
    //
    // The first two are the union that keeps the hover state from oscillating: once the pill has
    // grown, the pointer may be outside the resting pill, and testing only the current shape would
    // report "outside" and start collapsing -- which moves the shape again, so the bar flickers.
    //
    // The icons are the part that was missing. They sit BESIDE the island, outside every one of its
    // shapes, so a pointer on an icon was reported as "outside the bar": the window stayed
    // click-through, the icon could not be hovered, and it could not be clicked. The icons were
    // drawn correctly and were completely inert.
    //
    // WHERE THE CONTROLS ARE is asked of the SAME box the drawing asks of: the resting pill in every state
    // but the notice strip, and the notice strip itself while one is up (see RowAnchorBox).
    //
    // This was the resting pill UNCONDITIONALLY, and the notice strip is 420 DIP wide against the pill's
    // 220 -- so while a call or a message was up, the Start button and the panel toggle were PAINTED 100
    // DIP to the left of where this function looked for them: visible, and unclickable. Reported as exactly
    // that ("通话或者通知的时候左侧是开始菜单和系统按钮点不了"), with a screenshot, during a call.
    //
    // The old line's reasoning is kept because it is right about CARDS: the row must not move as the state
    // changes, and RowAnchorBox answers with the resting pill for every width between the pill and a card.
    // The notice strip is the one shape that is meant to push the row along, and it is the one this now
    // follows. A default-constructed box means "no animated shape supplied" -- the same convention
    // InputRegionFor uses -- so the self-test's calls read exactly as they did.
    //
    // ... AND WHILE A CALL OR A MESSAGE IS BEING ANNOUNCED, THAT BOX IS THE NOTICE STRIP -- in every state,
    // including while a card is open. The strip is not a state of its own: the state while a card is up is
    // Expanded, so a test written against the state cannot see the notice at all, which is how this box came
    // to be the pill while the bar was drawing the strip. `noticeStripUp` is the frame loop's answer, set
    // once per frame (see SetNoticeStripUp), and it is the same answer the renderer and the region get.
    //
    // What it fixes, reported with a photograph of a call: with a card open the bar's own capsule collapsed
    // to the pill's 220 DIP and the call strip -- 420 -- was clipped into it ("是在通话中 但是长度你给缩回去了").
    const RoundedBox restingPill =
        IslandBoxFor(widthDip, noticeStripUp ? island::kNoticeStripWidthDip : kCollapsedWidthDip,
                     kCollapsedHeightDip);
    const RoundedBox island =
        (currentShape.rect.right > currentShape.rect.left) ? RowAnchorBox(currentShape, restingPill)
                                                          : restingPill;

    // ... AND WITH A FULL-SCREEN APPLICATION IN FRONT, ONLY THE ISLAND IS THE BAR.
    //
    // Asked for after a message came up over a game: "只要显示灵动岛就行 左右两侧的不用显示". Neither the
    // deck nor the icon row is DRAWN in that situation (see Renderer::RenderFrame), and a bar that accepted
    // the pointer there would be answering for buttons that are not on screen -- the pointer crossing the
    // top of the screen would count as "on an icon", which opens that icon's title and is the hover state
    // the card rules are built on.
    if (fullscreenApp) {
        const RoundedBox shape = BoxForState(state, anchor);
        const auto inside = [](const RoundedBox& box, float x, float y) {
            return x >= box.rect.left && x <= box.rect.right && y >= box.rect.top &&
                   y <= box.rect.bottom;
        };
        // The state's own shape as well as the resting island, for the reason the union above gives: a
        // message widens the island, and the widened strip is what the pointer is on.
        return inside(island, localX, localY) || inside(shape, localX, localY);
    }

    // The tray BUTTON is the bar, exactly as the application icons are: it is drawn beside the island,
    // outside every one of the state's shapes, so a button the pointer could not reach would be drawn and
    // completely inert -- the defect the application icons had before this same union was extended for
    // them. (The card's own grid needs nothing here: it is inside the card, and the card's column below
    // already accepts the pointer.)
    if (trayIconCount > 0 && TrayButtonHit(island, localX, localY)) return true;

    // The row, from the box that accounts for the tray button standing between it and the island.
    const RoundedBox rowIsland = RowAnchorWithTray(island, trayIconCount > 0);
    if (iconCount > 0 && TaskRowContains(rowIsland, iconCount, localX, localY)) {
        return true;
    }

    // The Start button is part of the bar whether or not any application is open.
    //
    // The same is true of the panel toggle beside it, which must be clickable with no applications open
    // at all -- that is exactly when a user goes looking for it.
    //
    // Answered from `localX`/`localY`, which this function already computed, rather than by asking the
    // window for them: this is a static function with no window to ask, and the coordinates it is given
    // are already relative to the same window-local origin.
    if (SystemButtonHit(island, localX, localY)) {
        return true;
    }
    if (StartButtonHit(island, localX, localY)) {
        return true;
    }

    // An outer card is part of the bar, wherever it was placed.
    //
    // The card is drawn hanging BELOW the bar with its left edge pinned to the control that opened it,
    // so it is not inside any of the state's own shapes -- and the union tested above is built from
    // those shapes only. Measured on a 2560-wide screen with the panel open: the card is x 853..1373
    // y 50..300 while the Expanded state's own box is x 1020..1540 y 6..256, so the leftmost column of
    // tiles and the whole bottom band answered "not the bar's" and were click-THROUGH -- the click went
    // to whatever window was behind them.
    //
    // The full card, not its animated part: the card's final rectangle is what the content is laid out
    // in and what the window region covers, and the three have to agree or a control is painted
    // somewhere it cannot be pressed.
    //
    // The COLUMN, from the bar's underside down -- not from the card's top edge. The 8 DIP gap between the
    // bar and the card is part of the journey from the pill onto the card; if the window does not accept the
    // pointer there, the pointer counts as having left the bar for those 8 DIP and the card is put away
    // exactly as the user moves down onto it. See CardColumnFor.
    if (cardLeftDip > 0.0f) {
        const RoundedBox restingBar = BoxForState(IslandState::Collapsed, anchor);
        const RectF column = CardColumnFor(restingBar, cardLeftDip);
        if (localX >= column.left && localX <= column.right && localY >= column.top &&
            localY <= column.bottom) {
            return true;
        }
    }

    const RoundedBox current = BoxForState(state, anchor);
    const RoundedBox resting = BoxForState(IslandState::Collapsed, anchor);

    return ContainsWithTolerance(current, localX, localY, kHitToleranceDip) ||
           ContainsWithTolerance(resting, localX, localY, kHitToleranceDip);
}

void IslandWindow::SetTaskIconCount(int count) {
    taskButtonCount_ = count > 0 ? count : 0;
    if (hoveredIcon_ >= RowButtonCount()) hoveredIcon_ = -1;
}

bool IslandWindow::SystemButtonAtScreen(POINT screenPoint) const {
    if (!hwnd_) return false;

    float localX = 0.0f;
    float localY = 0.0f;
    ToLocalDip(screenPoint, &localX, &localY);
    return SystemButtonHit(LayoutIslandFor(), localX, localY);
}

bool IslandWindow::TrayButtonAtScreen(POINT screenPoint) const {
    if (!hwnd_ || trayIconCount_ <= 0) return false;

    float localX = 0.0f;
    float localY = 0.0f;
    ToLocalDip(screenPoint, &localX, &localY);
    return TrayButtonHit(LayoutIslandFor(), localX, localY);
}


void IslandWindow::UpdatePointerState() {
    if (!hwnd_) return;

    POINT cursor = {};
    if (!GetCursorPos(&cursor)) return;

    // Early-out while the pointer sits still: this runs every 8 ms and the
    // answer can only change when the pointer moves.
    //
    // The trace below sits AFTER this check on purpose. Placing it before looked reasonable and
    // was useless: with the pointer motionless the early-out returned every time, so the trace
    // never printed a single line and the log looked as though the timer had never fired. A
    // diagnostic that is skipped by the very condition it exists to explain is worse than none.
    if (havePointer_ && cursor.x == lastPointer_.x && cursor.y == lastPointer_.y) {
        return;
    }
    lastPointer_ = cursor;
    havePointer_ = true;

    // Where the pointer is, relative to the island -- reported only when the ANSWER changes.
    //
    // This exists because "hover does nothing" cannot be diagnosed from the development machine:
    // the sandbox has its own cursor, so the one action that matters cannot be performed here.
    // Printing every poll was the first attempt and it produced four lines a second of "still
    // outside", which buries the one line that matters. Entering and leaving are what count.
    {
        const float localX = static_cast<float>(cursor.x - rect_.left) / scale_;
        const float localY = static_cast<float>(cursor.y - rect_.top) / scale_;

        const RoundedBox resting = BoxForState(IslandState::Collapsed, CurrentAnchor());
        const bool insideResting = localX >= resting.rect.left &&
                                   localX <= resting.rect.right &&
                                   localY >= resting.rect.top &&
                                   localY <= resting.rect.bottom;
        // Kept, not just logged: the details card belongs to the PILL, so the policy has to be able to tell
        // "the pointer is on the island" from "the pointer is somewhere else on the bar" -- an icon, a deck
        // button, or the empty stretch past the row. Reported as "the card should come up only when the
        // pointer enters the island".
        pointerOnIsland_ = insideResting;

        // And whether it is in the CARD's COLUMN, which is the pill's own territory as well.
        //
        // The card hangs BELOW the bar, so it is not "on the island"; and the pointer has to be able to walk
        // from the pill down onto it, because the transport buttons are there. The column starts at the bar's
        // UNDERSIDE and not at the card's top edge -- the 8 DIP gap between them is part of the journey, and
        // leaving it out put the card away mid-crossing. See CardColumnFor.
        pointerInCardColumn_ = false;
        if (cardLeftDip_ > 0.0f) {
            const RectF column = CardColumnFor(
                BoxForState(IslandState::Collapsed, CurrentAnchor()), cardLeftDip_);
            pointerInCardColumn_ = localX >= column.left && localX <= column.right &&
                                   localY >= column.top && localY <= column.bottom;
        }
        const bool accepted = ShouldAcceptPointer(monitor_, rect_, cursor, motion_.Target(),
                                                  RowButtonCount(), cardLeftDip_, false, 0,
                                                  motion_.Current(), noticeStripUp_);

        // `haveTrace_` distinguishes the first poll from a genuine transition.
        if (!haveTrace_ || accepted != lastAccepted_) {
            LogLine(L"  pointer %s: screen (%ld,%ld) = local (%.0f,%.0f) DIP | pill x[%.0f,%.0f] "
                    L"y[%.0f,%.0f] | insidePill=%d accepted=%d clickThrough=%d",
                    accepted ? L"ENTERED the island" : L"left the island", cursor.x, cursor.y,
                    localX, localY, resting.rect.left, resting.rect.right, resting.rect.top,
                    resting.rect.bottom, insideResting ? 1 : 0, accepted ? 1 : 0,
                    0);
            haveTrace_ = true;
            lastAccepted_ = accepted;
        }
    }

    // Which icon the pointer is on, and whether it is on the bar at all.
    //
    // Decided on the RESTING layout, not the current one, and that distinction is the whole point.
    //
    // Widening an icon pushes the ones after it to the right. Deciding "which icon is the pointer
    // on" from a layout that already has one widened therefore closes a loop: the pointer moves to
    // where the second icon was, but the second icon has moved right, so that point is still inside
    // the first one's widened extent -- the first stays widened and the second can never be reached.
    // The report was precisely "the pointer reaches the second icon and something else widens".
    //
    // On the resting layout the answer is a function of the pointer's position alone and does not
    // depend on the answer it is about to produce, which is what makes it stable.
    {
        const float widthDip = PixelsToDip(rect_.right - rect_.left, monitor_.dpi);
        // THE SAME RESTING PILL THE HIT TEST AND THE RENDERER USE. While a call or a message is being
        // announced the bar's own capsule is the NOTICE STRIP (420 DIP), and the icon row is laid out
        // from ITS right edge. This hover test was still measuring from the 220 DIP pill, so with a
        // notice up the drawn row sat under a different set of slots -- the pointer on the first drawn
        // icon was answered as a gap or another icon, and the button did not open: "有时候鼠标放上去
        // 没有展开". `noticeStripUp_` is the frame's own answer (SetNoticeStripUp), the same one the
        // hit test above and the renderer's frame.restingPill read.
        const RoundedBox island =
            IslandBoxFor(widthDip, noticeStripUp_ ? island::kNoticeStripWidthDip : kCollapsedWidthDip,
                         kCollapsedHeightDip);

        int hovered = -1;
        float localXReported_ = 0.0f;
        float localYReported_ = 0.0f;

        // The pointer's position in window-local DIP, computed once. The Start button is checked
        // outside the `taskButtonCount_ > 0` guard below because it is part of the bar whether or not any
        // application is open -- a Start button that only worked once something else was running would
        // be a strange thing to ship.
        const float localX = PixelsToDip(cursor.x - rect_.left, monitor_.dpi);
        const float localY = PixelsToDip(cursor.y - rect_.top, monitor_.dpi);
        localXReported_ = localX;
        localYReported_ = localY;

        if (taskButtonCount_ > 0) {
            // `hoveredIcon_` is passed in rather than ignored, because opening a button pushes the
            // ones after it to the right -- so the button that was under the pointer moves out from
            // under it. Deciding only from the resting layout would then report "between buttons"
            // and close the button being pointed at, which ALSO let the island open, because "not on
            // an icon" is what permits that.
            // From the row's own box, which the tray button has moved along: a hover test against the
            // island's edge answers for a row that is not the one on screen.
            //
            // ... AND THROUGH RowAnchorBox, exactly as IconAtScreen answers. This is the HOVER half of
            // the same rule the click half follows, and the two disagreed while a notice strip was up:
            // the strip is 200 DIP wider than the pill, the row is DRAWN from the strip's right edge,
            // and this test was still measuring from the resting pill -- so the point on the first
            // drawn icon was answered as "somewhere along the row" or as a different slot. A hover
            // test that names a different row from the one painted is the whole family of defect this
            // function has now been fixed for twice.
            const RoundedBox rowAnchor = RowAnchorBox(motion_.Current(), island);
            hovered = TaskIconToKeepOpen(RowAnchorWithTray(rowAnchor, trayIconCount_ > 0),
                                         RowButtonCount(), hoveredIcon_, localX, localY);
        }
        hoveredIcon_ = hovered;

        // The tray, from the monitor's own width: the button is anchored to the right-hand edge rather than
        // to the row, so the row growing must not move it (see TrayButtonFor).
        trayButtonHovered_ = trayIconCount_ > 0 && TrayButtonHit(island, localX, localY);

        // ... and the icon under the pointer when its CARD is up. The card is placed by the frame loop, so
        // this is the same placed rectangle the renderer drew into -- the same rule the music card's
        // controls and the panel's tiles follow (see PlacedCardFor).
        trayCellHovered_ = -1;
        if (trayIconCount_ > 0 && cardLeftDip_ > 0.0f) {
            // The anchor the window is laid out from, NOT a local one: the card is placed from the resting
            // pill's box, and the frame loop placed it from this same anchor.
            const RoundedBox card =
                PlacedCardFor(BoxForState(IslandState::Collapsed, CurrentAnchor()), cardLeftDip_);
            trayCellHovered_ = TrayPageCellAt(card, trayIconCount_, localX, localY);
        }

        // ... and the tray's own menu, when one is up: the rows it was OPENED with, not a fresh layout from
        // the pointer -- a menu whose box is recomputed from the pointer every frame walks away from the
        // pointer that is on it, and then the row under the pointer changes as it moves.
        trayMenuRowHovered_ = -1;
        if (trayMenuOpen_) {
            trayMenuRowHovered_ = TrayMenuRowAt(trayMenuLayout_, localX, localY);
        }

        // ... and the same question for the Start card, which is laid out by the same rule.
        startHit_ = {};
        if (startPageOpen_ && cardLeftDip_ > 0.0f) {
            const RoundedBox card =
                PlacedCardFor(BoxForState(IslandState::Collapsed, CurrentAnchor()), cardLeftDip_);
            const island::StartPageLayout layout = island::StartPageFor(card);
            if (island::StartRectHasPoint(layout.powerButton, localX, localY)) {
                startHit_.target = StartHitTarget::PowerButton;
            } else if (startPage_ == 0 && island::StartRectHasPoint(layout.allApps, localX, localY)) {
                startHit_.target = StartHitTarget::AllApps;
            } else if (startPage_ == 2) {
                const int row = island::StartPowerRowAt(card, localX, localY);
                if (row >= 0) {
                    startHit_.target = StartHitTarget::PowerRow;
                    startHit_.index = row;
                }
            } else if (startPage_ == 1) {
                const int row = island::StartPickerRowAt(card, startPickerScroll_, startListCount_,
                                                         localX, localY);
                if (row >= 0) {
                    startHit_.target = StartHitTarget::PickerRow;
                    startHit_.index = row;
                }
            } else {
                const int pin = island::StartPinCellAt(layout, startPinCount_, localX, localY);
                if (pin >= 0) {
                    startHit_.target = StartHitTarget::Pin;
                    startHit_.index = pin;
                } else if (island::StartRectHasPoint(layout.grid, localX, localY)) {
                    // The empty upper area itself: what the user presses to add something.
                    startHit_.target = StartHitTarget::EmptyGrid;
                }
            }
        }

        // Reported on change, like the icon row above and for the same reason: "the highlight is on the
        // wrong row", "the highlight is on nothing", and "the highlight does not move at all" are three
        // different defects that a screenshot cannot tell apart -- and the third is what "鼠标移上去卡卡的"
        // turned out to be, with the pointer moving and the answer never changing because nothing redrew.
        {
            const int target = static_cast<int>(startHit_.target);
            if (target != lastReportedStartTarget_ || startHit_.index != lastReportedStartIndex_) {
                lastReportedStartTarget_ = target;
                lastReportedStartIndex_ = startHit_.index;
                LogLine(L"  start hover: %s #%d (pointer local %.0f,%.0f, page %d, scroll %d)",
                        StartHitTargetName(startHit_.target), startHit_.index, localX, localY,
                        startPage_, startPickerScroll_);
            }
        }

        // The Start button is on the other end, from the same island box.
        startHovered_ = StartButtonHit(island, localX, localY);

        // The system panel toggle is on the same deck, from the same island box.
        systemHovered_ = SystemButtonHit(island, localX, localY);

        // Reported on change, so what the pointer is doing is visible in the log.
        //
        // "The row does not make room" and "the pointer is on no icon at all" are indistinguishable
        // from a screenshot, and they need opposite fixes: the first is the layout, the second is the
        // hit test. This line says which.
        if (hovered != lastReportedHovered_) {
            lastReportedHovered_ = hovered;
            LogLine(L"  icon hover: %s (pointer local %.0f,%.0f, %d icons)",
                    hovered < 0 ? L"none" : L"a button", localXReported_, localYReported_,
                    taskButtonCount_);
        }
    }

    const bool inside =
        ShouldAcceptPointer(monitor_, rect_, cursor, motion_.Target(), RowButtonCount(),
                            cardLeftDip_, false, 0, motion_.Current(), noticeStripUp_);
    if (inside != pointerInside_) {
        pointerInside_ = inside;
        pointerInsideChanged_ = true;
    }

    // Only accept clicks while the pointer is actually on the bar. Staying
    // click-through is what lets the rest of the strip pass through to the
    // desktop.
}

void IslandWindow::ApplyPointerDecision(bool accept) {
    const bool wasInside = pointerInside_;
    pointerInside_ = accept;
    if (wasInside != accept) {
        pointerInsideChanged_ = true;
    }
}

void IslandWindow::SyncToMonitor() {
    if (!hwnd_) return;

    monitor_ = QueryMonitor(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
    scale_ = static_cast<float>(monitor_.dpi) / kDipPerInch;

    const LONG heightPx = DipToPixels(kWindowHeightDip, monitor_.dpi);

    // The window hangs from the TOP OF THE SCREEN, not from the top of the work area.
    //
    // Its position must not depend on the work area at all. It used to use work.top, on the
    // reasoning that the work area's top edge is where the taskbar ends. That is true for a
    // passive overlay and wrong for a bar that registers as an AppBar, because registering
    // CHANGES the work area and the shell then broadcasts WM_SETTINGCHANGE -- which this window
    // handles by calling SyncToMonitor(). The result was a closed loop: reserving space moved the
    // window down, which moved the island down, and every maximized window went with it. The
    // report was "all application windows have moved to the very bottom of the screen, including
    // the island".
    //
    // Anchored to the screen edge instead, the island's position is a fixed property of the
    // display. Nothing the shell does to the work area can move it, so the loop cannot close, and
    // the reserved strip and the drawn pill stay in the same place by construction.
    rect_.left   = monitor_.screen.left;
    rect_.right  = monitor_.screen.right;
    rect_.top    = monitor_.screen.top;
    rect_.bottom = rect_.top + heightPx;

    SetWindowPos(hwnd_, HWND_TOPMOST,
                 rect_.left, rect_.top,
                 rect_.right - rect_.left, rect_.bottom - rect_.top,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);

    // The anchor moved, so the pill has to be re-derived. The state and any
    // animation in flight are preserved: a DPI change must not restart the motion.
    Layout();
    layoutChanged_ = true;
    ApplyInputRegion();
}

bool IslandWindow::PumpMessages() {
    MSG msg = {};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return false;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !destroyed_ && hwnd_ != nullptr;
}

LRESULT CALLBACK IslandWindow::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    IslandWindow* self = nullptr;

    if (message == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<IslandWindow*>(cs->lpCreateParams);
        if (self) {
            self->hwnd_ = hwnd;
        }
    } else {
        auto it = g_windows.find(hwnd);
        if (it != g_windows.end()) self = it->second;
    }

    if (self) {
        return self->WndProc(message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT IslandWindow::WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        // Never take activation, not even when clicked.
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        // Region-level pass-through, decided HERE rather than by toggling WS_EX_TRANSPARENT.
        //
        // WS_EX_TRANSPARENT makes Windows skip hit testing for the window ENTIRELY: it does not ask
        // WM_NCHITTEST at all, it just hands the click to whatever is underneath. So while that style
        // was set, the only thing that could let a click through to the bar was the pointer poll
        // clearing it first -- and the poll runs on an 8 ms timer while the click arrives whenever the
        // user clicks. A click in that gap went to the desktop and did nothing.
        //
        // That is exactly what the log showed: 28 clicks reached the window procedure and only 4 were
        // routed. The other 24 arrived before the poll had cleared the style.
        //
        // HTTRANSPARENT answers the same question -- "is this point the bar's or the desktop's" -- but
        // it answers it DURING the message that carries the click, so there is no gap to lose it in.
        // The style is no longer toggled at all.
        case WM_NCHITTEST: {
            // lParam carries signed screen coordinates in two 16-bit halves, so
            // the extracting macros matter: hand-written masks would not sign
            // extend and would break on a multi-monitor desktop with negative
            // coordinates.
            const POINT screenPoint = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            return HitTestScreen(screenPoint) ? HTCLIENT : HTTRANSPARENT;
        }

        // THE CURSOR SAYS WHAT CAN BE PRESSED.
        //
        // The window class registers the plain arrow and nothing ever changed it, so every control on this bar
        // looked exactly like the bar's own surface: the media card's previous / play-pause / next, the icon
        // row, the deck buttons and the panel's tiles were all arrow-shaped until they were clicked. Asked for
        // in as many words: "这些按钮 鼠标移上去 应该变成小手".
        //
        // Set here rather than from the frame loop because this is the message Windows sends for exactly this
        // question, and it is sent again whenever the pointer moves or the mouse is clicked -- so the answer is
        // never stale, however the shape changed underneath. Only HTCLIENT is answered: anywhere else the
        // window has already told the system the point is not its own (see WM_NCHITTEST above).
        case WM_SETCURSOR: {
            if (LOWORD(lParam) != HTCLIENT) break;
            POINT cursor = {};
            const bool overControl = GetCursorPos(&cursor) && PointerOverControl(cursor);
            SetCursor(LoadCursorW(nullptr, overControl ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }

        case WM_ERASEBKGND:
            return 1;   // nothing to erase: the compositor owns every pixel

        default:
            break;
    }

    // The shell's AppBar callbacks arrive on the regular message channel with a
    // registered message id, so they cannot be given a case label -- the value is
    // not known until RegisterWindowMessage has run. They are checked in the
    // default path instead.
    //
    // ABN_POSCHANGED has to be acted on, not merely noted: it means another bar
    // moved and our reserved strip is stale. Ignoring it is what makes a bar
    // drift out of sync with the space it claims.
    if (appBar_ && appBar_->HandleCallback(hwnd_, message, wParam, lParam)) {
        return 0;
    }

    switch (message) {

        // A left click on the pill. The window is click-through everywhere else, so
        // reaching this at all means the pointer was on the island -- that is what
        // makes "click to dismiss" possible without ever taking focus.
        //
        // Recorded, not acted on: the meaning of a click is policy and belongs in
        // the frame loop, next to the notification state it affects.
        case WM_LBUTTONUP: {
            // Logged because a click that never arrives and a click that arrives and is ignored
            // look identical from outside the process, and the two need opposite fixes. This is
            // the only point that can tell them apart.
            POINT where = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ClientToScreen(hwnd_, &where);
            wchar_t local[96] = {};
            float localX = 0.0f;
            float localY = 0.0f;
            ToLocalDip(where, &localX, &localY);
            std::swprintf(local, 96, L"click at screen (%ld,%ld) = local (%.0f,%.0f) DIP",
                          where.x, where.y, localX, localY);
            LogAndPrint("  %ls", local);
            ++pendingClicks_;
            // The other half of the capture: whoever is holding the mouse gives it back, so the drag the
            // press started can be ended wherever the pointer happens to be.
            leftDown_ = false;
            if (GetCapture() == hwnd_) ReleaseCapture();
            return 0;
        }

        // A LEFT BUTTON PRESS, which is a different event from a click and had no handler at all.
        //
        // The bar acted on WM_LBUTTONUP -- and for a button that is right, but for a SLIDER it is the whole
        // problem: a drag is press-move-release, and with only the release reported, the volume jumped once
        // when the button came up at wherever the pointer had been dragged to, with nothing following the
        // pointer in between. Reported as "声音进度条那个不顺滑".
        //
        // Recorded, not acted on: what a press means is policy, and it belongs in the frame loop with
        // everything else.
        case WM_LBUTTONDOWN: {
            POINT where = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(hwnd_, &where);
            lastPress_ = where;
            ++pendingPresses_;

            // CAPTURED, and that is what makes a drag a drag.
            //
            // Without the capture the window stops hearing about the mouse the moment the pointer leaves
            // it, and a slider's whole gesture is "press here, move anywhere, release" -- the pointer
            // crosses out of the row, out of the card and off the bar on its way to the end of the track.
            // The messages that matter are then delivered to whatever is under the pointer instead, and the
            // slider stops following. `GetAsyncKeyState` was tried first as a way of tracking the button
            // without capturing, and it reported the button UP five milliseconds after a synthetic press
            // that was still held down: the state it reports is not the state the drag needs.
            //
            // Released on the button-up, and only if this window is the one holding it.
            leftDown_ = true;
            SetCapture(hwnd_);
            return 0;
        }

        // A RIGHT CLICK, counted rather than acted on, like the left one: what it means is policy and
        // belongs in the frame loop. It exists for the tray page's cells -- right-clicking one of the
        // shell's tray icons is how its application's own menu opens, which is the only way to reach an
        // item like 退出 -- and the window is the only thing that hears the click.
        case WM_RBUTTONUP: {
            POINT where = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(hwnd_, &where);
            float localX = 0.0f;
            float localY = 0.0f;
            ToLocalDip(where, &localX, &localY);
            wchar_t local[96] = {};
            std::swprintf(local, 96, L"right click at screen (%ld,%ld) = local (%.0f,%.0f) DIP",
                          where.x, where.y, localX, localY);
            LogAndPrint("  %ls", local);
            ++pendingRightClicks_;
            return 0;
        }

        case WM_TIMER:
            if (wParam == kPointerTimerId) {
                UpdatePointerState();
            }
            return 0;

        // The wheel, for the one thing on this bar that scrolls: the 声音输出 page's list.
        //
        // Counted here and taken by the frame loop. A wheel notch is a delta and nothing else -- there is
        // nowhere to read it back from -- so it has to be recorded the moment it arrives; but the OFFSET it
        // moves belongs to the frame loop, which is the one that lays the page out and draws it. The first
        // version of this scrolled the window's own copy instead, and the frame loop's next
        // SetSoundPage() wrote its (unchanged) offset straight back over it: the page could not be
        // scrolled at all, which is what "声音输出的位置应该可以用鼠标滚轮控制" asked for.
        case WM_MOUSEWHEEL: {
            // The two card levels that scroll: the 声音输出 page's list and the Start card's picker. The
            // wheel is counted for whichever of them is up -- the frame loop decides what the notches move,
            // because it owns the offsets.
            const bool startPickerUp = startPageOpen_ && startPage_ == 1;
            if (!soundPageOpen_ && !startPickerUp) break;

            // Only over the card. The window spans the whole bar strip at the top of the screen, and
            // swallowing the wheel anywhere in it would be stealing input from whatever is underneath.
            POINT cursor = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            float localX = 0.0f;
            float localY = 0.0f;
            ToLocalDip(cursor, &localX, &localY);
            const RoundedBox card =
                PlacedCardFor(BoxForState(IslandState::Collapsed, CurrentAnchor()), cardLeftDip_);
            if (localX < card.rect.left || localX > card.rect.right || localY < card.rect.top ||
                localY > card.rect.bottom) {
                break;
            }

            // Forwarded to the pointer position the frame loop will see anyway, so the two agree about
            // where the wheel happened.
            //
            // The delta ACCUMULATES before it becomes notches. Dividing each message by WHEEL_DELTA and
            // discarding the remainder throws away every message a precision touchpad sends (they come in
            // tens, not hundreds), and it also divides a coarse wheel's own motion into notches the list
            // never sees. What is left over is kept for the next message.
            soundWheelRemainder_ += GET_WHEEL_DELTA_WPARAM(wParam);
            const int notches = soundWheelRemainder_ / WHEEL_DELTA;
            if (notches != 0) {
                soundWheelRemainder_ -= notches * WHEEL_DELTA;
                soundWheelNotches_ += notches;
                return 0;
            }
            break;
        }

        case WM_DISPLAYCHANGE:
            SyncToMonitor();
            return 0;

        case WM_DPICHANGED:
            SyncToMonitor();
            return 0;

        case WM_SETTINGCHANGE:            // The work area changes when the taskbar auto-hides, moves edge or
            // changes thickness. Re-derive instead of caching, but note that
            // SetWindowPos from inside this message is what the shell expects.
            SyncToMonitor();
            return 0;

        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;

        case WM_DESTROY:
            g_windows.erase(hwnd_);
            hwnd_ = nullptr;
            destroyed_ = true;
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }

    // A count of what actually arrives at this window, reported once a second.
    //
    // `WM_TIMER` delivering is the precondition for all pointer handling, and it is invisible:
    // SetTimer reports success, and if the message never arrives the bar simply never notices the
    // pointer. Counting the deliveries turns "the timer is running" into "the timer is being
    // received", which are different claims.
    {
        static unsigned timers = 0;
        static unsigned mouseMoves = 0;
        static unsigned hits = 0;
        static DWORD lastReport = 0;
        if (message == WM_TIMER) ++timers;
        if (message == WM_MOUSEMOVE) ++mouseMoves;
        if (message == WM_NCHITTEST) ++hits;

        const DWORD now = GetTickCount();
        if (lastReport == 0) lastReport = now;
        if (now - lastReport >= 1000) {
            lastReport = now;
            LogLine(L"  messages/s: WM_TIMER=%u WM_MOUSEMOVE=%u WM_NCHITTEST=%u",
                    timers, mouseMoves, hits);
            timers = mouseMoves = hits = 0;
        }
    }

    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

} // namespace island
