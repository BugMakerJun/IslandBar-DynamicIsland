// =====================================================================
//  ChecksMotion.cpp -- see selftest/Checks.h for why the self-test is split like this
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
#include "domain/NiriBarLayout.h"
#include "render/Renderer.h"
#include "render/TextRenderer.h"
#include "shell/TrayIcons.h"
#include "shell/TrayPress.h"
#include "shell/TrayLearn.h"   // CountCoveredIcons: how much of the tray a press can reach (see the tray checks)

#include "selftest/Checks.h"
#include "selftest/Harness.h"

namespace island {

void TestMotion(float originX, float originY, float monitorWidthDip) {
    Section(L"island states and motion");

    const IslandAnchor anchor = AnchorForWindow(originX, originY, monitorWidthDip);

    // --- target geometry ---
    const RoundedBox collapsed = BoxForState(IslandState::Collapsed, anchor);
    const RoundedBox hover     = BoxForState(IslandState::Hover, anchor);
    const RoundedBox expanded  = BoxForState(IslandState::Expanded, anchor);

    const auto widthOf  = [](const RoundedBox& b) { return b.rect.right - b.rect.left; };
    const auto heightOf = [](const RoundedBox& b) { return b.rect.bottom - b.rect.top; };

    Check(Nearly(widthOf(collapsed), kCollapsedWidthDip) &&
          Nearly(heightOf(collapsed), kCollapsedHeightDip),
          L"collapsed is 220x36");
    Check(Nearly(widthOf(hover), kHoverWidthDip) &&
          Nearly(heightOf(hover), kHoverHeightDip),
          L"hover is 720x36");
    Check(Nearly(widthOf(expanded), kExpandedWidthDip) &&
          Nearly(heightOf(expanded), kExpandedHeightDip),
          L"expanded is the width and height it declares");

    // Search lives inside the resting pill: the island never grows when the
    // search field takes over its contents.
    {
        const RoundedBox search = BoxForState(IslandState::SearchStrip, anchor);
        wchar_t detail[192];
        std::swprintf(detail, 192,
                      L"pill [%.0f..%.0f], search [%.0f..%.0f]",
                      collapsed.rect.left, collapsed.rect.right,
                      search.rect.left, search.rect.right);
        Check(Nearly(search.rect.left, collapsed.rect.left, 0.01f) &&
                  Nearly(search.rect.right, collapsed.rect.right, 0.01f) &&
                  Nearly(heightOf(search), heightOf(collapsed), 0.01f),
              L"search keeps the resting pill geometry", detail);

        const NiriSearchLayout field = NiriSearchLayoutFor(search, 1.0f);
        const NiriSearchLayout start = NiriSearchLayoutFor(search, 0.0f);
        const float leftInset = field.field.left - search.rect.left;
        const float topInset = field.field.top - search.rect.top;
        const float rightInset = search.rect.right - field.field.right;
        const float bottomInset = search.rect.bottom - field.field.bottom;
        std::swprintf(detail, 192,
                      L"insets l/t/r/b=%.1f/%.1f/%.1f/%.1f, editor %.1f..%.1f",
                      leftInset, topInset, rightInset, bottomInset,
                      field.editor.left, field.editor.right);
        Check(field.valid &&
                  Nearly(leftInset, kNiriSearchInsetDip, 0.01f) &&
                  Nearly(topInset, kNiriSearchInsetDip, 0.01f) &&
                  Nearly(rightInset, kNiriSearchInsetDip, 0.01f) &&
                  Nearly(bottomInset, kNiriSearchInsetDip, 0.01f) &&
                  Nearly(field.editor.left - field.field.left,
                         field.field.right - field.editor.right, 0.01f),
              L"the inline search field and its editor have symmetric insets", detail);

        const NiriSearchMotion rest = NiriSearchMotionFor(0.0f);
        const NiriSearchMotion middle = NiriSearchMotionFor(0.5f);
        const NiriSearchMotion open = NiriSearchMotionFor(1.0f);
        std::swprintf(detail, 192,
                      L"field %.1f -> %.1f, content shift %.1f -> %.1f -> %.1f, "
                      L"opacity %.2f -> %.2f",
                      start.field.right - start.field.left, field.field.right - field.field.left,
                      rest.contentShiftDip, middle.contentShiftDip, open.contentShiftDip,
                      rest.contentOpacity, open.contentOpacity);
        Check(start.valid && field.valid &&
                  Nearly(start.field.left, field.field.left, 0.01f) &&
                  field.field.right - field.field.left >
                      start.field.right - start.field.left + 100.0f &&
                  Nearly(rest.contentShiftDip, 0.0f, 0.01f) &&
                  Nearly(rest.contentOpacity, 1.0f, 0.01f) &&
                  middle.contentShiftDip > rest.contentShiftDip &&
                  middle.contentShiftDip < open.contentShiftDip &&
                  middle.contentOpacity > 0.0f && middle.contentOpacity < 1.0f &&
                  Nearly(open.contentShiftDip, kNiriSearchContentTravelDip, 0.01f) &&
                  Nearly(open.contentOpacity, 0.0f, 0.01f),
              L"search grows to the right while its old content slides away", detail);

        const NiriSearchResultsLayout results =
            NiriSearchResultsLayoutFor(search, 3, false, 1.0f);
        const NiriSearchResultsLayout resultsMid =
            NiriSearchResultsLayoutFor(search, 3, false, 0.5f);
        const int firstHit =
            NiriSearchResultAt(results, (results.rows[0].left + results.rows[0].right) * 0.5f,
                               (results.rows[0].top + results.rows[0].bottom) * 0.5f);
        const int belowHit =
            NiriSearchResultAt(results, (results.rows[0].left + results.rows[0].right) * 0.5f,
                               results.panel.rect.bottom + 2.0f);
        std::swprintf(detail, 192,
                      L"search panel y[%.1f..%.1f], row 0 hit=%d, below hit=%d",
                      results.panel.rect.top, results.panel.rect.bottom, firstHit, belowHit);
        Check(results.valid && results.count == 3 &&
                  resultsMid.valid &&
                  Nearly(resultsMid.panel.rect.bottom - resultsMid.panel.rect.top,
                         (results.panel.rect.bottom - results.panel.rect.top) * 0.5f,
                         0.01f) &&
                  Nearly(results.panel.rect.left, search.rect.left, 0.01f) &&
                  Nearly(results.panel.rect.right, search.rect.right, 0.01f) &&
                  Nearly(results.panel.rect.top,
                         search.rect.bottom + kNiriSearchResultsGapDip, 0.01f) &&
                  firstHit == 0 && belowHit == -1,
              L"search results hang below the island and share one row geometry", detail);

        const RectF wave = NiriMusicWaveButtonFor(search);
        std::swprintf(detail, 192, L"wave x[%.1f..%.1f] y[%.1f..%.1f]",
                      wave.left, wave.right, wave.top, wave.bottom);
        Check(Nearly((wave.left + wave.right) * 0.5f,
                     search.rect.right - kNiriMusicWaveCentreFromRightDip, 0.01f) &&
                  Nearly(wave.right - wave.left, kNiriMusicWaveHitDip, 0.01f) &&
                  Nearly((wave.top + wave.bottom) * 0.5f,
                         (search.rect.top + search.rect.bottom) * 0.5f, 0.01f),
              L"the music wave keeps one fixed hit rectangle at the right end", detail);

        float notchExtent = 0.0f;
        const bool notchStarted = NiriNotchExtentStep(&notchExtent, 1.0f, 0.06f);
        const float notchQuarter = notchExtent;
        const bool notchContinues = NiriNotchExtentStep(&notchExtent, 1.0f, 0.06f);
        const bool notchSettles = NiriNotchExtentStep(&notchExtent, 1.0f, 1.0f);
        std::swprintf(detail, 192, L"notch extent %.2f -> %.2f -> %.2f",
                      notchQuarter, notchExtent, notchExtent);
        Check(notchStarted && notchContinues && notchSettles &&
                  notchQuarter > 0.0f && notchQuarter < 1.0f &&
                  Nearly(notchExtent, 1.0f, 0.01f),
              L"the Niri notch side panels expand over multiple frames", detail);

        const float sideGroupsShown = NiriSideGroupTarget(false, true);
        const float sideGroupsHidden = NiriSideGroupTarget(true, true);
        const float noSideContent = NiriSideGroupTarget(false, false);
        float retreat = 1.0f;
        const bool retreatStarted =
            NiriNotchExtentStep(&retreat, sideGroupsHidden, 0.06f);
        const float retreatMid = retreat;
        const bool retreatContinues =
            NiriNotchExtentStep(&retreat, sideGroupsHidden, 0.06f);
        const bool retreatSettles =
            NiriNotchExtentStep(&retreat, sideGroupsHidden, 1.0f);
        std::swprintf(detail, 192,
                      L"Niri sides shown %.0f hidden %.0f absent %.0f; retreat %.2f -> %.2f",
                      sideGroupsShown, sideGroupsHidden, noSideContent, retreatMid, retreat);
        Check(Nearly(sideGroupsShown, 1.0f, 0.001f) &&
                  Nearly(sideGroupsHidden, 0.0f, 0.001f) &&
                  Nearly(noSideContent, 0.0f, 0.001f) && retreatStarted &&
                  retreatContinues && retreatSettles && retreatMid > 0.0f &&
                  retreatMid < 1.0f && Nearly(retreat, 0.0f, 0.01f),
              L"Niri sides show in both shapes and disappear over multiple frames for a card",
              detail);

        constexpr float halfExtent = 0.5f;
        const NiriBarLayout taskLayout = NiriBarLayoutFor(monitorWidthDip, search, 2);
        const float visualOffset = NiriSideGroupVisualOffsetDip(halfExtent);
        const float taskCentreY =
            (taskLayout.taskButtons[0].top + taskLayout.taskButtons[0].bottom) * 0.5f;
        const float restingProbe = taskLayout.taskButtons[0].left + 0.25f;
        const int shiftedHit = NiriTaskAtVisual(
            taskLayout, taskLayout.taskButtons[0].left + visualOffset,
            taskCentreY, halfExtent);
        const int staleHit = NiriTaskAtVisual(taskLayout, restingProbe, taskCentreY, halfExtent);
        std::swprintf(detail, 192,
                      L"Niri task slide %.1f DIP; shifted hit=%d stale hit=%d",
                      visualOffset, shiftedHit, staleHit);
        Check(taskLayout.taskCount == 2 && visualOffset > 0.0f &&
                  visualOffset < kNiriSideGroupSlideDip &&
                  Nearly(NiriSideGroupVisualOffsetDip(1.0f), 0.0f, 0.001f) &&
                  shiftedHit == 0 && staleHit == -1,
              L"Niri task hit testing follows the side group's rendered slide", detail);
    }

    // OPENING MUST NOT GET NARROWER ANYWHERE.
    //
    // The card was 400 DIP while the lyric strip is 520, so the first ~100 ms of "opening"
    // pulled the sides IN while the bottom dropped -- an inward twitch on hover entry,
    // reported as a stutter. This is the assertion that states the rule, so the two widths
    // cannot drift apart again: the card may be wider than the strip, never narrower.
    {
        const RoundedBox strip = BoxForState(IslandState::MusicStrip, anchor);
        wchar_t detail[160];
        std::swprintf(detail, 160, L"strip %.0f DIP, card %.0f DIP", widthOf(strip),
                      widthOf(expanded));
        Check(widthOf(expanded) >= widthOf(strip),
              L"opening the card from the lyric strip never narrows it", detail);
        Check(widthOf(expanded) >= widthOf(collapsed),
              L"opening the card from the plain pill never narrows it either");
    }

    // The two short states are stadiums: radius exactly half the height, so the
    // ends are true semicircles.
    const auto isStadium = [&](const RoundedBox& b) {
        return Nearly(b.radiusX, heightOf(b) * 0.5f, 0.01f) &&
               Nearly(b.radiusY, heightOf(b) * 0.5f, 0.01f);
    };
    Check(isStadium(collapsed) && isStadium(hover), L"pill and hover are stadiums");

    // The card deliberately is NOT. At 132 DIP tall, half the height would be a
    // 66 DIP radius, which turns a 520-wide card into a lozenge and eats most of
    // its usable width; the card uses a small fixed radius instead.
    Check(Nearly(expanded.radiusX, kCardRadiusDip, 0.01f),
          L"the card uses the card radius, not half its height");
    Check(Nearly(kCardRadiusDip, 32.0f, 0.01f),
          L"the three Stitch cards share a 32 DIP outer radius");
    Check(expanded.radiusX < heightOf(expanded) * 0.5f,
          L"the card is not a lozenge");

    // All three share one TOP edge. The pill is top-anchored rather than centred,
    // because a centred 132 DIP card would have to extend 66 DIP above the
    // work-area edge -- and above that edge there is only the taskbar and then the
    // top of the screen, so the card would be clipped by the display.
    Check(Nearly(collapsed.rect.top, hover.rect.top, 0.01f) &&
          Nearly(collapsed.rect.top, expanded.rect.top, 0.01f),
          L"states share one top edge");

    // So the card opens downward, the way a notification drops out of the
    // taskbar, and stays inside the window.
    Check(expanded.rect.bottom > collapsed.rect.bottom,
          L"card opens downward");

    // The pass/fail for "does it actually fit in the window" belongs in
    // TestWindow, which has the real window rect. These probe coordinates come
    // from the monitor geometry, not from the window, and the two differ by the
    // strip's own top edge, so asserting fit here would be measuring the wrong
    // rectangle.
    Check(expanded.rect.bottom - expanded.rect.top <= kWindowHeightDip,
          L"card is no taller than the window allows");

    // --- interpolation ---
    const RoundedBox midpoint = LerpBox(collapsed, hover, 0.5f);
    Check(Nearly(widthOf(midpoint), (kCollapsedWidthDip + kHoverWidthDip) * 0.5f, 0.1f),
          L"midpoint blends the widths");
    // Both ends are stadiums here, so a blended radius is also half the (equal)
    // height. The interesting case is the card, tested further down.
    Check(Nearly(midpoint.radiusX, heightOf(midpoint) * 0.5f, 0.01f),
          L"the radius tracks the height between two stadiums");

    // Interpolating towards the card must land between the two radii. Blending
    // 18 (stadium) and 20 (card) at the midpoint gives 19, so the bounds are the
    // two ends -- not kCardRadiusDip itself, which is only the value at t = 1.
    const RoundedBox quarterCard = LerpBox(collapsed, expanded, 0.5f);
    {
        wchar_t detail[192];
        std::swprintf(detail, 192, L"collapsed r=%.2f expanded r=%.2f blend r=%.2f",
                      collapsed.radiusX, expanded.radiusX, quarterCard.radiusX);
        Check(quarterCard.radiusX >= collapsed.radiusX - 0.01f &&
              quarterCard.radiusX <= expanded.radiusX + 0.01f,
              L"the radius blends between the two ends", detail);
    }

    const RoundedBox clampedLow  = LerpBox(collapsed, hover, -5.0f);
    const RoundedBox clampedHigh = LerpBox(collapsed, hover, 5.0f);
    Check(Nearly(widthOf(clampedLow), kCollapsedWidthDip, 0.1f) &&
          Nearly(widthOf(clampedHigh), kHoverWidthDip, 0.1f),
          L"lerp clamps an out-of-range t");

    // --- easing ---
    Check(Nearly(EaseOutCubic(0.0f), 0.0f) && Nearly(EaseOutCubic(1.0f), 1.0f),
          L"ease-out is pinned at both ends");
    Check(Nearly(EaseInOutCubic(0.0f), 0.0f) && Nearly(EaseInOutCubic(1.0f), 1.0f),
          L"ease-in-out is pinned at both ends");
    Check(EaseOutCubic(0.5f) > 0.5f, L"ease-out leads a linear ramp");
    Check(Nearly(EaseInOutCubic(0.5f), 0.5f, 0.001f), L"ease-in-out is symmetric");

    // The curve a card transition uses must DECELERATE, and must move furthest at the start.
    //
    // This is the assertion behind "the panel's opening animation feels janky", which was reported three
    // times while its duration was adjusted and never helped. The problem was never the duration: it was
    // the SHAPE. A symmetric S-curve sits almost still for the first third, crosses the distance in the
    // middle third at 12 DIP per frame, and then stops dead -- a stall, a lurch and another stall, which
    // is exactly what "stiff" and "janky" describe. Sampled here at the cadence the bar actually runs at,
    // so this is a statement about frames rather than about an ideal curve: of 60 steps across a 0.36 s
    // transition, no step may be longer than the one before it, and the longest must be one of the first.
    //
    // Checked against CurveFor rather than against a named curve, because CurveFor is what the motion
    // calls: naming EaseOutCubic here would leave the curve free to change back underneath the test.
    {
        constexpr int kSteps = 60;   // ~165 Hz for the 0.36 s a card takes to open
        float (*curve)(float) = CurveFor(IslandState::Collapsed, IslandState::Expanded);

        float previous = -1.0f;
        float longest = 0.0f;
        int   longestAt = -1;
        bool  neverSpeedsUp = true;

        for (int i = 0; i < kSteps; ++i) {
            const float step = curve(static_cast<float>(i + 1) / kSteps) -
                               curve(static_cast<float>(i) / kSteps);
            if (step > longest) {
                longest = step;
                longestAt = i;
            }
            if (previous >= 0.0f && step > previous + 0.00001f) neverSpeedsUp = false;
            previous = step;
        }

        wchar_t detail[192];
        std::swprintf(detail, 192,
                      L"%d steps; longest %.4f at step %d; a decelerating curve peaks at step 0 "
                      L"(a symmetric one peaks near %d)",
                      kSteps, longest, longestAt, kSteps / 2);
        Check(neverSpeedsUp, L"a card transition never speeds up as it goes", detail);
        Check(longestAt <= 1, L"a card transition moves furthest at the start", detail);
        Check(Nearly(curve(0.0f), 0.0f, 0.001f) && Nearly(curve(1.0f), 1.0f, 0.001f),
              L"the curve a card transition uses still spans 0..1", detail);
    }

    // --- state machine ---
    Motion motion;
    motion.SetAnchor(anchor);
    motion.SnapTo(IslandState::Collapsed);

    Check(motion.Target() == IslandState::Collapsed && !motion.IsAnimating(),
          L"snap settles immediately");
    Check(Nearly(motion.Current().rect.right - motion.Current().rect.left,
                 kCollapsedWidthDip, 0.1f),
          L"snap produced the collapsed box");

    // Asking for the state already targeted must not restart anything: the hover
    // check runs 125 times a second and a restart per call would freeze the
    // animation at its first frame forever.
    motion.Tick(0.05f);
    Check(!motion.SetTarget(IslandState::Collapsed), L"re-targeting the same state is a no-op");

    Check(motion.SetTarget(IslandState::Expanded), L"to opened starts a transition");
    Check(motion.IsAnimating(), L"transition is in flight");

    const float widthAtStart = motion.Current().rect.right - motion.Current().rect.left;

    // Step to roughly the middle of the transition and confirm the shape is
    // strictly between the two ends -- not snapped to either.
    //
    // This targeted Hover, and that stopped being a width change when Hover was made the same size as
    // the resting pill: hovering no longer resizes the island at all, the icon row does the responding.
    // The assertion was right and pointed at a transition that no longer moves anything. Expanded
    // genuinely changes both width and height.
    motion.Tick(0.10f);
    const float widthMid = motion.Current().rect.right - motion.Current().rect.left;
    Check(widthMid > widthAtStart && widthMid < kExpandedWidthDip,
          L"mid-transition width lies between the ends");

    // Overshooting the duration must land exactly on the target, not past it.
    motion.Tick(1.0f);
    Check(!motion.IsAnimating(), L"transition settles");
    Check(Nearly(motion.Current().rect.right - motion.Current().rect.left,
                 kExpandedWidthDip, 0.1f),
          L"settled exactly on the opened width");
    Check(Nearly(motion.Progress(), 1.0f), L"progress reaches 1");

    // The NOTICE STRIP widens over time as well, and this is the assertion that would have caught the
    // report it came from.
    //
    // "没有动画啊" is what a shape that jumps looks like, and there are two ways to get one: a transition with
    // no duration, or a box that animates while something that should follow it is pinned elsewhere. The
    // second one was real (the island's own FILL was drawn from the resting pill, so the widening happened
    // to a box nothing was painted from). The first is what this checks: the strip is a genuine transition,
    // with a duration, and the shape at its middle is strictly between the pill and the strip.
    motion.SnapTo(IslandState::Collapsed);
    const float stripSeconds = TransitionSeconds(IslandState::Collapsed, IslandState::NoticeStrip,
                                                 MotionTiming{});
    Check(stripSeconds > 0.05f, L"widening into the notice strip takes time");
    Check(motion.SetTarget(IslandState::NoticeStrip), L"to notice strip starts a transition");
    motion.Tick(stripSeconds * 0.5f);
    const float stripWidthMid = motion.Current().rect.right - motion.Current().rect.left;
    motion.Tick(1.0f);
    const float stripWidthEnd = motion.Current().rect.right - motion.Current().rect.left;
    wchar_t stripDetail[256];
    std::swprintf(stripDetail, 256, L"pill %.0f -> mid %.0f -> strip %.0f, duration %.2f s",
                  kCollapsedWidthDip, stripWidthMid, stripWidthEnd, stripSeconds);
    Check(stripWidthMid > kCollapsedWidthDip + 5.0f && stripWidthMid < kNoticeStripWidthDip - 5.0f,
          L"the island is part-way wide in the middle of the widening", stripDetail);
    Check(Nearly(stripWidthEnd, kNoticeStripWidthDip, 0.1f),
          L"the widening ends on the notice strip's width", stripDetail);

    // "Which states does this transition touch?" must be answerable while it runs AND false once it has
    // finished.
    //
    // FromState() returned the stored previous target unconditionally, so `Target() == Expanded ||
    // FromState() == Expanded` -- the test the frame loop uses to ask "is a card involved?" -- was true
    // for ever after the first card. Measured consequence: a 36 DIP slice of the system panel was still
    // drawn under the bar after the panel had been put away. A question about what is on screen has to be
    // able to become false again.
    motion.SnapTo(IslandState::Collapsed);
    Check(motion.FromState() == IslandState::Collapsed,
          L"a settled motion reports the state it is in");
    motion.SetTarget(IslandState::Expanded);
    motion.Tick(0.05f);
    Check(motion.FromState() == IslandState::Collapsed,
          L"in flight the motion reports the state it came from");
    motion.Tick(1.0f);
    Check(!motion.IsAnimating() && motion.FromState() == IslandState::Expanded,
          L"a settled card still reports the card it is showing");
    motion.SetTarget(IslandState::Collapsed);
    motion.Tick(1.0f);
    Check(!motion.IsAnimating() && motion.FromState() == IslandState::Collapsed,
          L"once the card has gone, nothing claims it is involved");

    // The panel's reveal must be EMPTY at the near end of a transition, not 36 DIP of it.
    //
    // The box the outer cards are drawn in is the PILL at one end (36 DIP) and the card at the other
    // (250), so a clip taken from the box height leaves a band of the panel on screen at both ends --
    // which is what the retraction's slow tail was made of, and why it read as "it stops and then
    // disappears". The mapping is asserted at its ends and for being monotone, because the failure mode
    // is exactly a non-zero floor.
    {
        const float atPill = OuterCardRevealDip(kCollapsedHeightDip);
        const float atCard = OuterCardRevealDip(kExpandedHeightDip);
        const float midCard = OuterCardRevealDip((kCollapsedHeightDip + kExpandedHeightDip) * 0.5f);
        const float belowPill = OuterCardRevealDip(kCollapsedHeightDip - 20.0f);
        const float musicAtCard =
            CardRevealDipForHeight(kExpandedHeightDip, kMusicCardHeightDip);
        wchar_t detail[192];
        std::swprintf(detail, 192, L"reveal: pill %.1f, mid %.1f, card %.1f, below %.1f DIP",
                      atPill, midCard, atCard, belowPill);
        Check(Nearly(atPill, 0.0f, 0.01f), L"nothing of a card is revealed at the pill's height", detail);
        Check(Nearly(atCard, kExpandedHeightDip, 0.01f),
              L"the whole card is revealed at the card's height", detail);
        Check(Nearly(belowPill, 0.0f, 0.01f), L"the reveal cannot go negative", detail);
        Check(midCard > atPill && midCard < atCard, L"the reveal grows with the box", detail);
        Check(Nearly(musicAtCard, kMusicCardHeightDip, 0.01f),
              L"a compact owner reveals only its own card height");
    }

    // Reversing mid-flight must continue from where the shape is, never jump.
    // This is the bug that makes a bar look broken when the pointer skims the
    // pill: a restart from the target would snap the shape outward first.
    motion.SnapTo(IslandState::Collapsed);
    motion.SetTarget(IslandState::Hover);
    motion.Tick(0.08f);
    const RoundedBox beforeReverse = motion.Current();
    motion.SetTarget(IslandState::Collapsed);
    const RoundedBox afterReverse = motion.Current();
    Check(Nearly(beforeReverse.rect.right, afterReverse.rect.right, 1.0f) &&
          Nearly(beforeReverse.rect.left, afterReverse.rect.left, 1.0f),
          L"reversal continues from the current shape");
    motion.Tick(1.0f);
    Check(Nearly(motion.Current().rect.right - motion.Current().rect.left,
                 kCollapsedWidthDip, 0.1f),
          L"reversal reaches the collapsed width");

    // The card transition is the one that changes height, so the curve and the
    // duration both differ. Confirm the height really does move.
    motion.SnapTo(IslandState::Collapsed);
    motion.SetTarget(IslandState::Expanded);
    motion.Tick(0.13f);
    const float midHeight = motion.Current().rect.bottom - motion.Current().rect.top;
    Check(midHeight > kCollapsedHeightDip && midHeight < kExpandedHeightDip,
          L"card transition animates the height");
    // Mid-transition the radius must sit between the two ends' radii -- 18 for the
    // stadium and 24 for the card -- and must NOT be half the current height, which
    // is what the old height-derived version produced and what made the card bulge
    // into a lozenge (half of 84 DIP is 42).
    {
        const float midRadius = motion.Current().radiusX;
        wchar_t detail[160];
        std::swprintf(detail, 160, L"mid radius=%.2f, half the mid height=%.2f",
                      midRadius, midHeight * 0.5f);
        // Correct bounds: 18 is the stadium radius (half the resting height) and 24
        // is the card radius, so 19 is exactly between them. Getting these the wrong
        // way round is easy -- this check failed twice for that reason alone.
        Check(midRadius >= kCollapsedHeightDip * 0.5f - 0.01f &&
              midRadius <= kCardRadiusDip + 0.01f,
              L"card transition keeps the radius between the two ends", detail);
        Check(!Nearly(midRadius, midHeight * 0.5f, 0.5f),
              L"the card radius does not track half the height");
    }

    // A taller change gets a longer duration than a sideways one.
    Check(TransitionSeconds(IslandState::Collapsed, IslandState::Expanded,
                            MotionTiming{}) >
          TransitionSeconds(IslandState::Collapsed, IslandState::Hover, MotionTiming{}),
          L"the card takes longer than a spread");

    {
        Motion staged;
        staged.SetAnchor(anchor);
        staged.SetStagedExpansion(true);
        staged.SnapTo(IslandState::Collapsed);
        staged.SetTarget(IslandState::Expanded);
        staged.Tick(0.04f);
        const RoundedBox early = staged.Current();
        staged.Tick(0.16f);
        const RoundedBox later = staged.Current();
        Check(early.rect.right - early.rect.left > kCollapsedWidthDip + 20.0f &&
                  Nearly(early.rect.bottom - early.rect.top, kCollapsedHeightDip, 0.1f),
              L"the unified card expands sideways first and leaves the height alone");
        Check(later.rect.bottom - later.rect.top > kCollapsedHeightDip,
              L"... then the height follows after the width");
    }
}

void TestClock() {
    Section(L"clock");

    TextRenderer text;
    const bool ready = text.Initialize();
    Check(ready, L"DirectWrite formats created");
    if (!ready) return;

    wchar_t clock[16] = {};
    text.CurrentTimeText(clock, 16);
    const size_t length = std::wcslen(clock);
    Check(length == 5, L"clock is hh:mm");

    const bool shaped = length == 5 && clock[2] == L':' &&
                        iswdigit(clock[0]) && iswdigit(clock[1]) &&
                        iswdigit(clock[3]) && iswdigit(clock[4]);
    Check(shaped, L"clock looks like a time");

    // The hour must be a real hour: a formatting slip that produced "99:99"
    // would still be five characters with a colon in the middle.
    if (shaped) {
        const int hour = (clock[0] - L'0') * 10 + (clock[1] - L'0');
        const int minute = (clock[3] - L'0') * 10 + (clock[4] - L'0');
        wchar_t detail[64];
        std::swprintf(detail, 64, L"reads %ls", clock);
        Check(hour < 24 && minute < 60, L"clock is a valid time", detail);
    }

    // The second must advance on its own. Waiting for a rollover keeps this
    // honest: a cached value that never updates would otherwise pass.
    int first = -1;
    text.CurrentSecond(&first);
    Check(first >= 0 && first < 60, L"second is in range");

    text.Shutdown();
    Check(!text.IsReady(), L"shutdown released DirectWrite");
}

} // namespace island
