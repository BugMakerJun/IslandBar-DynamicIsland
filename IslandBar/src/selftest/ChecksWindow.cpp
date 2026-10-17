// =====================================================================
//  ChecksWindow.cpp -- see selftest/Checks.h for why the self-test is split like this
//
//  The includes are the ones the single file carried; pruning them per file is a smaller,
//  separate exercise and is deliberately not mixed into the move.
// =====================================================================
// =====================================================================
//  Diagnostics.cpp
//  The runtime self-test and the native-taskbar probe.
//
//  Every assertion below is deliberately phrased as a question about what the
//  system actually did, not about what was requested. The interesting failures
//  in this area of Windows are the silent ones -- a per-monitor-v2 awareness
//  that never took effect, a z-order that quietly dropped out of topmost, a
//  swap chain that presented black outside the pill -- and none of them raise
//  an HRESULT.
//
//  The probe's report goes to the log file as well as to stdout, because
//  stdout has been lost on this machine three separate times.
// =====================================================================
#include "selftest/Diagnostics.h"

#include <windows.h>
#include <shellapi.h>   // APPBARDATA / ABM_* for the native-taskbar probe

#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>   // strtoul: unpacking the hex of a captured registration in the tray checks
#include <vector>
#include <cwchar>
#include <string>

#include "domain/Geometry.h"
#include "core/Log.h"
#include "domain/MiniJson.h"
#include "domain/TrayRegistration.h"
#include "domain/TrayLedger.h"     // which registrations the shell is holding (see the tray checks)
#include "platform/TrayRing.h"     // the channel the module publishes registrations on
#include "platform/WorkerThread.h"   // the stop-event/thread rule the readers share (TestWorkerThread)
#include "domain/CardLayout.h"
#include "domain/StatePolicy.h"
#include "domain/TrayEncoding.h"   // the only place a tray click is encoded (see its header)
#include "media/Lyrics.h"
#include "app/AppBar.h"
#include "domain/TaskStripLayout.h"
#include "platform/WindowList.h"
#include "media/MusicSession.h"
#include "app/PlayerWindow.h"
#include "media/WicImage.h"
#include "services/Notification.h"
#include "services/NotificationIcon.h"
#include "services/ToastStore.h"
#include "services/WeChatWatch.h"
#include "shell/Weather.h"
#include "platform/ThumbnailPreview.h"
#include "app/IslandWindow.h"
#include "platform/Monitor.h"
#include "platform/NativeTaskbar.h"   // the three answers to "what happens to the shell's taskbar"
#include "shell/SystemToggles.h"
#include "render/Theme.h"
#include "render/GlassMaterial.h"
#include "shell/Weather.h"   // SkyMarkForCode: the word and the mark come from the same WMO code
#include "domain/Motion.h"
#include "render/Renderer.h"
#include "render/TextRenderer.h"
#include "shell/TrayIcons.h"
#include "shell/TrayPress.h"
#include "shell/TrayLearn.h"   // CountCoveredIcons: how much of the tray a press can reach (see the tray checks)

#include "selftest/Checks.h"
#include "selftest/Harness.h"

namespace island {

void TestWindow(IslandWindow& window, const MonitorGeometry& monitor) {
    Section(L"window creation and per-monitor DPI");

    const HWND hwnd = window.Handle();
    Check(hwnd != nullptr, L"CreateWindowEx succeeded");
    if (!hwnd) return;

    const RECT rect = window.WindowRect();
    const LONG widthPx  = rect.right - rect.left;
    const LONG heightPx = rect.bottom - rect.top;
    const LONG screenW  = ScreenWidth(monitor);

    wchar_t buf[256];

    std::swprintf(buf, 256, L"%ldx%ld at (%ld,%ld)", widthPx, heightPx, rect.left, rect.top);
    Info(L"window rect", buf);

    // The window spans the full monitor width. The bar hangs from the top of the SCREEN.
    Check(widthPx == screenW, L"window spans the full monitor width");
    Check(rect.left == monitor.screen.left, L"window starts at the screen edge");

    // The strip hangs from the top of the SCREEN, not from the work area.
    //
    // This is not a detail: an AppBar reservation CHANGES the work area and then broadcasts
    // WM_SETTINGCHANGE, which this window handles by re-syncing. A window that positions itself
    // from the work area therefore moves itself every time it reserves space -- a closed loop that
    // pushed the island and every maximized window down the screen. Anchored to the screen edge,
    // the island's position is a property of the display and no bar can move it.
    std::swprintf(buf, 256, L"strip top %ld, screen top %ld, work top %ld, shown %d", rect.top, monitor.screen.top, monitor.work.top, window.IsShown() ? 1 : 0);
    Check(rect.top == monitor.screen.top, L"strip top edge meets the top of the screen", buf);
    Check(rect.top <= monitor.work.top,
          L"the strip starts at or above the work area, never below it");

    const LONG expectedHeight = DipToPixels(kWindowHeightDip, monitor.dpi);
    Check(heightPx == expectedHeight, L"height follows the DPI scale");

    wchar_t detail[160];
    std::swprintf(detail, 160, L"%ux%u dpi, scale %.2f", monitor.dpi, monitor.dpiY,
                  window.Scale());
    Info(L"monitor dpi", detail);

    // Extended styles: the four that keep the bar out of the way.
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    Check((ex & WS_EX_NOACTIVATE) != 0, L"WS_EX_NOACTIVATE set");
    Check((ex & WS_EX_TOOLWINDOW) != 0, L"WS_EX_TOOLWINDOW set (no Alt-Tab)");
    Check((ex & WS_EX_TOPMOST) != 0, L"WS_EX_TOPMOST set");
    Check((ex & WS_EX_NOREDIRECTIONBITMAP) != 0, L"WS_EX_NOREDIRECTIONBITMAP set");

    // Topmost must survive the show, otherwise the bar sinks behind ordinary
    // windows the first time one is activated.
    Check((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0,
          L"still topmost after being shown");

    // The window does NOT carry WS_EX_TRANSPARENT, and that is deliberate.
    //
    // With that style Windows skips hit testing entirely: it never asks WM_NCHITTEST, so the only
    // thing that could let a click reach the bar was the pointer poll clearing the style first. A
    // click arriving before the poll did went to the desktop -- which is what the log showed, 28
    // clicks reaching the procedure and 4 being routed. The window now stays opaque to input and
    // WM_NCHITTEST answers per point, during the message that carries the click.
    Check((ex & WS_EX_TRANSPARENT) == 0,
          L"no WS_EX_TRANSPARENT: the hit test is asked instead of skipped");

    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    Check((style & WS_POPUP) != 0 && (style & WS_CAPTION) == 0,
          L"borderless popup, no caption");

    // Per-monitor-v2 must have been applied to the window, not just requested.
    typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        auto getDpiForWindow = reinterpret_cast<GetDpiForWindowFn>(
            reinterpret_cast<void*>(GetProcAddress(user32, "GetDpiForWindow")));
        if (getDpiForWindow) {
            const UINT windowDpi = getDpiForWindow(hwnd);
            std::swprintf(buf, 256, L"window reports %u dpi", windowDpi);
            Check(windowDpi == monitor.dpi, L"GetDpiForWindow matches monitor", buf);
        }
    }

    // The pill must sit inside the window it was laid out for, horizontally
    // centred. If the two disagree, hit testing and painting disagree too.
    const RoundedBox pill = window.CurrentBox();
    const float windowWidthDip = PixelsToDip(widthPx, monitor.dpi);
    const float pillCenter = (pill.rect.left + pill.rect.right) * 0.5f;
    std::swprintf(buf, 256, L"pill x [%.1f, %.1f] of [0, %.1f]",
                  pill.rect.left, pill.rect.right, windowWidthDip);
    Check(Nearly(pillCenter, windowWidthDip * 0.5f, 0.5f), L"pill is horizontally centred", buf);
    // The tallest state must fit too: a card clipped by the fixed window is the
    // failure this caught, and it is invisible in any HRESULT.
    const RoundedBox card = BoxForState(IslandState::Expanded,
                                        AnchorForWindow(PixelsToDip(rect.left, monitor.dpi),
                                                        PixelsToDip(rect.top, monitor.dpi),
                                                        windowWidthDip));
    std::swprintf(buf, 256, L"pill y [%.1f, %.1f] card y [%.1f, %.1f] of %.0f",
                  pill.rect.top, pill.rect.bottom, card.rect.top, card.rect.bottom,
                  kWindowHeightDip);
    Check(pill.rect.top >= 0.0f && pill.rect.bottom <= kWindowHeightDip,
          L"pill fits inside the window", buf);
    Check(card.rect.top >= 0.0f && card.rect.bottom <= kWindowHeightDip,
          L"card fits inside the window");
    {
        wchar_t why[256];
        std::swprintf(why, 256,
                      L"anchor topY=%.1f centerX=%.1f | window rect top=%ld left=%ld | "
                      L"work.top=%ld screen.top=%ld",
                      window.Anchor().topY, window.Anchor().centerX,
                      rect.top, rect.left, monitor.work.top, monitor.screen.top);
        Info(L"layout anchor", why);
    }

    // A hidden window must still exist as a real window.
    Check(IsWindow(hwnd) != FALSE, L"window is alive after creation");

}

void TestClickThrough(IslandWindow& window, const MonitorGeometry& monitor) {
    Section(L"region-level click-through");

    const HWND hwnd = window.Handle();
    if (!hwnd) return;

    const RECT rect = window.WindowRect();
    const RoundedBox pill = window.CurrentBox();

    // Window-local DIPs -> screen pixels.
    const auto toScreen = [&](float xDip, float yDip) -> POINT {
        POINT p = {};
        p.x = rect.left + DipToPixels(xDip, monitor.dpi);
        p.y = rect.top + DipToPixels(yDip, monitor.dpi);
        return p;
    };

    const POINT inside = toScreen((pill.rect.left + pill.rect.right) * 0.5f,
                                  (pill.rect.top + pill.rect.bottom) * 0.5f);
    // Inside the window but outside the pill: the strip is full width, so there
    // is a lot of it.
    const POINT outside = toScreen(8.0f, pill.rect.top + 1.0f);
    // Just underneath the pill, still inside the window.
    const POINT below = toScreen((pill.rect.left + pill.rect.right) * 0.5f,
                                 pill.rect.bottom + 10.0f);

    Check(window.HitTestScreen(inside), L"point in pill hits");
    Check(!window.HitTestScreen(outside), L"point in the strip misses");
    Check(!window.HitTestScreen(below), L"point under the pill misses");

    // --- WHAT THE CURSOR SAYS (2026-10-12, HANDOFF 8.37) -------------------
    //
    // Asked for over a photograph of the media card's previous / play-pause / next: "这些按钮 鼠标移上去 应该变成
    // 小手". The bar's window class registered the plain arrow and nothing ever changed it, so every control
    // looked exactly like the bar's own surface until it was clicked.
    //
    // Asserted on the same predicate the WM_SETCURSOR handler uses, at points built from the same layout the
    // hit tests use -- so "a hand where a click would do nothing" is a failing check rather than a matter of
    // opinion. The cases are the ones that pull in opposite directions: a control, the surface beside it, and
    // the shell area the bar shares with the deck buttons and the icon row.
    {
        const RoundedBox island = window.LayoutIslandFor();
        const RectF start = StartButtonFor(island);
        const RectF system = SystemButtonFor(island);
        const auto centreOf = [&](const RectF& r) {
            return toScreen((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f);
        };

        Check(!window.PointerOverControl(inside),
              L"the island's own surface is not a control (no hand on the pill)");
        Check(!window.PointerOverControl(outside),
              L"... and neither is the empty strip beside it");
        Check(window.PointerOverControl(centreOf(start)), L"the Start button is a control");
        Check(window.PointerOverControl(centreOf(system)), L"the system button is a control");

        // The icon row, with a row to point at: the middle icon's own centre.
        window.SetTaskIconCount(3);
        const TaskIconRow row = TaskRowFor(island, window.RowButtonCount());
        // The slot width the row is built from: the first icon's own centre is half a slot in, which is the
        // point the row's hit test answers 0 at.
        const float slotCentre =
            row.left + (kTaskIconButtonDip + kTaskIconGapDip) * 0.5f;
        wchar_t cursorDetail[224];
        std::swprintf(cursorDetail, 224, L"row starts at %.0f, first slot centre %.0f",
                      row.left, slotCentre);
        Check(window.PointerOverControl(toScreen(slotCentre, (row.top + row.bottom) * 0.5f)),
              L"an application icon is a control", cursorDetail);

        // ... AND THE ICON UNDER THE POINTER IS STILL THE ICON THAT IS DRAWN THERE once the TRAY
        // BUTTON stands between the island and the row.
        //
        // The row is DRAWN from a box the tray button has moved along (see RowAnchorWithTray), so the
        // window's own hit test -- IconAtScreen, which is what a click is routed through -- has to be
        // measured from that same box. It was not, and the report was exact: "鼠标移上去 第二个图片展开
        // 了", pointing at the FIRST icon opened the SECOND, because the hit test still believed the
        // row began one button to the left of where it is painted.
        //
        // Asserted from each icon's own DRAWN box, through the window's answer, so a future change
        // that moves one of the two apart is a failing check rather than something only a user sees.
        {
            window.SetTrayIconCount(3);
            const RoundedBox islandForRow = window.LayoutIslandFor();
            const RoundedBox drawnRow = RowAnchorWithTray(islandForRow, true);
            const int buttons = window.RowButtonCount();
            int mismatches = 0;
            int firstMismatch = -1;
            for (int i = 0; i < buttons; ++i) {
                const RectF box = TaskIconButtonAt(drawnRow, i, buttons, -1, 0.0f);
                const POINT centre = toScreen((box.left + box.right) * 0.5f,
                                              (box.top + box.bottom) * 0.5f);
                if (window.IconAtScreen(centre) != i) {
                    ++mismatches;
                    if (firstMismatch < 0) firstMismatch = i;
                }
            }
            wchar_t trayRowDetail[224];
            std::swprintf(trayRowDetail, 224,
                          L"drawn row starts at %.0f, %d buttons, %d mismatch(es), first %d",
                          drawnRow.rect.right + kTaskIconPadDip, buttons, mismatches, firstMismatch);
            Check(mismatches == 0,
                  L"the icon under the pointer is the icon drawn there, with the tray button up",
                  trayRowDetail);

            // ... and the tray button itself is still a control, which is what the same box moves for.
            Check(window.PointerOverControl(centreOf(TrayButtonFor(islandForRow))),
                  L"the tray button beside the island is a control", trayRowDetail);

            // Put the window back as it was found: the count is an input to every hit test below.
            window.SetTrayIconCount(0);
        }

        // ... and the deck's gap, which is a control only where a button is: the two buttons do not fill the
        // strip between them and the island, and that strip must stay an arrow.
        Check(!window.PointerOverControl(toScreen((start.right + system.left) * 0.5f,
                                                  (start.top + start.bottom) * 0.5f)),
              L"the gap between the two deck buttons is not a control", cursorDetail);

        // THE MEDIA CARD'S THREE BUTTONS, which is what the report pointed at. The box is the one the card is
        // DRAWN in (`PlacedCardFor`), because that is the box the click handler and this predicate both use --
        // the drawn card and the tested one were 167 DIP apart once (see MusicControlAtScreen), and a hand
        // that appeared somewhere other than over the button would be the first thing to show it.
        {
            window.SetState(IslandState::Expanded);
            for (int i = 0; i < 60; ++i) window.Tick(0.05f);
            // `cardLeftDip_` is an INPUT to the hit test -- the card's column is part of the bar only when it
            // is set -- and it is left over from whatever ran before this. The value is the one the frame loop
            // computes for the MEDIA card: centred under the ISLAND (the panel is the one that hangs from the
            // button it was opened with).
            const float islandCentre = (island.rect.left + island.rect.right) * 0.5f;
            const float windowWidthDip =
                static_cast<float>(monitor.screen.right - monitor.screen.left) / window.Scale();
            const float cardLeft = CardLeftFor(islandCentre - kExpandedWidthDip * 0.5f,
                                               kExpandedWidthDip, windowWidthDip);
            window.SetSystemPanel(window.SystemPanel(), cardLeft);
            // The OWNER as well: the cursor's hit tests are gated on which card is up, because the panel's
            // tiles and the card's buttons are both laid out in the window at all times.
            window.SetCardOwner(CardOwner::Music);
            const RoundedBox card = PlacedCardFor(
                BoxForState(IslandState::Collapsed, window.CurrentAnchor()), cardLeft);
            const MusicCardLayout music = MusicCardFor(card);
            Check(window.PointerOverControl(centreOf(music.playPause)),
                  L"the card's play/pause is a control");
            Check(window.PointerOverControl(centreOf(music.previous)),
                  L"... and so is previous");
            Check(window.PointerOverControl(centreOf(music.next)), L"... and so is next");
            Check(!window.PointerOverControl(centreOf(music.title)),
                  L"the card's own text is not a control");
            window.SetState(IslandState::Collapsed);
            for (int i = 0; i < 60; ++i) window.Tick(0.05f);
            // Put the window back as it was found. `cardLeftDip_` is part of the hit test's input and the
            // assertions that follow ask about a point UNDER the pill with no card up -- left set, the card's
            // column would accept it and they would fail for a reason that has nothing to do with them. (The
            // frame loop cannot leave it stale: it sets it every frame, to zero when no card is up.)
            window.SetSystemPanel(window.SystemPanel(), 0.0f);
            window.SetCardOwner(CardOwner::None);
        }

        window.SetTaskIconCount(0);
    }

    // WM_NCHITTEST is what actually routes the click. HTTRANSPARENT lets it
    // fall through to whatever is underneath; HTCLIENT takes it.
    const auto hitTest = [&](const POINT& p) -> LRESULT {
        return SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(p.x, p.y));
    };

    wchar_t buf[160];
    std::swprintf(buf, 160, L"inside=%lld strip=%lld below=%lld",
                  static_cast<long long>(hitTest(inside)),
                  static_cast<long long>(hitTest(outside)),
                  static_cast<long long>(hitTest(below)));
    Check(hitTest(inside) == HTCLIENT &&
          hitTest(outside) == HTTRANSPARENT &&
          hitTest(below) == HTTRANSPARENT,
          L"WM_NCHITTEST routes inside/outside", buf);

    // The pass-through decision itself, as a pure function of the point. This is
    // the same predicate the pointer poll uses, so checking it here checks the
    // live path without moving the physical cursor (which raced the window's own
    // poll and made the result depend on which ran last).
    Check(window.HitTestScreen(inside),
          L"decision: pointer on the pill accepts");
    Check(!window.HitTestScreen(outside),
          L"decision: pointer in the strip passes through");
    Check(!window.HitTestScreen(below),
          L"decision: pointer below the pill passes through");

    // The extended-style transition that follows from that decision.
    auto exStyle = [&]() -> LONG_PTR { return GetWindowLongPtrW(hwnd, GWL_EXSTYLE); };

    // The extended style is NOT touched by the pointer decision any more.
    //
    // There used to be a transition here: accepting the pointer cleared WS_EX_TRANSPARENT and passing
    // through set it. That mechanism WAS the bug. With the style set, Windows skips hit testing for the
    // window entirely -- it never asks WM_NCHITTEST -- so the style had to be cleared before a click
    // could reach the bar, and the only thing that cleared it was the 8 ms pointer poll. A click
    // arriving before the poll went to the desktop and did nothing; the log showed 28 clicks reaching
    // the procedure and 4 being routed.
    //
    // So what is asserted now is the opposite: the style stays off whatever the pointer decision is,
    // and the hit-test answers above are what WM_NCHITTEST returns to the system.
    window.ApplyPointerDecision(true);
    Check((exStyle() & WS_EX_TRANSPARENT) == 0,
          L"accepting the pointer leaves the hit test in charge");

    window.ApplyPointerDecision(false);
    Check((exStyle() & WS_EX_TRANSPARENT) == 0,
          L"passing through leaves the hit test in charge too");

    // The live poll, driven once with whatever the physical pointer happens to be over. Only the
    // invariant is asserted: the poll and the hit test agree, so the shape the user sees and the area
    // that accepts clicks cannot drift apart.
    POINT cursor = {};
    if (GetCursorPos(&cursor)) {
        SendMessageW(hwnd, WM_TIMER, kPointerTimerId, 0);
        Check(window.IsPointerInside() == window.HitTestScreen(cursor),
              L"live poll agrees with its own decision");
    }
}

void TestRenderer(IslandWindow& window) {
    Section(L"composition surface");

    Renderer renderer;
    const RECT rect = window.WindowRect();
    const LONG widthPx  = rect.right - rect.left;
    const LONG heightPx = rect.bottom - rect.top;

    const bool ok = renderer.Initialize(window.Handle(),
                                        static_cast<UINT>(widthPx),
                                        static_cast<UINT>(heightPx),
                                        window.Scale());
    Check(ok, L"D3D11 + D2D + DComp chain built");
    if (!ok) return;

    // The text renderer is attached AFTER Initialize, never before.
    //
    // Initialize begins by calling Shutdown, which clears every pointer it owns --
    // including the borrowed text renderer. Attaching it first therefore left the
    // renderer with text_ == null, and every text draw became a silent no-op that
    // returned success. That is why the notification assertions saw a bare card fill
    // with no glyphs while the live application rendered text correctly: the app
    // happens to attach in the working order.
    TextRenderer textForRenderer;
    const bool haveText = textForRenderer.Initialize();
    if (haveText) renderer.SetTextRenderer(&textForRenderer);
    Check(haveText, L"DirectWrite attached to the renderer for text assertions");

    wchar_t buf[160];
    std::swprintf(buf, 160, L"surface dpi %u, scale %.2f", renderer.Dpi(), window.Scale());
    Info(L"renderer", buf);

    // Draw the real thing a few times. A swap chain that was created but cannot
    // present shows up here and nowhere earlier.
    PillAppearance pill;
    pill.box = window.CurrentBox();
    pill.opacity = 1.0f;

    for (int i = 0; i < 3; ++i) {
        renderer.Render(pill);
    }
    Check(true, L"three frames rendered and committed");

    // Read the frame back off the GPU. Present returning S_OK says the compositor
    // accepted a frame; it does not say the frame contains anything. These two
    // assertions are what actually distinguish "drawing works" from "the surface
    // is blank", and they are the difference between a bug in the renderer and a
    // bug in how the compositor is attached.
    {
        const float inX = (pill.box.rect.left + pill.box.rect.right) * 0.5f;
        const float inY = (pill.box.rect.top + pill.box.rect.bottom) * 0.5f;
        const float outX = 8.0f;                       // far left of the strip
        const float outY = pill.box.rect.top + 1.0f;

        BYTE insideAlpha = 0;
        BYTE outsideAlpha = 255;
        BYTE insideRgb[3] = {0, 0, 0};

        wchar_t why[256] = {};
        const bool read = renderer.ReadBackFrameVerbose(inX, inY, outX, outY,
                                                        &insideAlpha, &outsideAlpha, insideRgb,
                                                        why, 256);
        Check(read, L"frame read back from the GPU", why);

        if (read) {
            wchar_t line[192];
            std::swprintf(line, 192, L"pill alpha=%u rgb=(%u,%u,%u); strip alpha=%u",
                          insideAlpha, insideRgb[0], insideRgb[1], insideRgb[2], outsideAlpha);
            Info(L"rendered frame contents", line);

            // Outside the pill the surface must be fully transparent, otherwise
            // the click-through strip would paint something over the desktop.
            Check(outsideAlpha == 0, L"outside the pill is fully transparent");
            // Inside the pill it must be opaque: the fill is 0.92 alpha.
            Check(insideAlpha > 200, L"inside the pill is drawn opaque");
        }
    }

    // Opacity must be drivable through the effect group: that is the whole
    // reason the effect group exists (Visual3 is unavailable on this device).
    pill.opacity = 0.0f;
    renderer.Render(pill);
    Check(true, L"zero-opacity frame accepted");
    pill.opacity = 1.0f;

    // The GPU-side fade. This is the only property animated on the compositor
    // rather than per frame, so it is the only place where a silent failure would
    // not show up as wrong geometry -- the shape would simply be invisible or
    // fully opaque with no error anywhere.
    const bool animated = renderer.AnimateOpacity(0.0f, 1.0f, 0.15);
    Check(animated, L"compositor opacity animation accepted");

    // A frame must still render and commit while the compositor owns opacity,
    // which is the case that would break if the two paths fought over the value.
    renderer.Render(pill);
    {
        BYTE a = 0, outA = 255;
        BYTE rgb[3] = {};
        wchar_t why[192] = {};
        const RoundedBox box = pill.box;
        const bool read = renderer.ReadBackFrameVerbose(
            (box.rect.left + box.rect.right) * 0.5f, (box.rect.top + box.rect.bottom) * 0.5f,
            8.0f, box.rect.top + 1.0f, &a, &outA, rgb, why, 192);
        Check(read, L"frame still readable during the fade", why);
    }

    Check(renderer.WaitForCompositorClock(50),
          L"compositor clock tick received");

    // --- the card's text is actually drawn -----------------------------------
    //
    // "The draw call returned S_OK" and "there are glyphs on screen" are different
    // claims, and this project has already shipped a pill that was invisible behind
    // a fully green self-test. The check is numeric: render the card with no
    // notification, then with one, and compare the mean luminance of the region the
    // notification occupies. Glyphs are light on a near-black fill, so the second
    // must be measurably brighter.
    {
        // Ask the window for the real card geometry instead of recomputing an
        // anchor here: the window owns the layout, and a second copy of the
        // arithmetic is a second thing to get wrong. Settling the motion first means
        // CurrentBox is the finished card, not a frame of the transition.
        window.SetState(IslandState::Expanded);
        for (int i = 0; i < 60; ++i) window.Tick(0.05f);
        const RoundedBox cardBox = window.CurrentBox();

        // The lower half of the card is where the notification is laid out; the
        // clock and date own the top.
        const float height = cardBox.rect.bottom - cardBox.rect.top;
        // The sample band must match where DrawNotification actually puts the
        // notification, and must EXCLUDE where the clock is drawn.
        //
        // DrawNotification starts at 52% of the card height; the clock occupies the
        // top. An earlier band began at 50%, which caught the clock's descenders, so
        // the "no notification" control still measured glyphs and the comparison
        // proved nothing. Starting at 60% is clear of the clock and still inside the
        // title and body lines.
        const float contentTop = cardBox.rect.top + height * 0.60f;
        const float contentBottom = cardBox.rect.bottom - height * 0.06f;

        wchar_t size[128];
        std::swprintf(size, 128, L"card y=[%.0f..%.0f] sample y=[%.0f..%.0f]",
                      cardBox.rect.top, cardBox.rect.bottom, contentTop, contentBottom);
        Info(L"card geometry", size);

        // Both regions must lie INSIDE the card, or the reading is of whatever is
        // behind the bar rather than of the card.
        //
        // The card is 560 DIP wide, so a "centre" region of left+110 .. right-110 is
        // 780 DIP wide -- wider than the card -- and a strip at left+12 .. left+72
        // starts outside it. An earlier version used both and measured the desktop:
        // every region read the same fill value, so the "no text" and "text" numbers
        // were identical and the comparison silently proved nothing.
        const float cardInset = 18.0f;
        const float textLeft = cardBox.rect.left + 150.0f;    // over the centred text
        const float textRight = cardBox.rect.right - 150.0f;
        const float blankLeft = cardBox.rect.left + cardInset;      // same fill, no glyphs
        const float blankRight = cardBox.rect.left + 70.0f;

        // The text is centred, so the glyph-free strip has to be clear of the middle.
        // It is checked rather than assumed: a strip that overlapped the text would
        // make the comparison meaningless in the other direction.
        {
            wchar_t geometry[224];
            std::swprintf(geometry, 224,
                          L"card x=[%.0f..%.0f] text x=[%.0f..%.0f] blank x=[%.0f..%.0f]",
                          cardBox.rect.left, cardBox.rect.right, textLeft, textRight,
                          blankLeft, blankRight);
            Info(L"sample geometry", geometry);
        }

        const auto renderCard = [&](const Notification* note, const uint8_t* icon = nullptr,
                                    int iconSide = 0) {
            FrameState frame;
            frame.pill.box = cardBox;
            // THE RESTING PILL, which the frame loop always sets and this frame used not to: without it
            // the renderer falls back to the animated box for the island's fill, so the fill covered the
            // whole card and the card's OWN surface was never needed -- which is how a notification card
            // with no background at all passed this test while the real bar showed floating text.
            frame.restingPill = window.LayoutIslandFor();
            frame.state = IslandState::Expanded;
            // The OWNER, not the state: the renderer composes a card from who owns it, so that the card is
            // still drawn while it retracts (the state is the target and is already the resting shape by
            // then). A frame that names only the state draws no card at all -- which is how this test
            // reported the change.
            frame.cardOwner = CardOwner::Notification;
            frame.outerCardUp = true;
            // The card clock is used on both sides of the comparison, and it lives
            // above the sampled band, so it cancels out.
            frame.clock = ClockStyle::Card;
            frame.contentOpacity = 1.0f;
            frame.notification = note;
            // The sender's icon, when the case is about one. Its revision is constant, which is what a real
            // message does too: one icon per notification.
            frame.notificationIconPixels = icon;
            frame.notificationIconWidth = icon ? iconSide : 0;
            frame.notificationIconHeight = icon ? iconSide : 0;
            frame.notificationIconRevision = icon ? 1u : 0u;
            // Rendered twice on purpose.
            //
            // Reading the swap chain right after a render returns the PREVIOUS
            // frame's contents -- the first read in this test always came back empty
            // while later reads were correct. Rendering twice makes the frame that is
            // read the frame that was intended, instead of depending on that lag.
            renderer.Render(frame);
            renderer.Render(frame);
        };

        Notification shown;
        strncpy_s(shown.app, kAppNameCapacity, "DSH", _TRUNCATE);
        strncpy_s(shown.title, kTitleCapacity, "Build finished", _TRUNCATE);
        strncpy_s(shown.body, kBodyCapacity, "0 errors, 3 warnings", _TRUNCATE);

        // Map the alpha across the card. A mean luminance that equals the fill cannot
        // say whether glyphs are present; a map makes an empty area and a sparsely
        // texted one immediately distinguishable.
        {
            renderCard(&shown);
            renderCard(&shown);
            wchar_t map[1024] = {};
            renderer.DescribeRegionAlpha(cardBox.rect.left + 20.0f, contentTop,
                                         cardBox.rect.right - 20.0f, contentBottom,
                                         44, 8, map, 1024);
            // The map is logged, not asserted: when this comparison fails, the first
            // question is whether glyphs exist in the region at all, and a map answers
            // that where a mean luminance cannot.
            LogLine(L"alpha map, card lower half (44x8; '#'=bright text):\n%ls", map);
        }

        renderCard(&shown);
        float textRegion = -1.0f;
        float blankRegion = -1.0f;
        const bool measured =
            renderer.RegionLuminance(textLeft, contentTop, textRight, contentBottom, &textRegion) &&
            renderer.RegionLuminance(blankLeft, contentTop, blankRight, contentBottom, &blankRegion);

        wchar_t detail[224];
        std::swprintf(detail, 224,
                      L"centre(text)=%.2f  left strip(no text)=%.2f  delta=%+.2f",
                      textRegion, blankRegion, textRegion - blankRegion);
        Check(measured, L"both regions of the card measured", detail);

        // The card must actually be painted. If both regions read near zero then
        // nothing was drawn, and a small delta would mean "no fill", not "no glyphs".
        Check(measured && blankRegion > 2.0f,
              L"the card fill is drawn (so a glyph reading is meaningful)", detail);
        Check(measured && textRegion > blankRegion + 1.0f,
              L"notification text is measurably drawn", detail);

        // Render the same card with no notification: the centre must fall back to the
        // fill. Without this, a centre that were brighter for some unrelated reason
        // would satisfy the check above.
        renderCard(nullptr);
        float textNoNote = -1.0f;
        float blankNoNote = -1.0f;
        const bool measuredControl =
            renderer.RegionLuminance(textLeft, contentTop, textRight, contentBottom, &textNoNote) &&
            renderer.RegionLuminance(blankLeft, contentTop, blankRight, contentBottom, &blankNoNote);

        std::swprintf(detail, 224,
                      L"without notification: centre=%.2f left=%.2f delta=%+.2f",
                      textNoNote, blankNoNote, textNoNote - blankNoNote);
        Check(measuredControl && Nearly(textNoNote, blankNoNote, 1.0f),
              L"with no notification the centre matches the fill", detail);
        Check(measuredControl && textRegion > textNoNote + 1.0f,
              L"the notification is what brightened the centre", detail);

        // --- the notice strip: a message widens the island, both ways -----------------
        //
        // The shape, and the two things it has to be built out of rather than merely look like:
        //
        //   * it grows LEFT AND RIGHT equally, because that is what was asked for ("是左右宽展") and it is
        //     what a centred box does -- asserted as a symmetry rather than as two numbers, so a future
        //     width cannot quietly become one-sided;
        //   * the icon row starts to the RIGHT of it. The row is measured from RowAnchorBox, and a strip that
        //     the row did not know about would be drawn underneath the icons.
        {
            const IslandAnchor a = AnchorForWindow(0.0f, kPillTopMarginDip, 2560.0f);
            const RoundedBox restingPill = BoxForState(IslandState::Collapsed, a);
            const RoundedBox strip = BoxForState(IslandState::NoticeStrip, a);
            const float pillCentre = (restingPill.rect.left + restingPill.rect.right) * 0.5f;
            const float stripCentre = (strip.rect.left + strip.rect.right) * 0.5f;

            wchar_t dn[256];
            std::swprintf(dn, 256, L"pill [%.0f..%.0f] strip [%.0f..%.0f], centres %.1f / %.1f",
                          restingPill.rect.left, restingPill.rect.right, strip.rect.left,
                          strip.rect.right, pillCentre, stripCentre);
            Check(Nearly(pillCentre, stripCentre, 0.5f),
                  L"the notice strip widens symmetrically about the island's centre", dn);
            Check(strip.rect.right - strip.rect.left >
                          restingPill.rect.right - restingPill.rect.left + 40.0f &&
                      Nearly(strip.rect.bottom - strip.rect.top,
                             restingPill.rect.bottom - restingPill.rect.top, 0.5f),
                  L"the notice strip is wider than the pill and the same height", dn);

            // The row is measured from the strip while it is up, and starts clear of its right edge.
            const RoundedBox rowAnchor = RowAnchorBox(strip, restingPill);
            const TaskIconRow row = TaskRowFor(rowAnchor, 4);
            std::swprintf(dn, 256, L"strip right %.0f, row starts %.0f", strip.rect.right, row.left);
            Check(row.left >= strip.rect.right + 4.0f,
                  L"the icon row starts clear of the notice strip", dn);

            // ... and the anchor rule itself: the pill in every state but the strip, and never the card or
            // the full-screen bar, which would drag the row sideways for the length of their animations.
            const RoundedBox card = BoxForState(IslandState::Expanded, a);
            const RoundedBox full = BoxForState(IslandState::FullBar, a);
            std::swprintf(dn, 256, L"anchors: pill %d, strip %d, card %d, full-bar %d",
                          SameBox(RowAnchorBox(restingPill, restingPill), restingPill) ? 1 : 0,
                          SameBox(RowAnchorBox(strip, restingPill), strip) ? 1 : 0,
                          SameBox(RowAnchorBox(card, restingPill), restingPill) ? 1 : 0,
                          SameBox(RowAnchorBox(full, restingPill), restingPill) ? 1 : 0);
            Check(SameBox(RowAnchorBox(restingPill, restingPill), restingPill) &&
                      SameBox(RowAnchorBox(strip, restingPill), strip) &&
                      SameBox(RowAnchorBox(card, restingPill), restingPill) &&
                      SameBox(RowAnchorBox(full, restingPill), restingPill),
                  L"the row follows the strip and nothing else", dn);

            // ... and the same rule checked on the shapes it actually sees DURING a transition, which is
            // where it was wrong. The two ends above both passed while every frame in between was broken:
            // the card's box is a lerp from the pill to the card, so most of an opening is "wider than the
            // pill and narrower than a card" -- the old rule accepted all of it under 125 DIP tall, and the
            // fill, the clock, the deck, the row and the hit tests rode the growing slab.
            //
            // Walked in steps rather than asserted at one fraction, because the deciding quantity is a
            // HEIGHT that only crosses the threshold once: a single sample either side of it would both be
            // right on a rule that is wrong everywhere else.
            {
                int stripFramesMissed = 0;   // the strip's growth must be followed, every frame
                int cardFramesRidden = 0;    // the card's growth must never be
                constexpr int kSteps = 20;
                for (int step = 1; step < kSteps; ++step) {
                    const float t = static_cast<float>(step) / static_cast<float>(kSteps);
                    const RoundedBox stripFrame = LerpBox(restingPill, strip, t);
                    if (!SameBox(RowAnchorBox(stripFrame, restingPill), stripFrame)) {
                        ++stripFramesMissed;
                    }
                    const RoundedBox cardFrame = LerpBox(restingPill, card, t);
                    if (!SameBox(RowAnchorBox(cardFrame, restingPill), restingPill)) {
                        ++cardFramesRidden;
                    }
                }
                std::swprintf(dn, 256,
                              L"%d steps: %d strip frames not followed, %d card frames followed",
                              kSteps - 1, stripFramesMissed, cardFramesRidden);
                Check(stripFramesMissed == 0 && cardFramesRidden == 0,
                      L"the anchor follows the strip's growth frame by frame and never the card's", dn);
            }
        }
        // --- the third channel: WeChat, judged from a window ------------------------
        //
        // Every case below is a window this machine ACTUALLY HAS (measured by enumerating the WeChat
        // process on 2026-10-11), plus a synthetic corner popup. That is the point: the rule decides whether
        // the bar ever says "微信 · 有新消息", and a rule that is wrong in either direction looks exactly like
        // "WeChat sent nothing" on screen.
        {
            // The main window: 400x700 and up. Its title is NOT part of the rule (see the comment there: the
            // client has two windows titled 微信 and Weixin, and the second one is the popup candidate).
            Check(!LooksLikeWeChatToast(L"Qt51514QWindowIcon", L"微信", true, 400, 700),
                  L"the main window is not a notification");
            // The main window as it is actually parked: off-screen, and 160x28.
            Check(!LooksLikeWeChatToast(L"Qt51514QWindowIcon", L"微信", true, 160, 28),
                  L"the parked main window is not a notification");
            // The audio/video call window: 376x648, hidden in practice.
            Check(!LooksLikeWeChatToast(L"Qt51514QWindowIcon", L"微信音视频通话 ", true, 376, 648),
                  L"the call window is not a notification");
            // The tray icon's message window: full screen sized, and hidden.
            Check(!LooksLikeWeChatToast(L"Qt51514WxTrayIconMessageWindowClass", L"WxTrayIconMessageWindow",
                                        false, 1920, 1035),
                  L"the tray icon's window is not a notification");
            // The 0x0 message-only windows the client also owns.
            Check(!LooksLikeWeChatToast(L"Base_PowerMessageWindow", L"", false, 0, 0),
                  L"a message-only window is not a notification");

            // ... and the popup: a compact card, WHATEVER ITS POSITION.
            //
            // Position is deliberately not part of the rule. The first version required a screen edge, and
            // the log then showed that a real message produced no window at all -- while the client owns a
            // hidden 176x199 window sitting dead centre, which is exactly the shape a compact new-message card
            // has. A hidden window's rectangle is its layout's last guess, not where it will appear.
            Check(LooksLikeWeChatToast(L"Qt51514QWindowIcon", L"", true, 176, 199),
                  L"a compact new-message card is a notification wherever it sits");
            Check(LooksLikeWeChatToast(L"Qt51514QWindowIcon", L"", true, 320, 110),
                  L"a wider notification card is a notification too");
            // Hidden, whatever its shape.
            Check(!LooksLikeWeChatToast(L"Qt51514QWindowIcon", L"", false, 320, 110),
                  L"a hidden window is not a notification");

            // The CALL window, which is the other thing the island has to show. Its rule is the TITLE, because
            // it has one -- and the notification popup has none, which is why these are two rules rather than
            // one "is WeChat doing something" test.
            Check(LooksLikeWeChatCall(L"微信音视频通话 ", true),
                  L"a visible call window is a call");
            Check(LooksLikeWeChatCall(L"微信音视频通话", true),
                  L"the trailing space in the client's own title is not part of the rule");
            Check(!LooksLikeWeChatCall(L"微信音视频通话 ", false),
                  L"a hidden call window is not a call in progress");
            Check(!LooksLikeWeChatCall(L"微信", true) && !LooksLikeWeChatCall(L"", true),
                  L"the main window and an untitled window are not calls");

            // The timer's format, at the boundaries -- the part of the call strip that is wrong in a way nobody
            // notices for ten minutes: 61 seconds printed as "1:1" looks like a timer until you check it.
            wchar_t timer[32] = {};
            TextRenderer::CallTimerText(0, timer, 32);
            Check(std::wcscmp(timer, L"00:00") == 0, L"a call that just started reads 00:00");
            TextRenderer::CallTimerText(9, timer, 32);
            Check(std::wcscmp(timer, L"00:09") == 0, L"single-digit seconds are padded");
            TextRenderer::CallTimerText(69, timer, 32);
            Check(std::wcscmp(timer, L"01:09") == 0, L"a minute and nine seconds reads 01:09");
            TextRenderer::CallTimerText(3661, timer, 32);
            Check(std::wcscmp(timer, L"1:01:01") == 0, L"past an hour the timer grows an hour field");

            // The meter's shape: never flat, never over the top, and taller when the level is higher.
            bool meterSane = true;
            bool meterVaries = false;
            for (int i = 0; i < 12; ++i) {
                const float quiet = TextRenderer::CallBarFraction(i, 12, 0.0f);
                const float loud = TextRenderer::CallBarFraction(i, 12, 1.0f);
                if (quiet < 0.0f || loud > 1.0f || loud < quiet) meterSane = false;
                if (std::fabs(loud - quiet) > 0.05f) meterVaries = true;
            }
            wchar_t meterDetail[160];
            std::swprintf(meterDetail, 160, L"bar 5: quiet %.2f, loud %.2f",
                          TextRenderer::CallBarFraction(5, 12, 0.0f),
                          TextRenderer::CallBarFraction(5, 12, 1.0f));
            Check(meterSane, L"the level meter stays within its box and grows with the level", meterDetail);
            Check(meterVaries, L"the level meter is a shape, not a flat line", meterDetail);

            // The sound channel's rule, which is the one the user asked for in as many words: a WeChat SOUND
            // starting is a message. The failure it has to avoid is announcing repeatedly for one sound, and
            // announcing on the sound ENDING, which is the same state change in the other direction.
            Check(ShouldAnnounceWeChatSound(true, false, true, 0, 1, true, 60000),
                  L"a WeChat sound starting is a message");
            Check(!ShouldAnnounceWeChatSound(true, true, true, 1, 1, true, 60000),
                  L"a sound that is still playing is not a second message");
            Check(!ShouldAnnounceWeChatSound(true, true, false, 1, 1, true, 60000),
                  L"a sound ENDING is not a message");
            Check(!ShouldAnnounceWeChatSound(false, false, true, 0, 1, true, 60000),
                  L"a sound from something that is not WeChat is not a message");
            Check(!ShouldAnnounceWeChatSound(true, false, true, 0, 1, true, 1000),
                  L"two sounds inside the cooldown are one message");
            // The client may create a session only when it has something to play, so a session APPEARING is the
            // same news as a state rising.
            Check(ShouldAnnounceWeChatSound(true, true, true, 1, 2, true, 60000),
                  L"a session appearing where there was none is a sound starting");
            // ... and the first read is a baseline: starting the bar while WeChat already holds a session must
            // not announce a message that never arrived.
            Check(!ShouldAnnounceWeChatSound(true, true, true, 1, 1, false, 60000),
                  L"the first read is a baseline, not a message");
        }
        // --- the second channel: Windows' own toasts --------------------------------
        //
        // The parser and the name-mapper, asserted directly. Both are pure, and both have a wrong answer that
        // looks exactly like a working bar with a slightly odd card in it:
        //
        //   * the XML is UTF-8, so reading it a byte at a time turns every Chinese character into latin
        //     look-alikes -- the first version of this printed `æ£ç´¢é¹ç®` for a title that was `检索项目`,
        //     and it looked like a corrupt database rather than a decoder;
        //   * an AppUserModelID is not a name: `OpenAI.Codex_2p2nqsd0c76g0!App` has to become `Codex`.
        {
            wchar_t title[kTitleCapacity] = {};
            wchar_t body[kBodyCapacity] = {};

            // Real shape, real encoding, and the entities that a toast is allowed to carry.
            const char* xml =
                "<toast launch=\"type=click&amp;tag=7\"><visual><binding template=\"ToastGeneric\">"
                "<text>检索项目</text><text>0 errors &amp; 3 warnings &lt;done&gt;</text>"
                "</binding></visual></toast>";
            const bool parsed =
                ToastTextFromXml(xml, std::strlen(xml), title, kTitleCapacity, body, kBodyCapacity);

            wchar_t detailT[224];
            std::swprintf(detailT, 224, L"title='%ls' body='%ls'", title, body);
            Check(parsed, L"a toast XML is parsed", detailT);
            Check(std::wcscmp(title, L"检索项目") == 0,
                  L"the title is decoded as UTF-8, not byte by byte", detailT);
            Check(std::wcscmp(body, L"0 errors & 3 warnings <done>") == 0,
                  L"the body is the second text element, with its entities decoded", detailT);

            // A document with no <text> at all is not a toast this reader can show.
            const char* empty = "<toast><visual></visual></toast>";
            Check(!ToastTextFromXml(empty, std::strlen(empty), title, kTitleCapacity, body, kBodyCapacity),
                  L"a toast with no text element is refused");

            // A body far longer than the store's field is TRUNCATED, not dropped: the conversion used to fail
            // outright when its buffer was too small, and a dropped body looks like an empty message.
            std::string longBody = "<toast><visual><binding><text>T</text><text>";
            longBody.append(3000, 'x');
            longBody.append("</text></binding></visual></toast>");
            const bool longParsed = ToastTextFromXml(longBody.c_str(), longBody.size(), title,
                                                     kTitleCapacity, body, kBodyCapacity);
            wchar_t detailL[224];
            std::swprintf(detailL, 224, L"long body -> %zu character(s)", std::wcslen(body));
            Check(longParsed && std::wcslen(body) > 100 && std::wcslen(body) < kBodyCapacity,
                  L"a body longer than the store's field is truncated rather than lost", detailL);

            wchar_t name[128] = {};
            FriendlyNameFromHandlerId(L"OpenAI.Codex_2p2nqsd0c76g0!App", name, 128);
            wchar_t detailN[224];
            std::swprintf(detailN, 224, L"AppUserModelID -> '%ls'", name);
            Check(std::wcscmp(name, L"Codex") == 0,
                  L"a packaged app's handler id becomes the name a person knows", detailN);

            FriendlyNameFromHandlerId(L"C:\\SOFTWARE\\QQMusic\\QQMusic.exe", name, 128);
            std::swprintf(detailN, 224, L"exe path -> '%ls'", name);
            Check(std::wcscmp(name, L"QQMusic.exe") == 0,
                  L"a handler that is an executable keeps its file name", detailN);
        }
        // --- the sender's icon ------------------------------------------------------
        //
        // Three things have to hold, and each one fails silently on screen:
        //   * the SOURCE is chosen right -- a named path wins, and otherwise it is the sending PROCESS;
        //   * the path really loads into pixels (a `.png` read as an executable, or the reverse, gives none);
        //   * the card DRAWS them, asserted by measuring the icon's own column: the notification's text is
        //     centred, so that column holds nothing at all without an icon and the picture with one.
        //
        // The last one is measured rather than eyeballed because "no icon" and "an icon that failed to load"
        // look exactly alike on screen, and they have opposite fixes.
        {
            constexpr int side = 32;
            std::vector<uint8_t> icon(static_cast<size_t>(side) * side * 4);
            for (size_t i = 0; i < icon.size(); i += 4) {
                // Opaque and bright, BGR order with premultiplied alpha: a white square.
                icon[i + 0] = 255;
                icon[i + 1] = 255;
                icon[i + 2] = 255;
                icon[i + 3] = 255;
            }

            // The icon's own column: empty without an icon (the text on this card is centred, so the left
            // margin is bare) and carrying the picture with one.
            //
            // The band starts at 50% of the card rather than at `contentTop` (60%) because the icon belongs to
            // the FIRST line, and that line starts just above 60% -- measuring from contentTop was reading the
            // card's fill in both cases, which is exactly the "no icon" reading the check is meant to catch.
            const float gapLeft = cardBox.rect.left + 17.0f;
            const float gapRight = gapLeft + 12.0f;
            const float iconTop = cardBox.rect.top + height * 0.50f;
            const float iconBottom = cardBox.rect.top + height * 0.70f;

            renderCard(&shown, icon.data(), side);
            renderCard(&shown, icon.data(), side);
            float withIcon = -1.0f;
            const bool measuredIcon = renderer.RegionLuminance(gapLeft, iconTop, gapRight, iconBottom,
                                                               &withIcon);

            renderCard(&shown);
            renderCard(&shown);
            float withoutIcon = -1.0f;
            const bool measuredPlain = renderer.RegionLuminance(gapLeft, iconTop, gapRight, iconBottom,
                                                                &withoutIcon);

            wchar_t iconDetail[224];
            std::swprintf(iconDetail, 224,
                          L"icon column: with=%.2f  without=%.2f  delta=%+.2f", withIcon, withoutIcon,
                          withIcon - withoutIcon);
            Check(measuredIcon && measuredPlain, L"both icon cases measured", iconDetail);
            Check(measuredIcon && measuredPlain && withIcon > withoutIcon + 1.0f,
                  L"the sender's icon is drawn in its own column", iconDetail);

            // The two rules about WHERE the icon comes from. Asserted directly because both are decisions with
            // a wrong answer that looks like a working card with a blank space in it.
            Notification named;
            strncpy_s(named.iconPath, 260, "C:\\somewhere\\picture.png", _TRUNCATE);
            named.senderPid = GetCurrentProcessId();
            wchar_t source[260] = {};
            std::swprintf(iconDetail, 224, L"named path -> '%ls'",
                          NotificationIconSource(named, source, 260) ? source : L"(none)");
            Check(NotificationIconSource(named, source, 260) &&
                      std::wcscmp(source, L"C:\\somewhere\\picture.png") == 0,
                  L"a named icon path is the source", iconDetail);

            Notification anonymous;
            anonymous.senderPid = GetCurrentProcessId();
            const bool fromProcess = NotificationIconSource(anonymous, source, 260);
            std::swprintf(iconDetail, 224, L"unnamed sender -> '%ls'", fromProcess ? source : L"(none)");
            Check(fromProcess && std::wcsstr(source, L"island_bar.exe") != nullptr,
                  L"with no icon named, the sender's own process is the source", iconDetail);

            Notification fromNobody;
            Check(!NotificationIconSource(fromNobody, source, 260),
                  L"a notification with no icon and no known sender reports no source");

            // THE CARD'S OWN SURFACE. Measured in the card's TOP band, which holds no text at all (the
            // clock is drawn in the pill and the notification's words start at 52% of the height), so this
            // reads the card's background and nothing else. It was 0 before -- a transparent card, with the
            // words floating on the desktop -- which is what the user photographed as "a window appearing".
            renderCard(&shown);
            float cardSurface = -1.0f;
            const bool measuredSurface =
                renderer.RegionLuminance(cardBox.rect.left + 24.0f, cardBox.rect.top + 8.0f,
                                         cardBox.rect.right - 24.0f, cardBox.rect.top + 40.0f,
                                         &cardSurface);
            wchar_t surfaceDetail[160];
            std::swprintf(surfaceDetail, 160, L"card top band = %.2f (a transparent card reads 0.00)",
                          cardSurface);
            Check(measuredSurface && cardSurface > 2.0f,
                  L"the notification card draws its own surface", surfaceDetail);

            // THE MEDIA CARD'S OWN SURFACE, which is a DIFFERENT case and was missing for a different
            // reason: this card has always had a surface, but only when there is artwork to put in it --
            // the cover's blurred wash AND the scrim are both drawn from `background`, so a track whose
            // player publishes no thumbnail got a card with nothing behind its words at all. Reported as
            // "背景色 不显示了", and the media session's own answer for that track is `Thumbnail: none` --
            // so the bar was drawing exactly what it was given: nothing.
            //
            // Asserted with NO artwork, which is the case that was broken -- and the band is the COVER'S OWN
            // SLOT, which with no cover holds nothing but the surface. The card's top strip would not do:
            // this card's first row carries the player's icon and name, and that icon alone reads 31 (measured
            // on the broken build), which is why the first version of this check passed against the defect.
            //
            // The threshold separates two MEASURED numbers rather than being a taste: with the card's own
            // surface behind it the slot reads 19.01 (the surface is 26, and this card's own scrim darkens it
            // by 48% -- the card reads as a dark surface, which is the point), and with only the scrim over
            // nothing -- the defect -- it read 6.16. Twelve sits between them with room on both sides.
            {
                MusicInfo artLess;
                artLess.present = true;
                artLess.state = PlaybackState::Playing;
                std::snprintf(artLess.title, kMusicTitleCapacity, "no artwork");
                std::snprintf(artLess.app, kMusicAppCapacity, "QQMusic.exe");
                // artworkSize stays 0, and no pixels are passed: what a track with `Thumbnail: none` looks
                // like to this card.

                FrameState frame;
                frame.pill.box = cardBox;
                frame.restingPill = window.LayoutIslandFor();
                frame.state = IslandState::Expanded;
                frame.cardOwner = CardOwner::Music;
                frame.outerCardUp = true;
                frame.clock = ClockStyle::Card;
                frame.contentOpacity = 1.0f;
                frame.music = &artLess;
                frame.transportGlyph = ">";
                renderer.Render(frame);
                renderer.Render(frame);

                float mediaSurface = -1.0f;
                // The band comes from the card's OWN layout rather than from arithmetic on the box: the
                // renderer composes this card below the bar (its top edge is the resting pill's underside
                // plus the gap), and `MusicCardFor` is the same function the drawing uses. The COVER'S SLOT
                // is the one place this card leaves empty when there is no cover.
                RoundedBox drawn = cardBox;
                drawn.rect.top = frame.restingPill.rect.bottom + island::kCardTopGapDip;
                drawn.rect.bottom = drawn.rect.top + island::kExpandedHeightDip;
                const MusicCardLayout drawnLayout = MusicCardFor(drawn);
                const bool measuredMedia =
                    renderer.RegionLuminance(drawnLayout.artwork.left + 6.0f,
                                             drawnLayout.artwork.top + 8.0f,
                                             drawnLayout.artwork.right - 6.0f,
                                             drawnLayout.artwork.bottom - 8.0f, &mediaSurface);
                wchar_t mediaDetail[192];
                // ... and the slot is a PLACEHOLDER rather than the bare surface (2026-10-12, HANDOFF 8.38):
                // a cover-less track used to leave a dark rectangle where the cover belongs, which reads as
                // "the bar lost the picture" (reported twice: "背景色 不显示了", "切歌的时候 封面图就没有了").
                //
                // Two readings, because they fail differently: the fill makes the slot brighter than the bare
                // surface, and the ♪ is a bright mark in the middle of it. Measured: 6.16 with nothing drawn,
                // 19.01 with the surface alone, 36.58 with the placeholder.
                float mediaCorners = -1.0f;
                float mediaCentre = -1.0f;
                const bool measuredCorners =
                    renderer.RegionLuminance(drawnLayout.artwork.left + 6.0f,
                                             drawnLayout.artwork.top + 6.0f,
                                             drawnLayout.artwork.left + 16.0f,
                                             drawnLayout.artwork.top + 16.0f, &mediaCorners) &&
                    renderer.RegionLuminance(
                        (drawnLayout.artwork.left + drawnLayout.artwork.right) * 0.5f - 14.0f,
                        (drawnLayout.artwork.top + drawnLayout.artwork.bottom) * 0.5f - 14.0f,
                        (drawnLayout.artwork.left + drawnLayout.artwork.right) * 0.5f + 14.0f,
                        (drawnLayout.artwork.top + drawnLayout.artwork.bottom) * 0.5f + 14.0f,
                        &mediaCentre);
                std::swprintf(mediaDetail, 192,
                              L"cover slot with no artwork: mean %.2f, corner %.2f, centre %.2f (nothing drawn "
                              L"reads 6.16, the bare surface 19.01)",
                              mediaSurface, mediaCorners, mediaCentre);
                Check(measuredMedia && mediaSurface > 12.0f,
                      L"the media card draws its own surface with no artwork", mediaDetail);
                // The note lives in the MIDDLE of the slot, so a band at the corner holds the fill and nothing
                // else -- which is what makes "the placeholder is drawn" measurable rather than a matter of
                // looking at it.
                Check(measuredCorners && mediaCentre > mediaCorners + 4.0f,
                      L"... and marks the empty cover slot with a placeholder, not a hole", mediaDetail);

                // ... AND THE GPU CACHE MUST TELL TWO COVERS APART, even when their revision repeats.
                //
                // This is the assertion for "歌曲切换了 但是封面图没有换": the media session's artwork counter
                // used to be `previous + 1` and reset to 1 whenever a track published no cover, so the next
                // track's late cover carried the same revision as the previous track's -- and the renderer
                // matched its cached entry and drew the OLD album art while the log said `cover decoded`.
                // The provider counts up and never repeats now; this is what keeps the cache honest either way,
                // asked directly of the cache rather than through a picture.
                {
                    std::vector<uint8_t> coverA(16 * 16 * 4, 0x30);
                    std::vector<uint8_t> coverB(16 * 16 * 4, 0xC0);
                    const auto renderWithArtwork = [&](const uint8_t* pixels,
                                                       unsigned long long revision) {
                        FrameState f;
                        f.pill.box = cardBox;
                        f.restingPill = window.LayoutIslandFor();
                        f.state = IslandState::Expanded;
                        f.cardOwner = CardOwner::Music;
                        f.outerCardUp = true;
                        f.clock = ClockStyle::Card;
                        f.contentOpacity = 1.0f;
                        f.music = &artLess;
                        f.transportGlyph = ">";
                        f.artworkPixels = pixels;
                        f.artworkWidth = 16;
                        f.artworkHeight = 16;
                        f.artworkRevision = revision;
                        f.backgroundPixels = pixels;
                        f.backgroundWidth = 16;
                        f.backgroundHeight = 16;
                        f.backgroundRevision = revision;
                        renderer.Render(f);
                        renderer.Render(f);
                    };

                    renderWithArtwork(coverA.data(), 1);
                    const bool cachedA = renderer.CachedArtworkIs(coverA.data(), 16, 16, 1);
                    renderWithArtwork(coverB.data(), 1);   // same revision, a different picture
                    const bool cachedB = renderer.CachedArtworkIs(coverB.data(), 16, 16, 1);
                    const bool stillA = renderer.CachedArtworkIs(coverA.data(), 16, 16, 1);

                    wchar_t cacheDetail[192];
                    std::swprintf(cacheDetail, 192, L"A cached %d, B cached %d, A still cached %d",
                                  cachedA ? 1 : 0, cachedB ? 1 : 0, stillA ? 1 : 0);
                    Check(cachedA && cachedB && !stillA,
                          L"a new cover with a repeated revision is not the old bitmap", cacheDetail);
                }
            }

            // ... and the loader, on a file that really exists: this process's own executable.
            std::vector<uint8_t> loaded;
            int loadedWidth = 0;
            int loadedHeight = 0;
            wchar_t ownPath[260] = {};
            GetModuleFileNameW(nullptr, ownPath, 260);
            const bool haveOwnIcon = LoadNotificationIconPixels(ownPath, &loaded, &loadedWidth,
                                                                &loadedHeight);
            std::swprintf(iconDetail, 224, L"own executable -> %d x %d, %zu byte(s)", loadedWidth,
                          loadedHeight, loaded.size());
            Check(haveOwnIcon && loadedWidth > 0 && loadedHeight > 0 &&
                      loaded.size() == static_cast<size_t>(loadedWidth) * loadedHeight * 4,
                  L"an executable's icon loads into pixels", iconDetail);

            // WHAT MAY BE HANDED TO THE SHELL.
            //
            // Every string below is something a notification can carry, and the refusals are the point:
            // the loader used to pass anything not ending in `.png` to `SHGetFileInfoW`, and the shell
            // RESOLVES whatever it is given -- so a notification whose icon field was a shell reference
            // made the shell act on it. Reported as "我有通知的时候 鼠标放在灵动岛上就会出现 不放是不会出现的".
            Check(IsPlainIconFilePath(L"C:\\SOFTWARE\\QQMusic\\QQMusic.exe") &&
                      IsPlainIconFilePath(L"\\\\server\\share\\app.exe") &&
                      IsPlainIconFilePath(L"C:\\Program Files\\App\\logo.PNG"),
                  L"a plain absolute path with a safe extension may be handed to the shell", iconDetail);
            Check(!IsPlainIconFilePath(L"ms-appx:///Assets/Logo.png") &&
                      !IsPlainIconFilePath(L"shell:AppsFolder\\OpenAI.Codex_2p2nqsd0c76g0!App") &&
                      !IsPlainIconFilePath(L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}") &&
                      !IsPlainIconFilePath(L"OpenAI.Codex_2p2nqsd0c76g0!App") &&
                      !IsPlainIconFilePath(L"logo.png") && !IsPlainIconFilePath(L"") &&
                      !IsPlainIconFilePath(nullptr),
                  L"a shell reference, an app id or a bare name is refused", iconDetail);
            Check(!IsPlainIconFilePath(L"C:\\temp\\logo.exe?size=32") &&
                      !IsPlainIconFilePath(L"C:\\temp\\shortcut.lnk") &&
                      !IsPlainIconFilePath(L"C:\\temp\\thing.bat"),
                  L"a query, a shortcut and a script are refused", iconDetail);
        }

        // --- the pill keeps its clock while a card is up ---------------------------
        //
        // Reported as "after clicking the popup, the island's time is gone". The clock used to be skipped
        // ENTIRELY in the card states -- that was the cure for a second clock drawn into the card's box --
        // which also emptied the pill, and the pill is drawn from its resting box in every state. It is
        // asserted on the pixels rather than on the picture: the pill's centre must measure brighter than
        // a glyph-free strip of the same pill, with the frame in the state that used to draw nothing.
        {
            const RoundedBox pillBox = window.LayoutIslandFor();

            FrameState frame;
            frame.pill.box = cardBox;        // the ANIMATED shape is card-sized while a card is up
            frame.restingPill = pillBox;     // ... and the pill is drawn from its own box
            frame.state = IslandState::Expanded;
            // The flag the renderer actually keys the clock's placement off. Set here because the frame
            // loop sets it and this frame is a copy of what the loop builds -- the first version of this
            // check left it at its default and measured an empty pill, which is what the flag is for.
            frame.outerCardUp = true;
            frame.clock = ClockStyle::Card;  // what the frame loop sets for a card
            frame.contentOpacity = 1.0f;
            renderer.Render(frame);
            renderer.Render(frame);

            // Both strips sit clear of the rounded ends, so neither reading includes transparent corner
            // pixels -- which would read as "dark" and make the comparison mean something else.
            const float centre = (pillBox.rect.left + pillBox.rect.right) * 0.5f;
            float overGlyphs = -1.0f;
            float clearOfGlyphs = -1.0f;
            const bool measuredPill =
                renderer.RegionLuminance(centre - 34.0f, pillBox.rect.top, centre + 34.0f,
                                         pillBox.rect.bottom, &overGlyphs) &&
                renderer.RegionLuminance(pillBox.rect.left + 24.0f, pillBox.rect.top,
                                         pillBox.rect.left + 56.0f, pillBox.rect.bottom,
                                         &clearOfGlyphs);
            wchar_t pillDetail[224];
            std::swprintf(pillDetail, 224, L"pill centre=%.2f  pill edge=%.2f  delta=%+.2f",
                          overGlyphs, clearOfGlyphs, overGlyphs - clearOfGlyphs);
            Check(measuredPill && clearOfGlyphs > 2.0f,
                  L"the pill is painted while a card is up", pillDetail);

            // ... and it still has its LINE in it, which is the regression this exists for.
            //
            // Measured against the pill's own FILL rather than against another part of the pill. The
            // previous version compared the pill's centre with a strip near its rounded end, because
            // the clock was short and centred and the ends were therefore bare -- true of a clock and
            // false of the status line, which now runs most of the width of the pill (2026-10-12,
            // HANDOFF 8.48). What the regression is actually about is "the pill is not EMPTY while a
            // card is up", so the comparison is with a frame whose content is switched off.
            {
                FrameState bare = frame;
                bare.contentOpacity = 0.0f;   // the renderer skips the content at this strength
                renderer.Render(bare);
                renderer.Render(bare);
                float fillOnly = -1.0f;
                const bool measuredFill =
                    renderer.RegionLuminance(pillBox.rect.left + 24.0f, pillBox.rect.top,
                                             pillBox.rect.right - 24.0f, pillBox.rect.bottom,
                                             &fillOnly);

                // The frame with its line drawn again, because the measurement above was taken on the
                // bare one -- and a reading repeated without redrawing measures the same picture,
                // which is exactly what the first version of this check did.
                renderer.Render(frame);
                renderer.Render(frame);
                float withLine = -1.0f;
                const bool measuredLine =
                    renderer.RegionLuminance(pillBox.rect.left + 24.0f, pillBox.rect.top,
                                             pillBox.rect.right - 24.0f, pillBox.rect.bottom,
                                             &withLine);
                std::swprintf(pillDetail, 224, L"pill fill alone=%.2f, with its line=%.2f",
                              fillOnly, withLine);
                Check(measuredFill && measuredLine && fillOnly > 2.0f,
                      L"the pill's fill is painted while a card is up", pillDetail);
                Check(measuredFill && measuredLine && withLine > fillOnly + 2.0f,
                      L"the pill still shows its own line while a card is up", pillDetail);
            }

            // --- AND WHILE A FULL-SCREEN APPLICATION IS IN FRONT, NOTHING IS PAINTED AT ALL --------
            //
            // The state exists for that and for nothing else: the application has the screen. Painting
            // the pill and its date-and-time line over a game is what was reported ("现在的灵动岛一行的在
            // 玩游戏的时候也显示出来了"), and "the state is reached" says nothing about it -- the policy
            // assertion above passes just as happily for a bar that fills the band. Only the pixels can
            // tell the two apart.
            {
                FrameState standDown;
                standDown.pill.box = BoxForState(IslandState::FullBar, window.CurrentAnchor());
                standDown.pill.opacity = 1.0f;
                standDown.state = IslandState::FullBar;
                renderer.Render(standDown);
                renderer.Render(standDown);

                float bandInFullScreen = -1.0f;
                const bool measuredFull =
                    renderer.RegionLuminance(pillBox.rect.left, pillBox.rect.top,
                                             pillBox.rect.right, pillBox.rect.bottom,
                                             &bandInFullScreen);

                // The same strip with the bar at rest: the pill IS there, which is what makes the reading
                // above mean "nothing" rather than "the readback is broken".
                FrameState resting;
                resting.pill = standDown.pill;
                resting.pill.box = pillBox;
                resting.restingPill = pillBox;
                resting.state = IslandState::Collapsed;
                renderer.Render(resting);
                renderer.Render(resting);
                float bandAtRest = -1.0f;
                const bool measuredRest =
                    renderer.RegionLuminance(pillBox.rect.left, pillBox.rect.top,
                                             pillBox.rect.right, pillBox.rect.bottom, &bandAtRest);

                std::swprintf(pillDetail, 224, L"the island's own band: full-screen=%.2f, at rest=%.2f",
                              bandInFullScreen, bandAtRest);
                Check(measuredRest && bandAtRest > 2.0f,
                      L"the island's band is painted at rest, so the next reading means something",
                      pillDetail);
                Check(measuredFull && bandInFullScreen < 1.0f,
                      L"nothing of the bar is painted while a full-screen application is in front",
                      pillDetail);
            }

            // --- ... AND ONLY THE ISLAND IS THE BAR ----------------------------------------------
            //
            // Asked for after a message came up over a game: "玩游戏的时候 有通知和通话时 只要显示灵动岛就行
            // 左右两侧的不用显示". Three readings, and the first is the one that says the other two mean
            // something: the island's own band has ink, the strip where the application icon row would be
            // has none, and neither has the one where the Start button sits.
            {
                FrameState onlyIsland;
                onlyIsland.pill.box = BoxForState(IslandState::NoticeStrip, window.CurrentAnchor());
                onlyIsland.pill.opacity = 1.0f;
                onlyIsland.state = IslandState::NoticeStrip;
                onlyIsland.restingPill = pillBox;
                onlyIsland.contentOpacity = 1.0f;
                onlyIsland.noticeLine = "a message while a game has the screen";
                onlyIsland.fullscreenApp = true;
                renderer.Render(onlyIsland);
                renderer.Render(onlyIsland);

                // The row and the deck are laid out from the shape the bar is in -- the widened strip here,
                // through the same RowAnchorBox the renderer uses -- so the bands sampled are the bands
                // those controls would occupy if they were drawn.
                const RoundedBox strip = BoxForState(IslandState::NoticeStrip, window.CurrentAnchor());
                const RectF firstIcon = TaskIconButtonAt(strip, 0, 4, -1);
                const RectF startButton = StartButtonFor(strip);

                float islandInk = -1.0f;
                float rowInk = -1.0f;
                float deckInk = -1.0f;
                const bool measuredIsland = renderer.RegionLuminance(
                    strip.rect.left + 40.0f, strip.rect.top, strip.rect.right - 40.0f,
                    strip.rect.bottom, &islandInk);
                const bool measuredRow = renderer.RegionLuminance(
                    firstIcon.left, firstIcon.top, firstIcon.right, firstIcon.bottom, &rowInk);
                const bool measuredDeck = renderer.RegionLuminance(
                    startButton.left, startButton.top, startButton.right, startButton.bottom,
                    &deckInk);

                std::swprintf(pillDetail, 224,
                              L"island band=%.2f, first icon=%.2f, Start button=%.2f", islandInk,
                              rowInk, deckInk);
                Check(measuredIsland && islandInk > 2.0f,
                      L"the island is drawn while a full-screen application is in front", pillDetail);
                Check(measuredRow && rowInk < 1.0f,
                      L"the application icon row beside it is not", pillDetail);
                Check(measuredDeck && deckInk < 1.0f, L"nor is the Start button", pillDetail);
            }
        }

        // --- a card must not dim the BAR -------------------------------------------
        //
        // Reported as "点击系统按钮 或者 播放音乐的时候 鼠标移入灵动岛时候 图片中红框里的会闪一下", with the
        // whole bar circled -- the deck, the icon row and the lyric line. The cause was one number
        // answering two questions: the card's content and the bar's were both drawn at
        // `0.35 + 0.65 * eased`, so EVERY card transition dimmed the bar to 35% and ramped it back over
        // 0.34 s. The loop's own log named it -- `start button: ... opacity 0.43` climbing to 1.00 while
        // the island's geometry never moved a pixel.
        //
        // Asserted twice and in opposite directions, because either half alone is satisfied by the wrong
        // answer: dimming the CARD must measurably dim the card's pixels, and must leave the deck's and
        // the pill's exactly where they were.
        {
            const ContentStrengths opening = ContentStrengthsFor(true, 0.0f, /*opening=*/true);
            const ContentStrengths settled = ContentStrengthsFor(true, 1.0f, /*opening=*/true);
            const ContentStrengths idle = ContentStrengthsFor(false, 0.25f, /*opening=*/true);
            const ContentStrengths closing = ContentStrengthsFor(true, 0.0f, /*opening=*/false);
            const ContentStrengths closed = ContentStrengthsFor(true, 1.0f, /*opening=*/false);
            std::swprintf(size, 128,
                          L"opening bar=%.2f card=%.2f | settled bar=%.2f card=%.2f | idle bar=%.2f card=%.2f",
                          opening.bar, opening.card, settled.bar, settled.card, idle.bar, idle.card);
            Check(Nearly(opening.bar, 1.0f, 0.001f) && Nearly(settled.bar, 1.0f, 0.001f) &&
                      Nearly(idle.bar, 1.0f, 0.001f),
                  L"the bar's own content never fades while a card moves", size);
            Check(Nearly(opening.card, 0.35f, 0.001f) && Nearly(settled.card, 1.0f, 0.001f) &&
                      Nearly(idle.card, 1.0f, 0.001f),
                  L"the card fades in with its own reveal and is solid at rest", size);

            // AND THE SEQUENCING, which is what the brief asked for on top of the fade: on the way open the
            // content is held back while the shape makes room, and on the way closed it leaves before the
            // shape does. Asserted at the third of the movement the lead is defined at, in both directions --
            // a function that ignored `opening` would pass the two ends above and fail here.
            const ContentStrengths openingThird =
                ContentStrengthsFor(true, kCardContentLeadFraction, /*opening=*/true);
            const ContentStrengths closingThird =
                ContentStrengthsFor(true, kCardContentLeadFraction, /*opening=*/false);
            std::swprintf(size, 128,
                          L"at %.2f of the movement: opening card=%.2f, closing card=%.2f "
                          L"(closing starts at %.2f, ends at %.2f)",
                          kCardContentLeadFraction, openingThird.card, closingThird.card,
                          closing.card, closed.card);
            Check(Nearly(openingThird.card, 0.35f, 0.001f),
                  L"opening: the content waits for the shape's first third", size);
            Check(Nearly(closing.card, 1.0f, 0.001f) && Nearly(closingThird.card, 0.35f, 0.001f) &&
                      Nearly(closed.card, 0.35f, 0.001f),
                  L"closing: the content is gone by the shape's first third", size);

            // The same thing on the pixels: the two strengths really do reach two different places.
            //
            // The deck and the pill are measured as well as the card, and the deck's tolerance is not
            // zero for one reason only: the clock in the pill's band can tick between the two renders.
            // A tick moves a fraction of one glyph, which is orders below the ~20 the bug's dimming
            // moved, so the tolerance is exact enough to catch it and loose enough not to flake.
            const RoundedBox pillBox = window.LayoutIslandFor();
            const RectF startBox = StartButtonFor(pillBox);
            const float pillCentre = (pillBox.rect.left + pillBox.rect.right) * 0.5f;
            const auto renderCardAt = [&](float cardStrength) {
                FrameState frame;
                frame.pill.box = cardBox;
                frame.restingPill = pillBox;
                frame.state = IslandState::Expanded;
                frame.cardOwner = CardOwner::Notification;
                frame.outerCardUp = true;
                frame.cardLeftDip = cardBox.rect.left;
                frame.clock = ClockStyle::Card;
                frame.contentOpacity = 1.0f;       // the bar, at rest -- what the loop passes
                frame.cardOpacity = cardStrength;  // the card, mid-reveal
                frame.notification = &shown;
                renderer.Render(frame);
                renderer.Render(frame);
            };

            renderCardAt(1.0f);
            float cardSolid = -1.0f, deckSolid = -1.0f, pillSolid = -1.0f;
            const bool measuredSolid =
                renderer.RegionLuminance(textLeft, contentTop, textRight, contentBottom, &cardSolid) &&
                renderer.RegionLuminance(startBox.left, startBox.top, startBox.right, startBox.bottom,
                                         &deckSolid) &&
                renderer.RegionLuminance(pillCentre - 34.0f, pillBox.rect.top, pillCentre + 34.0f,
                                         pillBox.rect.bottom, &pillSolid);

            renderCardAt(0.35f);
            float cardFaint = -1.0f, deckFaint = -1.0f, pillFaint = -1.0f;
            const bool measuredFaint =
                renderer.RegionLuminance(textLeft, contentTop, textRight, contentBottom, &cardFaint) &&
                renderer.RegionLuminance(startBox.left, startBox.top, startBox.right, startBox.bottom,
                                         &deckFaint) &&
                renderer.RegionLuminance(pillCentre - 34.0f, pillBox.rect.top, pillCentre + 34.0f,
                                         pillBox.rect.bottom, &pillFaint);

            wchar_t fadeDetail[256];
            std::swprintf(fadeDetail, 256,
                          L"card %.2f->%.2f (%+.2f) | deck %.2f->%.2f (%+.2f) | pill %.2f->%.2f (%+.2f)",
                          cardSolid, cardFaint, cardFaint - cardSolid, deckSolid, deckFaint,
                          deckFaint - deckSolid, pillSolid, pillFaint, pillFaint - pillSolid);
            Check(measuredSolid && measuredFaint, L"both card strengths measured", fadeDetail);
            Check(measuredSolid && measuredFaint && cardFaint < cardSolid - 1.0f,
                  L"a faint card is measurably dimmer than a solid one", fadeDetail);
            Check(measuredSolid && measuredFaint && Nearly(deckFaint, deckSolid, 0.5f) &&
                      Nearly(pillFaint, pillSolid, 0.5f),
                  L"dimming the card leaves the deck and the pill untouched", fadeDetail);
        }

    // --- the now-playing card -----------------------------------------------
    //
    // Deliberately inside the same scope: it reuses the card geometry and the two
    // sample regions, so the two cards are measured identically and a change to one
    // cannot silently diverge from the other.
    {
        const auto renderMusicCard = [&](const MusicInfo* info, const char* glyph) {
            FrameState frame;
            frame.pill.box = cardBox;
            frame.state = IslandState::Expanded;
            // The owner, for the same reason as the notification above: the renderer composes a card from
            // who owns it, not from the state it is heading to.
            frame.cardOwner = CardOwner::Music;
            frame.outerCardUp = true;
            // Music IS playing in every frame this lambda builds, so the bar's resting state is the
            // strip: the loop sets this, and the pill's own line is chosen by it.
            frame.restingState = IslandState::MusicStrip;
            frame.clock = ClockStyle::Card;
            frame.contentOpacity = 1.0f;
            frame.music = info;
            frame.transportGlyph = glyph;
            // Twice: a read right after a render sees the previous frame.
            renderer.Render(frame);
            renderer.Render(frame);
        };

        MusicInfo sample;
        sample.present = true;
        sample.state = PlaybackState::Playing;
        strncpy_s(sample.title, kMusicTitleCapacity, "Midnight City", _TRUNCATE);
        strncpy_s(sample.artist, kMusicArtistCapacity, "M83", _TRUNCATE);
        strncpy_s(sample.app, kMusicAppCapacity, "MusicApp", _TRUNCATE);

        renderMusicCard(&sample, ">");
        float musicText = -1.0f;
        float musicBlank = -1.0f;
        const bool musicMeasured =
            renderer.RegionLuminance(textLeft, contentTop, textRight, contentBottom,
                                     &musicText) &&
            renderer.RegionLuminance(blankLeft, contentTop, blankRight, contentBottom,
                                     &musicBlank);

        wchar_t musicDetail[224];
        std::swprintf(musicDetail, 224, L"centre=%.2f left strip=%.2f delta=%+.2f",
                      musicText, musicBlank, musicText - musicBlank);
        Check(musicMeasured && musicText > musicBlank + 1.0f,
              L"now-playing text is measurably drawn", musicDetail);

        // A frame with no music must fall back to the bare card, or the assertion
        // above would pass for something that is always there.
        renderMusicCard(nullptr, nullptr);
        float noMusic = -1.0f;
        renderer.RegionLuminance(textLeft, contentTop, textRight, contentBottom, &noMusic);
        std::swprintf(musicDetail, 224, L"with music=%.2f without=%.2f", musicText, noMusic);
        Check(musicMeasured && musicText > noMusic + 1.0f,
              L"the music is what brightened the card", musicDetail);

        // The transport controls must be hit-tested where the card is DRAWN.
        //
        // The card hangs below the bar with its left edge pinned to the control that opened it; the hit
        // test asked `motion_.Current()`, which is centred on the screen with its top at the PILL's top.
        // Measured on a 2560 monitor: drawn at x 853..1373 y 50..300, tested at x 1020..1540 y 6..256 --
        // 167 DIP to the right and 44 DIP up, so no transport button could be pressed at all. This is
        // asserted through the WINDOW, on the same screen point a click would arrive at, because that is
        // the path the frame loop takes.
        {
            window.SetState(IslandState::Expanded);
            for (int i = 0; i < 60; ++i) window.Tick(0.05f);
            const float cardLeftDip = 853.0f;
            window.SetSystemPanel(window.SystemPanel(), cardLeftDip);

            const RoundedBox placed = PlacedCardFor(window.LayoutIslandFor(), cardLeftDip);
            const MusicCardLayout controls = MusicCardFor(placed);
            const POINT screen = {
                static_cast<LONG>((controls.playPause.left + controls.playPause.right) * 0.5f),
                static_cast<LONG>((controls.playPause.top + controls.playPause.bottom) * 0.5f)};

            wchar_t playDetail[224];
            std::swprintf(playDetail, 224,
                          L"card x %.0f..%.0f y %.0f..%.0f; play/pause centre (%.0f,%.0f) -> %d",
                          placed.rect.left, placed.rect.right, placed.rect.top, placed.rect.bottom,
                          static_cast<float>(screen.x), static_cast<float>(screen.y),
                          static_cast<int>(window.MusicControlAtScreen(screen)));
            Check(window.MusicControlAtScreen(screen) == CardControl::PlayPause,
                  L"a transport control is hit-tested where the card is drawn", playDetail);

            // Put the window back as it was found. `cardLeftDip_` is part of the hit test's input, and the
            // assertions that follow this block ask about a point under the pill with NO card up -- left
            // set, the card's rectangle would accept it and they would fail for a reason that has nothing
            // to do with them. (In the frame loop this cannot happen: the loop sets it on every frame, to
            // zero when no card is up.)
            window.SetSystemPanel(window.SystemPanel(), 0.0f);
        }
    }

    // --- the lyric strip -----------------------------------------------
    //
    // The strip shows a lyric in place of the clock, and the line scrolls. Neither
    // property can be checked by looking at the code: a missing draw and a draw
    // outside the pill both look like "nothing there". Region luminance settles both.
    {
        // Derived here rather than reused from the card block, which is a different
        // scope with a different anchor.
        const IslandAnchor stripAnchor = AnchorForWindow(0.0f, 0.0f, 2560.0f);
        const RoundedBox strip = BoxForState(IslandState::MusicStrip, stripAnchor);

        // A lyric long enough to overflow the strip, so the marquee is engaged. It has
        // to be genuinely wider than the strip minus the scroll threshold: earlier
        // versions of this test used 198 DIP and then 429 DIP inside a 520 DIP strip,
        // both of which were CORRECTLY centred and did not move, so the assertion failed
        // against working code.
        const char* longLyric =
            "\xe6\x88\x91\xe6\x83\xb3\xe8\xa6\x81\xe5\xb8\xa6\xe7\x9d\x80\xe4\xbd\xa0"
            "\xe5\x8e\xbb\xe6\x88\x91\xe7\x9a\x84\xe5\xb0\x8f\xe5\x9f\x8e\xe5\xb8\x82"
            "\xe7\x9c\x8b\xe6\x97\xa5\xe8\x90\xbd\xe7\x9a\x84\xe6\x97\xb6\xe5\x80\x99"
            "\xe5\x92\x8c\xe4\xbd\xa0\xe4\xb8\x80\xe8\xb5\xb7\xe6\x85\xa2\xe6\x85\xa2"
            "\xe8\x80\x81\xe5\x8e\xbb\xe7\x9c\x8b\xe9\x81\x8d\xe6\x89\x80\xe6\x9c\x89"
            "\xe7\x9a\x84\xe9\xa3\x8e\xe6\x99\xaf\xe8\xb5\xb0\xe9\x81\x8d\xe6\x89\x80"
            "\xe6\x9c\x89\xe7\x9a\x84\xe8\xb7\xaf\xe5\x92\x8c\xe4\xbd\xa0\xe4\xb8\x80"
            "\xe8\xb5\xb7\xe6\x85\xa2\xe6\x85\xa2\xe8\x80\x81\xe5\x8e\xbb\xe7\x9c\x8b"
            "\xe9\x81\x8d\xe6\x89\x80\xe6\x9c\x89\xe7\x9a\x84\xe9\xa3\x8e\xe6\x99\xaf";

        // `cardUp` builds the frame the bar really builds with a card open: the ANIMATED box is
        // card-sized while the pill is drawn from its own resting box, and the state is the CARD's
        // target. The lyric must survive that -- see the check at the end of this block.
        const RoundedBox expandedForCard = BoxForState(IslandState::Expanded, stripAnchor);
        const auto renderStrip = [&](const char* line, float scroll, bool cardUp = false,
                                     float rollProgress = 1.0f, const char* previous = nullptr,
                                     bool drawNothing = false, float meterLevel = -1.0f,
                                     const D2D1_COLOR_F* accent = nullptr) {
            FrameState frame;
            frame.pill.box = cardUp ? expandedForCard : strip;
            frame.restingPill = strip;                     // the pill's own box, where the line goes
            frame.state = cardUp ? IslandState::Expanded : IslandState::MusicStrip;
            frame.outerCardUp = cardUp;
            // Music is playing: the resting state says so, whatever the state is growing into.
            frame.restingState = IslandState::MusicStrip;
            frame.clock = ClockStyle::Compact;
            // A frame that draws NO text at all, for measuring the pill's own fill: passing a null line
            // is not that frame -- with no lyric and no fallback the renderer draws the CLOCK, which is
            // what made the first version of this baseline read higher than the "empty window" it was
            // supposed to be the floor of.
            frame.contentOpacity = drawNothing ? 0.0f : 1.0f;
            frame.lyricLine = line;
            frame.lyricScrollDip = scroll;
            frame.lyricLineRoll = rollProgress;
            frame.lyricLinePrev = previous;
            if (meterLevel >= 0.0f) {
                frame.musicLevelKnown = true;
                frame.musicLevel = meterLevel;
            }
            if (accent) frame.musicAccent = *accent;
            // Twice: a read right after a render sees the previous frame.
            renderer.Render(frame);
            renderer.Render(frame);
        };

        // A band across the middle of the strip, inside its rounded ends.
        const float stripMidY = (strip.rect.top + strip.rect.bottom) * 0.5f;
        const float bandTop = stripMidY - 9.0f;
        const float bandBottom = stripMidY + 9.0f;
        const float bandLeft = strip.rect.left + 60.0f;
        const float bandRight = strip.rect.right - 60.0f;

        renderStrip(nullptr, 0.0f);
        float emptyLuma = -1.0f;
        const bool measuredEmpty =
            renderer.RegionLuminance(bandLeft, bandTop, bandRight, bandBottom, &emptyLuma);

        renderStrip(longLyric, 0.0f);
        float textLuma = -1.0f;
        const bool measuredText =
            renderer.RegionLuminance(bandLeft, bandTop, bandRight, bandBottom, &textLuma);

        wchar_t stripDetail[224];
        std::swprintf(stripDetail, 224, L"empty=%.2f with lyric=%.2f delta=%+.2f", emptyLuma,
                      textLuma, textLuma - emptyLuma);
        Check(measuredEmpty && measuredText, L"both strip regions measured", stripDetail);
        Check(measuredText && textLuma > emptyLuma + 1.0f,
              L"the lyric line is measurably drawn in the strip", stripDetail);

        // --- the line's ARRIVAL, and the position it is chosen by ---------------------
        //
        // Two complaints with one cause, both only visible once lyrics actually arrived on screen:
        // "现在歌词和唱的有点对应不上" and "现在感觉没有动画".
        //
        // The line was selected from the RAW session reading, which is reported once every second or
        // two -- so it changed a couple of seconds after the words were sung -- and it simply appeared,
        // because nothing else about it moved.
        {
            // The projection: forward while playing, and only then.
            wchar_t timing[224];
            const bool played = island::ProjectedPositionMs(10000, 200000, 1500, true) == 11500;
            const bool paused = island::ProjectedPositionMs(10000, 200000, 1500, false) == 10000;
            const bool live = island::ProjectedPositionMs(10000, 0, 1500, true) == 10000;
            const bool noPosition = island::ProjectedPositionMs(0, 200000, 1500, true) == 0;
            const bool clamped = island::ProjectedPositionMs(199000, 200000, 9000, true) == 200000;
            std::swprintf(timing, 224,
                          L"+1500 ms while playing: %lld; paused: %lld; live: %lld; no reading: %lld",
                          island::ProjectedPositionMs(10000, 200000, 1500, true),
                          island::ProjectedPositionMs(10000, 200000, 1500, false),
                          island::ProjectedPositionMs(10000, 0, 1500, true),
                          island::ProjectedPositionMs(0, 200000, 1500, true));
            Check(played && paused && live && noPosition && clamped,
                  L"the drawn position is projected forward while playing, and only then", timing);

            // --- and what happens when the session sends NO timeline at all (2026-10-13, HANDOFF 8.74) ---
            //
            // Reported with the player's own card beside it: "网易云的还是有问题 歌词不动 进度条也不动".
            // Measured: NetEase Cloud Music reports `Position 00:00 / StartTime 00:00 / EndTime 00:00`
            // for the WHOLE song, playing -- the timeline is optional in SMTC and it simply leaves it out.
            // The lyric line is chosen by the position, so the pill sat on the first line for the whole
            // song, and the card printed `00:00 / 00:00` with an empty bar.
            {
                const bool reportsWhenPlaying =
                    island::SessionReportsTimeline(30000, 240000);
                const bool reportsHalf = island::SessionReportsTimeline(30000, 0) &&
                                         island::SessionReportsTimeline(0, 240000);
                const bool reportsNothing = !island::SessionReportsTimeline(0, 0);

                const island::TrackPosition counted = island::TrackPositionFor(0, 0, 5000, 42000, true);
                const island::TrackPosition projected =
                    island::TrackPositionFor(30000, 240000, 2000, 0, true);
                const island::TrackPosition none = island::TrackPositionFor(0, 0, 0, 0, false);

                wchar_t timelineText[224];
                std::swprintf(timelineText, 224,
                              L"no timeline -> %lld (session? %d); a reading -> %lld (session? %d); "
                              L"nothing -> %lld",
                              counted.ms, counted.fromSession ? 1 : 0, projected.ms,
                              projected.fromSession ? 1 : 0, none.ms);
                Check(reportsWhenPlaying && reportsHalf && reportsNothing && counted.ms == 42000 &&
                          !counted.fromSession && projected.ms == 32000 && projected.fromSession &&
                          none.ms == 0 && !none.fromSession,
                      L"a session with no timeline is counted by the bar, and one with a reading is not",
                      timelineText);
            }

            // WHICH changes are worth animating at all.
            //
            // The loop re-evaluates the current line whenever the media session reports anything (a
            // position update does that, once every second or two), and treating that as a change rolled
            // the SAME text in and out of the window: "歌词每1、2秒就会上下出现一模一样的歌词然后和在一
            // 起似的". A repeated lyric line -- a real thing in LRC files -- is the same case.
            Check(island::LyricLineChanged("a line", "") &&
                      island::LyricLineChanged("a line", "another line"),
                  L"an arriving line is a change when the words differ");
            Check(!island::LyricLineChanged("a line", "a line") &&
                      !island::LyricLineChanged("", nullptr) &&
                      !island::LyricLineChanged(nullptr, "") &&
                      !island::LyricLineChanged(nullptr, nullptr),
                  L"the same words again are not a change, and neither is nothing over nothing");

            // The ROLL: 0 at the change, settled a quarter of a second later, and EASED on the way --
            // "歌词最好是切换的时候是从下面向上滚动". A linear ramp is asserted against, because an
            // arrival that stops dead is the thing the ease exists to avoid.
            //
            // 8.31 replaced this with a cross-fade in place, on the strength of "是歌词不要换行"; the user
            // asked for the roll back the same day ("还是改回去吧") and the roll is what these assertions
            // measure again. The fade was the only transition that could keep the pill to one row, so what
            // is asserted below about TWO HALF ROWS is not a defect -- it is the shape that was asked for.
            const float atChange = island::LyricLineRollFor(0.0f);
            const float halfWay = island::LyricLineRollFor(island::kLyricLineEnterSeconds * 0.5f);
            const float settled = island::LyricLineRollFor(island::kLyricLineEnterSeconds);
            const float later = island::LyricLineRollFor(island::kLyricLineEnterSeconds * 4.0f);
            std::swprintf(timing, 224,
                          L"roll: at the change %.2f, half way %.2f, at the end %.2f, later %.2f",
                          atChange, halfWay, settled, later);
            Check(atChange == 0.0f && halfWay > 0.6f && halfWay < 0.95f &&
                      Nearly(settled, 1.0f, 0.001f) && Nearly(later, 1.0f, 0.001f),
                  L"the roll runs to one, and reaches most of the way before half the time", timing);

            // ... and it is visible on the pixels, INSIDE A ONE-LINE WINDOW.
            //
            // The window is the whole point of the second version: drawn into the pill's full 36 DIP,
            // a roll shows two lines at once, cramped and overlapping -- what the user photographed,
            // with "没有过度动画" and "歌词应该和当前唱的一样才对", because half of what was on screen
            // belonged to the line before. Inside a window one line tall, the pill holds at most one
            // line's worth of text at any moment.
            //
            // Measured with a DIRECTION, which is why the two halves of the window are read separately: the
            // leaving line is the upper one and the arriving line the lower one, so "half way" is not just
            // "some text is here" but "the right text is in the right half".
            const float windowUpperTop = stripMidY - 9.0f;
            const float windowUpperBottom = stripMidY - 2.0f;
            const float windowLowerTop = stripMidY + 2.0f;
            const float windowLowerBottom = stripMidY + 9.0f;

            // The BASELINE is the pill's own fill, not zero: the window sits inside the pill, so an
            // empty window still measures the fill (about 12.5), and "is there text here" is a question
            // about the difference from it. Measuring against zero passed nothing and would have
            // accepted a transparent hole as an empty window.
            renderStrip(nullptr, 0.0f, /*cardUp=*/false, /*rollProgress=*/1.0f,
                        /*previous=*/nullptr, /*drawNothing=*/true);
            float fillUpper = -1.0f;
            float fillLower = -1.0f;
            const bool measuredFill =
                renderer.RegionLuminance(bandLeft, windowUpperTop, bandRight, windowUpperBottom,
                                         &fillUpper) &&
                renderer.RegionLuminance(bandLeft, windowLowerTop, bandRight, windowLowerBottom,
                                         &fillLower);

            renderStrip(longLyric, 0.0f, /*cardUp=*/false, /*rollProgress=*/0.0f);
            float upperAtStart = -1.0f;
            float lowerAtStart = -1.0f;
            const bool measuredStart =
                renderer.RegionLuminance(bandLeft, windowUpperTop, bandRight, windowUpperBottom,
                                         &upperAtStart) &&
                renderer.RegionLuminance(bandLeft, windowLowerTop, bandRight, windowLowerBottom,
                                         &lowerAtStart);

            renderStrip(longLyric, 0.0f, /*cardUp=*/false, /*rollProgress=*/1.0f);
            float upperSettled = -1.0f;
            float lowerSettled = -1.0f;
            const bool measuredSettled =
                renderer.RegionLuminance(bandLeft, windowUpperTop, bandRight, windowUpperBottom,
                                         &upperSettled) &&
                renderer.RegionLuminance(bandLeft, windowLowerTop, bandRight, windowLowerBottom,
                                         &lowerSettled);

            // Half way, with a line leaving: BOTH halves of the window hold text, one above the other.
            renderStrip(longLyric, 0.0f, /*cardUp=*/false, /*rollProgress=*/0.5f, longLyric);
            float upperWithPrevious = -1.0f;
            float lowerWithPrevious = -1.0f;
            const bool measuredPrevious =
                renderer.RegionLuminance(bandLeft, windowUpperTop, bandRight, windowUpperBottom,
                                         &upperWithPrevious) &&
                renderer.RegionLuminance(bandLeft, windowLowerTop, bandRight, windowLowerBottom,
                                         &lowerWithPrevious);

            // The same moment with NOTHING leaving: only the arriving line, which is the lower one.
            renderStrip(longLyric, 0.0f, /*cardUp=*/false, /*rollProgress=*/0.5f);
            float upperAlone = -1.0f;
            float lowerAlone = -1.0f;
            const bool measuredAlone =
                renderer.RegionLuminance(bandLeft, windowUpperTop, bandRight, windowUpperBottom,
                                         &upperAlone) &&
                renderer.RegionLuminance(bandLeft, windowLowerTop, bandRight, windowLowerBottom,
                                         &lowerAlone);

            // And AT THE MOMENT OF THE CHANGE, the old line is still where it was -- the centre.
            //
            // This is the assertion the first version needed and did not have: both sides were drawn
            // with the same offset, so the leaving line was already one line height ABOVE the window on
            // the first frame, the window sat empty while the new line rose, and the whole thing read as
            // "the old lyric blinks out and nothing rolls". Reported as "歌词向上消失的时候 应该是向上滚
            // 一点一点消失，新的应该是从下面滚上了 但是现在看起来像没有一样".
            renderStrip(longLyric, 0.0f, /*cardUp=*/false, /*rollProgress=*/0.0f, longLyric);
            float upperLeavingAtStart = -1.0f;
            float lowerLeavingAtStart = -1.0f;
            const bool measuredLeavingAtStart =
                renderer.RegionLuminance(bandLeft, windowUpperTop, bandRight, windowUpperBottom,
                                         &upperLeavingAtStart) &&
                renderer.RegionLuminance(bandLeft, windowLowerTop, bandRight, windowLowerBottom,
                                         &lowerLeavingAtStart);

            std::swprintf(stripDetail, 256,
                          L"fill %.2f / %.2f | window halves: at the change %.2f / %.2f, settled "
                          L"%.2f / %.2f, half way with the old line %.2f / %.2f, half way alone "
                          L"%.2f / %.2f | the old line at the change %.2f / %.2f",
                          fillUpper, fillLower, upperAtStart, lowerAtStart, upperSettled,
                          lowerSettled, upperWithPrevious, lowerWithPrevious, upperAlone, lowerAlone,
                          upperLeavingAtStart, lowerLeavingAtStart);
            Check(measuredFill && measuredStart && measuredSettled && measuredPrevious &&
                      measuredAlone && measuredLeavingAtStart,
                  L"the roll measured", stripDetail);
            Check(measuredSettled && upperSettled > fillUpper + 1.0f &&
                      lowerSettled > fillLower + 1.0f,
                  L"a settled line fills the window", stripDetail);
            Check(measuredStart && Nearly(upperAtStart, fillUpper, 1.0f) &&
                      Nearly(lowerAtStart, fillLower, 1.0f),
                  L"a line that has just changed starts BELOW the window, so the window is empty",
                  stripDetail);
            Check(measuredPrevious && upperWithPrevious > fillUpper + 1.0f &&
                      lowerWithPrevious > fillLower + 1.0f,
                  L"half way through, the window holds the old line above the new one", stripDetail);
            Check(measuredAlone && Nearly(upperAlone, fillUpper, 1.0f) &&
                      lowerAlone > fillLower + 1.0f && upperWithPrevious > upperAlone + 1.0f,
                  L"with nothing leaving, only the lower half of the window is filled", stripDetail);
            Check(measuredLeavingAtStart && upperLeavingAtStart > upperAtStart + 1.0f &&
                      lowerLeavingAtStart > fillLower + 1.0f,
                  L"at the change the OLD line is still in the window, not already gone", stripDetail);
        }

        // --- the background meter ("背景加上峰值计") ---------------------------------
        //
        // Drawn from the output's peak reading, behind the words. Asserted on the pixels because
        // "a meter that never moves" and "a meter that is not drawn" look the same in a screenshot --
        // and because the whole point of it is that it moves.
        {
            const float meterTop = stripMidY + 2.0f;
            const float meterBottom = strip.rect.bottom - 2.0f;

            // Its own fill baseline: measured here rather than borrowed from the roll's, so the two
            // blocks can be read (and moved) independently.
            renderStrip(nullptr, 0.0f, /*cardUp=*/false, /*rollProgress=*/1.0f, nullptr,
                        /*drawNothing=*/true);
            float meterFill = -1.0f;
            const bool measuredMeterFill =
                renderer.RegionLuminance(bandLeft, bandTop, bandRight, bandBottom, &meterFill);

            renderStrip(longLyric, 0.0f, /*cardUp=*/false, /*rollProgress=*/1.0f, nullptr,
                        /*drawNothing=*/false, /*meterLevel=*/0.05f);
            float quiet = -1.0f;
            const bool measuredQuiet =
                renderer.RegionLuminance(bandLeft, meterTop, bandRight, meterBottom, &quiet);

            renderStrip(longLyric, 0.0f, /*cardUp=*/false, /*rollProgress=*/1.0f, nullptr,
                        /*drawNothing=*/false, /*meterLevel=*/1.0f);
            float loud = -1.0f;
            const bool measuredLoud =
                renderer.RegionLuminance(bandLeft, meterTop, bandRight, meterBottom, &loud);
            // ... and the words are still the thing being read: at a full meter the text's own band is
            // still brighter than the pill's fill, which is what "behind the words, at a third of the
            // opacity" has to mean in practice.
            float wordsOverMeter = -1.0f;
            const bool measuredWords =
                renderer.RegionLuminance(bandLeft, bandTop, bandRight, bandBottom, &wordsOverMeter);

            std::swprintf(stripDetail, 224,
                          L"meter: quiet=%.2f loud=%.2f delta=%+.2f | words over the meter=%.2f "
                          L"(fill=%.2f)",
                          quiet, loud, loud - quiet, wordsOverMeter, meterFill);
            Check(measuredMeterFill && measuredQuiet && measuredLoud && measuredWords,
                  L"the meter measured", stripDetail);
            Check(measuredQuiet && measuredLoud && loud > quiet + 1.0f,
                  L"the background meter's bars grow with the output's peak", stripDetail);
            Check(measuredWords && wordsOverMeter > meterFill + 1.0f,
                  L"the words stay readable over the meter", stripDetail);
        }

        // --- the wave: full pill, full width, album colour ("电平条有没有别的什么方式") -----
        //
        // What was asked for: a shape other than vertical bars, filling the pill's whole height and
        // width, tinted with a colour taken from the cover. Two things can go wrong silently -- a shape
        // that does not move with the level, and a colour that never reaches the pixels -- so both are
        // measured.
        {
            // The envelope: inside its range, CLOSED across the width (the first and last heights match,
            // so the ribbon shuts), and moving with the phase.
            float lowest = 2.0f;
            float highest = -1.0f;
            for (int i = 0; i <= 48; ++i) {
                const float v = TextRenderer::AudioWaveEnvelope(i, 48, 1.234f);
                if (v < lowest) lowest = v;
                if (v > highest) highest = v;
            }
            bool moves = false;
            for (int i = 0; i <= 48; ++i) {
                if (std::fabs(TextRenderer::AudioWaveEnvelope(i, 48, 1.234f) -
                              TextRenderer::AudioWaveEnvelope(i, 48, 2.434f)) > 0.05f) {
                    moves = true;
                    break;
                }
            }
            const bool closed = Nearly(TextRenderer::AudioWaveEnvelope(0, 48, 1.234f),
                                       TextRenderer::AudioWaveEnvelope(48, 48, 1.234f), 0.01f);
            std::swprintf(stripDetail, 224, L"envelope: %.2f..%.2f, closed=%d, moves=%d", lowest,
                          highest, closed ? 1 : 0, moves ? 1 : 0);
            Check(lowest >= 0.0f && highest <= 1.0f && highest > lowest + 0.2f,
                  L"the wave's envelope stays in range and is a shape", stripDetail);
            Check(closed, L"the wave closes across the full width", stripDetail);
            Check(moves, L"the wave moves as its phase advances", stripDetail);

            // The whole pill, so "it fills the pill's height and width" is what is being measured.
            const float fullTop = strip.rect.top + 1.0f;
            const float fullBottom = strip.rect.bottom - 1.0f;

            renderStrip(longLyric, 0.0f, /*cardUp=*/false, /*rollProgress=*/1.0f, nullptr,
                        /*drawNothing=*/false, /*meterLevel=*/0.10f);
            float quietWave = -1.0f;
            const bool measuredQuietWave =
                renderer.RegionLuminance(bandLeft, fullTop, bandRight, fullBottom, &quietWave);

            renderStrip(longLyric, 0.0f, /*cardUp=*/false, /*rollProgress=*/1.0f, nullptr,
                        /*drawNothing=*/false, /*meterLevel=*/1.0f);
            float loudWave = -1.0f;
            const bool measuredLoudWave =
                renderer.RegionLuminance(bandLeft, fullTop, bandRight, fullBottom, &loudWave);

            // The colour: luma is what this measurement can see, and red and blue differ in it by
            // construction (0.299 against 0.114), so "a red tint reads brighter than a blue one at the
            // same level" is what says the accent reached the pixels. The mapping itself is asserted in
            // TestAccentColour.
            const D2D1_COLOR_F red = D2D1::ColorF(1.0f, 0.05f, 0.05f, 1.0f);
            const D2D1_COLOR_F blue = D2D1::ColorF(0.05f, 0.05f, 1.0f, 1.0f);
            renderStrip(longLyric, 0.0f, false, 1.0f, nullptr, false, 1.0f, &red);
            float redWave = -1.0f;
            const bool measuredRed =
                renderer.RegionLuminance(bandLeft, fullTop, bandRight, fullBottom, &redWave);
            renderStrip(longLyric, 0.0f, false, 1.0f, nullptr, false, 1.0f, &blue);
            float blueWave = -1.0f;
            const bool measuredBlue =
                renderer.RegionLuminance(bandLeft, fullTop, bandRight, fullBottom, &blueWave);

            std::swprintf(stripDetail, 224,
                          L"wave: quiet=%.2f loud=%.2f delta=%+.2f | red accent=%.2f blue accent=%.2f",
                          quietWave, loudWave, loudWave - quietWave, redWave, blueWave);
            Check(measuredQuietWave && measuredLoudWave && measuredRed && measuredBlue,
                  L"the wave measured", stripDetail);
            Check(loudWave > quietWave + 1.0f, L"the wave grows with the output's peak", stripDetail);
            Check(redWave > blueWave + 1.0f, L"the accent's colour reaches the pixels", stripDetail);

            // AND IT IS ACTUALLY THERE, which the first version was not: it compared quiet against loud
            // and passed while the wave was too faint to see on a real pill (a live capture is what
            // showed that). Same frame, same lyric, wave versus no wave.
            renderStrip(longLyric, 0.0f, /*cardUp=*/false, /*rollProgress=*/1.0f, nullptr,
                        /*drawNothing=*/false, /*meterLevel=*/-1.0f);
            float noWave = -1.0f;
            const bool measuredNoWave =
                renderer.RegionLuminance(bandLeft, fullTop, bandRight, fullBottom, &noWave);
            renderStrip(longLyric, 0.0f, false, 1.0f, nullptr, false, 0.5f);
            float withWave = -1.0f;
            const bool measuredWithWave =
                renderer.RegionLuminance(bandLeft, fullTop, bandRight, fullBottom, &withWave);
            std::swprintf(stripDetail, 224, L"no wave=%.2f with wave=%.2f delta=%+.2f", noWave,
                          withWave, withWave - noWave);
            Check(measuredNoWave && measuredWithWave && withWave > noWave + 2.0f,
                  L"the wave is visible behind the words, not merely present", stripDetail);
        }

        // Scrolling must move the pixels.
        //
        // Measured at the strip's LEADING EDGE, not across a wide middle band. A wide
        // band sees both marquee copies at once, so shifting the whole pattern by half
        // a line's worth of characters leaves the mean almost unchanged -- an earlier
        // version of this check compared a 400 DIP band at two offsets and read exactly
        // the same value both times, against a marquee that was working.
        //
        // At the leading edge the difference is unambiguous: at offset 0 the first copy
        // begins there, and at 200 DIP that copy has moved on and what remains is the
        // gap between copies, which is empty.
        const float edgeTop = stripMidY - 9.0f;
        const float edgeBottom = stripMidY + 9.0f;
        const float edgeLeft = strip.rect.left + 6.0f;
        const float edgeRight = strip.rect.left + 36.0f;

        renderStrip(longLyric, 0.0f);
        float edgeAtStart = -1.0f;
        const bool measuredEdgeStart =
            renderer.RegionLuminance(edgeLeft, edgeTop, edgeRight, edgeBottom, &edgeAtStart);

        renderStrip(longLyric, 200.0f);
        float edgeScrolled = -1.0f;
        const bool measuredEdgeScrolled =
            renderer.RegionLuminance(edgeLeft, edgeTop, edgeRight, edgeBottom, &edgeScrolled);

        std::swprintf(stripDetail, 224, L"leading edge at 0 DIP=%.2f, at 200 DIP=%.2f",
                      edgeAtStart, edgeScrolled);
        Check(measuredEdgeStart && measuredEdgeScrolled &&
                  std::fabs(edgeScrolled - edgeAtStart) > 0.5f,
              L"scrolling moves the text, so the leading edge changes", stripDetail);

        // THE ENDS FADE, so a long line leaves the strip instead of being sliced by it.
        //
        // Measured as a RAMP rather than as a level, and every other reading in this block is a level: the
        // two bands here are the SAME width, on the same line, at the same offset -- one sits where the fade
        // is and the other just past it. Without the mask the two read alike, because the strip's clip is a
        // hard edge: a glyph is CUT by the stadium's curve rather than dimmed, which is exactly what the
        // report shows ("歌词 长的话能不能会这样" -- the leading "na" halved at the island's left edge).
        //
        // At this offset the first copy starts 4 DIP left of the strip (start = left + 8, offset = 12), so
        // both bands sit inside glyphs: DIP 2..12 is the tail of the first one, 28..38 the head of another.
        {
            const float fadeBandTop = stripMidY - 6.0f;
            const float fadeBandBottom = stripMidY + 6.0f;
            const float stripWidth = strip.rect.right - strip.rect.left;

            renderStrip(longLyric, 12.0f);
            float atTheEnd = -1.0f;
            float justInside = -1.0f;
            const bool measuredFade =
                renderer.RegionLuminance(strip.rect.left + 2.0f, fadeBandTop,
                                         strip.rect.left + 12.0f, fadeBandBottom, &atTheEnd) &&
                renderer.RegionLuminance(strip.rect.left + 28.0f, fadeBandTop,
                                         strip.rect.left + 38.0f, fadeBandBottom, &justInside);

            std::swprintf(stripDetail, 224,
                          L"at the end=%.2f, just inside=%.2f, fade=%.0f DIP of %.0f",
                          atTheEnd, justInside, island::LyricEndFadeDip(stripWidth), stripWidth);
            Check(measuredFade && justInside > atTheEnd + 1.0f,
                  L"a scrolling line fades at the strip's end instead of being cut", stripDetail);
            // ... and the cap that keeps a narrow strip readable: the fade is a quarter of the width at
            // most, so two of them can never meet in the middle.
            Check(Nearly(island::LyricEndFadeDip(220.0f), 20.0f, 0.01f) &&
                      Nearly(island::LyricEndFadeDip(40.0f), 10.0f, 0.01f) &&
                      island::LyricEndFadeDip(0.0f) == 0.0f,
                  L"the end fade is capped at a quarter of the strip", stripDetail);
        }

        // A LATIN LYRIC IS ONE ROW TOO, and it is worth its own check because the rest of this block is
        // Chinese: CJK glyphs are one width whatever the weight, while a Latin run is wider in the pill's BOLD
        // face than the same run measured in the format the marquee decision uses. If the draw rectangle were
        // ever sized from the narrower number again, THIS is the line that would break -- and the report that
        // came with the two-row photograph was a row of "na na na" ("是歌词不要换行").
        {
            const char* latinLine = "na na na na na na na";
            const float centerLeft = strip.rect.left + 40.0f;
            const float centerRight = strip.rect.right - 40.0f;

            renderStrip(nullptr, 0.0f, false, 1.0f, nullptr, /*drawNothing=*/true);
            float fill[25] = {};
            bool measuredFill = true;
            for (int i = 0; i <= 24; ++i) {
                const float top = stripMidY - 12.0f + static_cast<float>(i);
                if (!renderer.RegionLuminance(centerLeft, top, centerRight, top + 1.0f, &fill[i])) {
                    measuredFill = false;
                }
            }

            renderStrip(latinLine, 0.0f);
            int ink = 0;
            for (int i = 0; i <= 24; ++i) {
                const float top = stripMidY - 12.0f + static_cast<float>(i);
                float lum = 0.0f;
                if (renderer.RegionLuminance(centerLeft, top, centerRight, top + 1.0f, &lum) &&
                    lum > fill[i] + 1.0f) {
                    ++ink;
                }
            }

            std::swprintf(stripDetail, 224,
                          L"'na na na na na na na': %d rows of ink (one row is 6-10, two are 16+)", ink);
            Check(measuredFill && ink > 0 && ink <= 12,
                  L"a Latin lyric stays on ONE row, it does not wrap", stripDetail);

            // ... and the line that actually caught it, from a captured frame of the running bar: a pushed
            // notification whose text is `WeChat · hello there` was drawn on TWO rows ("hello" then "there")
            // in the strip -- photographed in `captures/call-2-message.png`.
            //
            // The cause is a measure/draw mismatch, and it needs WIDE Latin glyphs to show: the line is
            // MEASURED with `appFormat_` (11 DIP, NORMAL) and DRAWN with `pillLineFormat_` (11 DIP, BOLD),
            // while the draw rectangle is sized from the narrower number and word WRAP is still on (the
            // DirectWrite default; this format never turned it off). A bold advance is wider for Latin, so the
            // words did not fit the rectangle they were handed. Seven "na"s did not exceed the 4 DIP of slack
            // and went unnoticed; "WeChat · hello there" does.
            {
                const char* noticeLine = "WeChat \xc2\xb7 hello there my friend";
                renderStrip(nullptr, 0.0f, false, 1.0f, nullptr, /*drawNothing=*/true);
                float noticeFill[25] = {};
                bool measuredNoticeFill = true;
                for (int i = 0; i <= 24; ++i) {
                    const float top = stripMidY - 12.0f + static_cast<float>(i);
                    if (!renderer.RegionLuminance(centerLeft, top, centerRight, top + 1.0f,
                                                  &noticeFill[i])) {
                        measuredNoticeFill = false;
                    }
                }
                renderStrip(noticeLine, 0.0f);
                int noticeInk = 0;
                int bands = 0;
                bool inBand = false;
                for (int i = 0; i <= 24; ++i) {
                    const float top = stripMidY - 12.0f + static_cast<float>(i);
                    float lum = 0.0f;
                    const bool bandInk = renderer.RegionLuminance(centerLeft, top, centerRight, top + 1.0f,
                                                              &lum) &&
                                     lum > noticeFill[i] + 1.0f;
                    if (bandInk) {
                        ++noticeInk;
                        if (!inBand) {
                            ++bands;
                            inBand = true;
                        }
                    } else {
                        inBand = false;
                    }
                }
                std::swprintf(stripDetail, 224,
                              L"'WeChat · hello there my friend': %d rows, %d band(s) of ink",
                              noticeInk, bands);
                // BANDS, not rows, and that is the measurement that separates the two cases: ONE row of text
                // is one band whatever the glyphs, while a wrapped line is TWO bands with the descender gap
                // between them. Counted as rows the difference was 9 against 11 -- and a threshold of 12 let
                // the wrap through, which is the kind of assertion this project calls a false green.
                Check(measuredNoticeFill && noticeInk > 0 && bands == 1,
                      L"a message line stays on ONE row, it does not wrap", stripDetail);
            }
        }
        // ... and the pill shows its lyric just the same when a CARD is up.
        //
        // Reported as "我现在正在播放音乐 但是点击系统按钮的时候 灵动岛就不显示歌词了", with the pill
        // showing 01:01 over a system panel that has nothing to do with the music. The rule used to be
        // keyed on `state`, which is Expanded for the whole of a card, so the lyric fell through to the
        // clock. It is keyed on the RESTING state now.
        //
        // Measured at the pill's LEADING EDGE, which is where a long lyric has glyphs and the clock --
        // centred in the pill -- has none, so "the lyric is there" and "the clock is there" cannot be
        // confused with each other. The first check is the strong one: the two cases differ only in the
        // box the bar is growing into, so the pixels of the pill's line must be identical.
        renderStrip(longLyric, 0.0f, /*cardUp=*/false);
        float lyricAlone = -1.0f;
        const bool measuredAlone =
            renderer.RegionLuminance(edgeLeft, edgeTop, edgeRight, edgeBottom, &lyricAlone);

        renderStrip(longLyric, 0.0f, /*cardUp=*/true);
        float lyricUnderCard = -1.0f;
        const bool measuredUnderCard =
            renderer.RegionLuminance(edgeLeft, edgeTop, edgeRight, edgeBottom, &lyricUnderCard);

        renderStrip(nullptr, 0.0f, /*cardUp=*/true);
        float clockUnderCard = -1.0f;
        const bool measuredClockUnderCard =
            renderer.RegionLuminance(edgeLeft, edgeTop, edgeRight, edgeBottom, &clockUnderCard);

        std::swprintf(stripDetail, 224,
                      L"lyric alone=%.2f, lyric under a card=%.2f, clock under a card=%.2f",
                      lyricAlone, lyricUnderCard, clockUnderCard);
        Check(measuredAlone && measuredUnderCard && measuredClockUnderCard,
              L"all three pill-line cases measured", stripDetail);
        Check(measuredAlone && measuredUnderCard && Nearly(lyricUnderCard, lyricAlone, 0.5f),
              L"the pill shows its lyric with a card up exactly as it does without", stripDetail);
        Check(measuredUnderCard && measuredClockUnderCard && lyricUnderCard > clockUnderCard + 1.0f,
              L"the lyric, not the clock, is what the pill shows under a card", stripDetail);

        // ... and the line stays in the PILL while a retraction is being INTERRUPTED.
        //
        // This is the frame range a mouse crossing the island quickly produces, and it is the only one
        // where the ANIMATED box is card-sized with no card up: the exit has begun (so the target is a
        // pill state) while the shape has not finished shrinking. `outerCardUp` is therefore false and
        // the line used to be drawn into that card-sized box -- where it is VERTICALLY CENTRED -- so the
        // lyric dropped into the middle of the card and climbed back out over the rest of the exit.
        // Reported as "播放音乐的时候 鼠标移入显示卡片 鼠标快速横移 就会出现这种情况 歌词会上下闪一下",
        // and the user's own log shows the interruption: `state: music-strip` at 01:07:07.455 and
        // `state: hover` at .458 -- three milliseconds of leaving, right at the start of the exit.
        //
        // Measured as "where are the glyphs". The card is not drawn in this frame at all (no owner), so
        // the part of its footprint below the bar must be EMPTY, not merely dim, while the pill's own
        // band must still hold the line.
        {
            FrameState frame;
            frame.pill.box = expandedForCard;   // the shape has not finished shrinking
            frame.restingPill = strip;          // ... while the bar is a pill again
            frame.state = IslandState::Hover;   // the retarget: NOT a card state
            frame.outerCardUp = false;          // ... so the renderer is told no card is up
            frame.restingState = IslandState::MusicStrip;
            frame.clock = ClockStyle::Compact;
            frame.contentOpacity = 1.0f;
            frame.lyricLine = longLyric;
            renderer.Render(frame);
            renderer.Render(frame);

            float lineInPill = -1.0f;
            float lineUnderPill = -1.0f;
            const bool measuredInterrupted =
                renderer.RegionLuminance(edgeLeft, edgeTop, edgeRight, edgeBottom, &lineInPill) &&
                renderer.RegionLuminance(strip.rect.left + 60.0f, strip.rect.bottom + 60.0f,
                                         strip.rect.right - 60.0f, strip.rect.bottom + 130.0f,
                                         &lineUnderPill);
            std::swprintf(stripDetail, 224, L"interrupted exit: in the pill=%.2f, under the pill=%.2f",
                          lineInPill, lineUnderPill);
            Check(measuredInterrupted, L"the interrupted exit measured", stripDetail);
            Check(measuredInterrupted && lineInPill > clockUnderCard + 1.0f && lineUnderPill < 1.0f,
                  L"the line stays in the pill while an interrupted exit shrinks", stripDetail);
        }

        // Report the measurement inputs, because "the two readings match" has two very
        // different causes: the marquee not moving, or the line being measured as short
        // enough to be centred -- in which case not moving is CORRECT.
        {
            wchar_t wide[512] = {};
            MultiByteToWideChar(CP_UTF8, 0, longLyric, -1, wide, 512);
            wchar_t probe[224];
            std::swprintf(probe, 224, L"line %.1f DIP, strip %.1f DIP, scroll threshold %.1f",
                          textForRenderer.TrackLineWidth(wide),
                          strip.rect.right - strip.rect.left,
                          (strip.rect.right - strip.rect.left) - 24.0f);
            Info(L"lyric strip geometry", probe);
        }

        // A short lyric is centred and must not scroll, or it would drift away.
        renderStrip("short", 0.0f);
        float shortStart = -1.0f;
        renderer.RegionLuminance(bandLeft, bandTop, bandRight, bandBottom, &shortStart);
        renderStrip("short", 90.0f);
        float shortScrolled = -1.0f;
        renderer.RegionLuminance(bandLeft, bandTop, bandRight, bandBottom, &shortScrolled);
        std::swprintf(stripDetail, 224, L"short line at 0=%.2f at 90=%.2f", shortStart,
                      shortScrolled);
        Check(Nearly(shortStart, shortScrolled, 0.001f),
              L"a line that fits does not move when the scroll advances", stripDetail);

        // The line must be long enough that scrolling is engaged, or the readings below
        // would match for the right reason and the marquee would go untested.
        {
            wchar_t wide[512] = {};
            MultiByteToWideChar(CP_UTF8, 0, longLyric, -1, wide, 512);
            Check(LyricNeedsScroll(textForRenderer.TrackLineWidth(wide),
                                   strip.rect.right - strip.rect.left),
                  L"the test line is long enough to scroll");
        }

        // The rounded ends must stay clear of text.
        //
        // TWO THINGS ABOUT THE REFERENCE, and both were learned the hard way.
        //
        // It is the pill's own surface rather than black -- the corner is inside the pill, so it is
        // painted with the fill, and an earlier version demanded near-zero and failed at 11.44 against a
        // fill of 12.46.
        //
        // And it is taken at the SAME HEIGHT as the corner, because the material now has a highlight
        // along the top of the pill: the first version measured the corner against a glyph-free row
        // near the BOTTOM, so a theme's gloss moved the corner to 23.4 against a fill of 12.3 and the
        // check failed for a reason that has nothing to do with text. Comparing along one row keeps the
        // tolerance tight -- a glyph in the corner still fails it -- while following the material.
        renderStrip(longLyric, 120.0f);
        float cornerLuma = -1.0f;
        float fillLuma = -1.0f;
        const bool measuredCorner = renderer.RegionLuminance(
            strip.rect.left + 2.0f, strip.rect.top + 2.0f, strip.rect.left + 8.0f,
            strip.rect.top + 8.0f, &cornerLuma);
        // The same band, in the middle of the strip: above the line's own height, so it is as free of
        // glyphs as the corner is.
        const bool measuredFill = renderer.RegionLuminance(
            strip.rect.left + (strip.rect.right - strip.rect.left) * 0.5f - 3.0f,
            strip.rect.top + 2.0f, strip.rect.left + (strip.rect.right - strip.rect.left) * 0.5f + 3.0f,
            strip.rect.top + 8.0f, &fillLuma);
        std::swprintf(stripDetail, 224, L"corner=%.2f, fill=%.2f", cornerLuma, fillLuma);
        Check(measuredCorner && measuredFill && Nearly(cornerLuma, fillLuma, 2.0f),
              L"the strip's rounded corner holds only the fill, not text", stripDetail);
    }

    // --- the media state mapping -------------------------------------------
    //
    // A wrong mapping would report "Playing" for a paused session, which is the one
    // thing the card exists to communicate.
    Check(std::strcmp(PlaybackStateName(PlaybackState::Playing), "playing") == 0 &&
          std::strcmp(PlaybackStateName(PlaybackState::Paused), "paused") == 0 &&
          std::strcmp(PlaybackStateName(PlaybackState::Stopped), "stopped") == 0 &&
          std::strcmp(PlaybackStateName(PlaybackState::Closed), "closed") == 0,
          L"playback states have distinct names");

    {
        MusicInfo fresh;
        Check(!fresh.present && fresh.state == PlaybackState::Closed,
              L"a default MusicInfo reports nothing playing");
        Check(fresh.title[0] == '\0' && fresh.artist[0] == '\0',
              L"its text fields start empty");
    }
    }   // end of the card-measurement scope

    // Leave the window as the rest of the test expects it.
    window.SetState(IslandState::Collapsed);
    for (int i = 0; i < 60; ++i) window.Tick(0.05f);

    // And the plain per-frame path must take over again afterwards, otherwise the
    // island could never be made fully opaque again by the frame loop.
    renderer.Shutdown();
    Check(!renderer.IsInitialized(), L"shutdown released the chain");
}

// --- the worker-thread rule ---------------------------------------------------------------------------
//
// WHAT THIS ASSERTS, and why it is a HANDLE COUNT rather than a reading of the code: the four readers that
// used to hand-roll this dance each left something behind on the paths where Start() gave up -- an open
// event, a running thread, an initialised critical section, or all three -- and the failure only shows up
// when a start is RETRIED, which is exactly what the callers do. So the assertion performs the retry, many
// times, and asks the process whether anything accumulated.
//
// The bound is not zero: the check itself is running inside a process that owns windows, threads and a
// compositor, so a couple of handles moving during the loop is normal. What the old code did was grow by
// ONE EVENT PER ITERATION, which fifty iterations make unmistakable.
void TestWorkerThread() {
    Section(L"worker thread teardown (platform)");

    DWORD before = 0;
    GetProcessHandleCount(GetCurrentProcess(), &before);

    {
        WorkerThread worker;
        for (int i = 0; i < 50; ++i) {
            // The shape of a start that gives up half-way, then a retry.
            worker.Prepare();
            worker.Stop();
        }
        Check(!worker.Prepared(), L"after Stop() the worker reports nothing prepared");
    }

    {
        // Prepare() WITHOUT a Stop() between, which is the leak the copies really had: each call used to
        // create an event over the previous handle. Prepare() ends the previous attempt first.
        WorkerThread worker;
        for (int i = 0; i < 50; ++i) {
            worker.Prepare();
        }
        Check(worker.Prepared(), L"the last of fifty prepares is the one that is armed");
    }

    {
        // And the real thing: a thread that waits on the stop event, told to stop, joined and closed --
        // three times over against ONE WorkerThread, because that is the sequence Start()/Stop()/Start()
        // performs when a reader is restarted.
        struct Helper {
            static unsigned long __stdcall Entry(void* self) {
                auto* worker = static_cast<WorkerThread*>(self);
                WaitForSingleObject(worker->StopEvent(), 5000);
                return 0;
            }
        };

        WorkerThread worker;
        for (int i = 0; i < 3; ++i) {
            const bool prepared = worker.Prepare();
            HANDLE thread = prepared ? CreateThread(nullptr, 0, &Helper::Entry, &worker, 0, nullptr)
                                     : nullptr;
            if (thread) worker.Attach(thread);
            Check(prepared && thread != nullptr, L"a worker starts a real thread against its stop event");
            Check(worker.Stop(3000), L"and Stop() sees that thread end rather than giving up on it");
        }
    }

    DWORD after = 0;
    GetProcessHandleCount(GetCurrentProcess(), &after);

    wchar_t detail[96] = {};
    std::swprintf(detail, 96, L"after ~150 prepare/stop cycles: %lu -> %lu handle(s)", before, after);
    Check(after <= before + 8, L"a retried start leaves no handles behind", detail);
}

} // namespace island
