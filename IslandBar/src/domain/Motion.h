// =====================================================================
//  Motion.h
//  The island's three states and the interpolation between them.
//
//  This file owns two things and nothing else:
//    * the target geometry of each state, and
//    * how far a transition has progressed, and what that looks like.
//
//  It deliberately does NOT own the clock, the window or the renderer. The
//  animation is a pure function of elapsed time, which is what makes it testable
//  without a screen: given a start box, a target box and a duration, Tick() must
//  produce a specific box.
//
//  Design rule carried over from phase 0-d: the window is never resized. Every
//  state change moves the pill's own width, height and corner radius inside a
//  window that stays exactly as it was created. Resizing a composition
//  swap-chain window flickers, so the window is a fixed stage and the pill is
//  the actor.
// =====================================================================
#pragma once

#include "domain/BarShape.h"
#include "domain/Geometry.h"

namespace island {

enum class IslandState : int {
    Collapsed  = 0,  // at rest, nothing playing: a small pill showing the time
    Hover      = 1,  // the pointer is on the bar; the ICON ROW responds, not the island
    Expanded   = 2,  // a card: grows tall, for content that needs room
    MusicStrip = 3,  // music is playing: shows a lyric in place of the clock, same size
    FullBar    = 4,  // the whole width of the screen, for a full-screen application
    // A message has arrived: the island WIDENS, LEFT AND RIGHT, and the message is shown inside it.
    //
    // Asked for in exactly those words ("有通知的时候 灵动岛变宽 在里面显示 … 是左右宽展"), and it is the
    // Dynamic Island gesture rather than the card: a notification is a glance, and a 520x250 card hanging
    // under the bar is not a glance. The card is still what a HOVER opens, which is where the full body is
    // read (see NextShape).
    //
    // Growing symmetrically is the island's own arithmetic rather than a choice -- every state's box is
    // centred on the screen, so a wider state reaches equally both ways. What makes that safe now, and what
    // did not exist when a wide strip last covered the Start button, is that the deck is anchored to the
    // island's LEFT EDGE (DeckButtonFor / StartButtonFor): widening pushes the buttons outward with it
    // instead of leaving them behind at a fixed place.
    NoticeStrip = 5,
    // Search is an island-only horizontal state: the left edge stays on the
    // resting pill while the right edge extends into a text field.
    SearchStrip = 6,
};

// The full-width bar used when an application is full-screen.
//
// The reason it exists is a limitation rather than a design choice. A screen edge holds ONE AppBar,
// and the shell's own taskbar is a registered AppBar even when it is hidden: it keeps its claim on
// whatever edge it is docked to, so the desktop work area stays inset by its thickness and a
// maximized window keeps a strip of wallpaper above it that nothing can use. Claiming that edge for
// ourselves is refused for the same reason.
//
// So the bar fills the strip instead. It is full-width and exactly as tall as the strip that is
// already reserved, which means it covers the unusable band rather than fighting for it -- the
// application is still given a work area starting below it, and nothing on screen is wasted.
inline constexpr float kFullBarWidthDip  = 0.0f;   // 0 means "the whole monitor width"
// The FALLBACK height, used only by callers with no monitor to measure: the self-test's hand-built
// anchors, and a window whose monitor query failed. The real height is the band the work area leaves,
// measured per frame and carried on the anchor (see IslandAnchor::bandHeightDip).
inline constexpr float kFullBarHeightDip = 32.0f;
inline constexpr float kFullBarRadiusDip = 6.0f;

// What counts as a measured band: below the minimum there is no band to fill (the work area starts at
// the screen's top edge, which happens when the taskbar is on another edge, and a zero-height bar is
// not a state -- it is an invisible one). Above the maximum nothing sensible is being reported.
inline constexpr float kMinBandHeightDip = 8.0f;
inline constexpr float kMaxBandHeightDip = 128.0f;

// The gap between the bar's underside and the top of a card that hangs BELOW it.
//
// The system panel and the notification are flyouts beside the bar -- the shell's own quick-settings
// flyout opens under the taskbar and never over its buttons. Two DIP of clearance keeps the card's top
// edge from touching the pill and reading as a single shape with it.
inline constexpr float kCardTopGapDip = 8.0f;

const wchar_t* StateName(IslandState state);

// The state a name refers to, for the debug override that pins a shape so it can be looked at.
//
// Unknown names give Collapsed rather than failing: the override is a development tool, and a typo in a
// command line should show the resting bar, not refuse to start.
IslandState StateFromName(const wchar_t* name);

// --- Target geometry -------------------------------------------------
// All in DIPs.
//
// THE ISLAND HAS ONE SHAPE. Collapsed, Hover and MusicStrip are all the same size, and that is a
// decision rather than an omission: "the island does not need to grow sideways any more -- take that
// animation out". Only two states are a different shape, and both are a different KIND of thing:
//
//   * Expanded, which is a card and grows downward;
//   * FullBar, which fills the band a full-screen application leaves.
//
// Everything else is the pill. What changes between those states is the CONTENT -- the clock, or a
// lyric -- and what responds to the pointer is the row of icons beside it. A bar whose outline jitters
// as the pointer crosses it is the thing this removes, and it also removes a whole class of collision:
// a wider island grew symmetrically, so its left edge reached towards the Start button.

inline constexpr float kCollapsedWidthDip  = 300.0f;
inline constexpr float kCollapsedHeightDip = 32.0f;

// Hover is the resting shape with the application icons WIDENED so each one can say what it is.
//
// It is no longer a full-width taskbar. The first version of this grew the island into a
// screen-spanning strip and put the icons inside it, which is not a Dynamic Island and not what was
// asked for: the request was that opening an application adds an icon BESIDE the island, with the
// island staying where it is. So the island keeps its geometry and the row of icons is what
// changes.
//
// The width is the island, the padding, a full row of the WIDE buttons, and a margin -- fixed,
// because a strip that changed size as windows opened and closed would make the whole bar appear to
// jump. TaskStripLayout.h derives the buttons; the two are asserted to agree.
// The pointer being on the bar does NOT change the island's size.
//
// It used to: Hover was 1040 wide, because the first design had no permanent icon row and the island
// had to spread to make room for a taskbar inside itself. Then the icons moved out to sit BESIDE the
// island, and the widening was left behind -- so hovering produced a 1040 DIP pill with a clock at one
// end and 1000 DIP of nothing, with the icons drawn outside it (measured: island ended at 1799, first
// icon started at 1807). Reported, with a screenshot, as "this is what it looks like when the pointer
// is on it".
//
// The row is what responds to the pointer now: the icon under it widens to show its title, and the
// island keeps its shape. So this is the resting size, and the state remains only because the pointer
// being on the bar is still a distinct condition the policy tracks.
inline constexpr float kHoverWidthDip      = kCollapsedWidthDip;
inline constexpr float kHoverHeightDip     = kCollapsedHeightDip;

// The card is as wide as the lyric strip, and that is not a coincidence: opening then moves
// only DOWNWARD.
//
// It was 400, narrower than the 520 strip, so the first ~100 ms of "opening" actually pulled
// the sides IN while the bottom dropped -- a visible inward twitch, reported as a stutter on
// hover entry. A shape that is opening must not get narrower anywhere. Matching the strip's
// width also gives the card the room its text wanted.
//
// 520 x 500: wide enough for the Start page's six 72 DIP columns and for the tray page's grid. The
// system tiles page has its own smaller face below.
inline constexpr float kExpandedWidthDip   = 520.0f;
// 232 DIP, up from the original 132.
//
// The music card is laid out like the Windows 11 media flyout, which needs an app row, an
// 80 DIP cover with two lines of text beside it, a row of times, a progress bar and a row of
// controls. 132 DIP could hold the cover or the text, not both. The window is 240 DIP tall,
// so this still fits inside it with the pill's top margin to spare -- and the window is never
// resized, which is what makes that constraint load-bearing.
// 250 DIP.
//
// Expanded is the motion ENVELOPE. The rendered card face can differ by owner and panel level -- the music
// card and the compact system tiles page each have their own height -- but every transition still runs
// through the same duration, curve and motion state.
//
// The original music card selected this ceiling, and the reason is not obvious. Its content band is a FRACTION of the
// card's height (0.52 to 0.94) rather than a fixed inset from the top, so shrinking the card moves the
// time row UP faster than it moves the artwork -- the band itself gets shorter from the top while the
// 44 DIP cover keeps its size. Measured:
//
//     h 232   artwork [48, 140]   elapsed [144, 160]   fits
//     h 220   artwork [52, 148]   elapsed [138, 154]   overlaps by 10
//     h 212   artwork [56, 156]   elapsed [132, 148]   overlaps by 24
//
// so the reduction to 212 that the system panel wanted was not available. The panel's arithmetic was
// right and its conclusion was wrong: it asked for less height, and the card cannot give it.
//
// The ceiling is the window: the card starts at the pill's margin of 6, so 250 ends at 256 inside a
// never-resized 240 DIP window. THE WINDOW THEREFORE GROWS TO 260, which is the one number here that
// is free -- nothing is drawn below the card, and the window is a transparent strip over the desktop.
//
// 500, not 540: the compact system panel.
//
// The panel keeps three bands -- tiles, the inset volume/brightness surface, and an independent footer --
// but redistributes their heights to fit 500 DIP. See SystemPanelFor for the arithmetic.
inline constexpr float kExpandedHeightDip  = 500.0f;

// The music card is not a settings page. It keeps the compact height it was designed around.
inline constexpr float kMusicCardHeightDip = 280.0f;

// Every page behind the unified Niri side rail uses this face. It is the largest one any page needs,
// so switching Dashboard / applications / tray / system tabs never resizes the card.
inline constexpr float kSystemPanelWidthDip  = 880.0f;
inline constexpr float kSystemPanelHeightDip = 540.0f;

// How much of an outer card (the system panel, a notification) is uncovered, given the height of the
// animated box it is being drawn in.
//
// The box IS the pill at the near end of a transition and the card at the far end, so its height runs
// from the pill's 36 DIP up to the card's 500 -- but the panel is only 500 tall when the box is 500.
// Taking the clip from the box height directly therefore left 36 DIP of the panel on screen at BOTH ends
// of the movement, and the last quarter of the retraction was a band of tile tops hanging under the bar
// that did not move at all and then vanished in a single frame. Reported, with a screenshot of exactly
// that band, as "when I press to put it away it stops for a moment and then disappears".
//
// Zero means nothing of the card is showing, and the caller must SKIP the draw rather than clip to
// nothing: an empty rectangle is not a clip -- D2D fails to create it, no layer is pushed, and the panel
// would be composed at full size, which is the opposite of hiding it.
inline float CardRevealDipForHeight(float boxHeightDip, float cardHeightDip) {
    // No guard for a zero span: both ends are compile-time constants and they are 504 DIP apart, so the
    // division below cannot be by zero -- the compiler said so itself (C4127, "constant conditional
    // expression"), and a branch that cannot be reached is a branch that cannot be tested. The guard was
    // deleted rather than silenced.
    constexpr float span = kExpandedHeightDip - kCollapsedHeightDip;
    float t = (boxHeightDip - kCollapsedHeightDip) / span;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return cardHeightDip * t;
}

inline float OuterCardRevealDip(float boxHeightDip) {
    return CardRevealDipForHeight(boxHeightDip, kExpandedHeightDip);
}

// The lyric strip: the same width as the resting pill, showing a lyric in place of the clock.
//
// It used to be 520 DIP -- "no taller, but a bit wider" -- and that width is what hid the Start button.
// Every state grows symmetrically about the centre, so 150 DIP of extra width reached 150 DIP further
// LEFT, and the button lives 8 DIP from the resting pill's left edge. The strip therefore covered it:
// measured, the strip began at 1020 while the button occupied 1132..1162.
//
// The user's decision settles it in a way that removes the whole class of problem rather than tuning a
// number: "the island does not need to grow sideways any more -- take that animation out". So the
// island's outline is now the SAME SHAPE in every state that is not the card, the full-screen bar, or the
// notice strip (added later, at the user's request, and the only wide shape that is not a card). What
// responds to the pointer is the icon row, and what changes for music is the content, not the outline.
inline constexpr float kMusicStripWidthDip  = kCollapsedWidthDip;
inline constexpr float kMusicStripHeightDip = kCollapsedHeightDip;

// The notice strip: the resting pill's height, and wide enough for a notification to be READ.
//
// 420 DIP is a little under twice the pill and about four fifths of a card. The number is bounded at the
// top by the icon row: the row is measured from the strip's right edge (see RowAnchorBox), so a wider strip
// pushes the row further right, and with seven applications open the row already reaches most of the way to
// the monitor's edge. 420 leaves room for the row on a 2560-wide screen and still shows a full sentence of
// a Chinese title before it has to scroll.
inline constexpr float kNoticeStripWidthDip  = 620.0f;
inline constexpr float kNoticeStripHeightDip = kCollapsedHeightDip;

// Search keeps the resting pill's geometry. Only the contents change, so the
// input field never pushes the island or the controls around it.
inline constexpr float kSearchStripWidthDip = kCollapsedWidthDip;

// The box the application icon row -- and every control measured from the island's edges -- is anchored to.
//
// The RESTING PILL in every state but one, and that is deliberate: the states that share the pill's
// rectangle must not make the row jerk as the pointer moves between them, and a row measured from the CARD
// would swing 150 DIP sideways for the length of every card animation.
//
// The notice strip is the exception, and it is recognised by its SHAPE rather than by its name, because its
// own animation has to be followed: the strip grows 220 -> 420 DIP, so most of its frames are a shape that
// is neither the pill nor the strip, and a test written against the state's NAME would make the fill and
// the row jump at both ends of that growth (see the notice-strip assertions in Diagnostics.cpp).
//
// WHICH SHAPE, exactly -- the height, and only the height. Written first as "wider than the pill, narrower
// than a card, and shorter than half a card", and that rule has a hole big enough to drive the whole bar
// through: A CARD HALF WAY THROUGH ITS OWN GROWTH SATISFIES EVERY CLAUSE OF IT.
//
// The card's box is a lerp from the pill to 560x540, so for the whole first 40% of an opening -- and the
// last 40% of a retraction -- it is wider than the pill, narrower than a card, and under 125 DIP tall. The
// anchor followed it, and with the anchor the island's own FILL, the clock, the deck, the icon row, the
// window region and every hit test: a dark slab grew out of the pill and across the system panel's tiles
// with the time floating in the middle of it, the application icons slid 150 DIP right and back, and the
// anchor snapped home on the frame the box passed 125 DIP. Reported as "现在点击系统按钮会出现这种问题"
// with a screenshot of exactly that slab, and guessed at the time -- correctly -- to be the notice strip's
// doing, because this rule is what the strip added.
//
// The pill's own height is the discriminator: a strip keeps it for the whole of its growth (the strip's
// height IS the pill's -- see SettledSizeFor), while a card's morph leaves it on its first frame. The card
// and the full-screen bar stay excluded, as before, by width and by height.
//
// The trade-off this leaves: a transition that MORPHS between the strip and a card (the panel opening while
// a message is up, or a message arriving while the panel is open) puts the anchor back on the resting pill
// at its first frame, so the fill snaps from 420 wide to 220 instead of lerping. One frame's jump in a rare
// transition, against a growing slab in every card animation.
inline RoundedBox RowAnchorBox(const RoundedBox& current, const RoundedBox& restingPill) {
    const float currentWidth = current.rect.right - current.rect.left;
    const float pillWidth = restingPill.rect.right - restingPill.rect.left;
    const float currentHeight = current.rect.bottom - current.rect.top;
    const float pillHeight = restingPill.rect.bottom - restingPill.rect.top;
    // A full-screen bar can be the same height as the resting pill on machines
    // whose native taskbar is 32 DIP. Its screen-wide width, not its height,
    // distinguishes it from the notice strip.
    if (currentWidth > pillWidth + 0.5f &&
        currentWidth <= kNoticeStripWidthDip + 0.5f &&
        std::fabs(currentHeight - pillHeight) <= 0.5f) {
        return current;
    }
    return restingPill;
}

// Corner radius.
//
// The short states are stadiums: radius = half the height, so the ends are true
// semicircles. Deriving that from the height keeps them perfectly round.
inline float StadiumRadiusForHeight(float heightDip) { return heightDip * 0.5f; }

// The card is NOT a stadium. Half of its 500 DIP height would turn it into a
// lozenge and eat most of its usable width. 24 DIP is the outer radius shared by
// the three Stitch references (系统 / 开始菜单 / 托盘), so every page opens as the
// same rounded glass sheet rather than as a different kind of window.
inline constexpr float kCardRadiusDip = 32.0f;

struct SizeF {
    float width  = 0.0f;
    float height = 0.0f;
    float radius = 0.0f;
};

SizeF StateSize(IslandState state);

// --- The anchor ------------------------------------------------------
//
// The pill is placed by its CENTRE, not its top-left, and the centre is fixed.
// That is what makes the expansion read as the island swelling in place rather
// than unfolding downwards: a top-anchored box grows entirely toward the bottom
// edge, which looks like a menu opening.
//
// The anchor is the pill's TOP EDGE, not its centre, and it is fixed.
//
// A centre anchor would look nicer -- the card would swell symmetrically -- but
// it cannot be made to fit: the resting pill has to sit flush against the
// work-area edge, and the card is 132 DIP tall, so half of it (66 DIP) would have
// to extend above that edge. Above the work-area edge there is only the taskbar
// and then the top of the screen, so the card would be clipped by the display
// itself. Symmetric growth is possible only on a monitor with 66 DIP of unused
// space above the taskbar, which is not something to rely on.
//
// Anchoring the top instead means the card opens downward, the way a menu or a
// notification drops out of the taskbar, and every state fits on screen.
//
// With the numbers in this file the resting pill occupies y = 38..74 and the card
// y = 38..170, both inside the 240 DIP strip on a 32 px taskbar.
struct IslandAnchor {
    float centerX = 0.0f;
    float topY    = 0.0f;

    // The thickness of the band the SHELL's own taskbar claims at the top of the screen, in DIPs.
    //
    // It exists for one state: FullBar, the widened bar that fills the band while a full-screen
    // application runs -- the band is space no application can use (the work area is inset by it) and
    // filling it is the whole point of that state. The number is therefore not a constant of this
    // program but a SETTING of the user's: Settings > Personalisation > Taskbar decides it, and 32 on
    // this machine is a coincidence. It used to be hardcoded, which was measured as wrong the moment
    // anyone could change the setting (HANDOFF section 9's open item, closed in 8.50).
    //
    // Carried on the anchor because the anchor is already where monitor-derived numbers live (its
    // centre comes from the monitor's width), so no layout function needs a new parameter and the
    // geometry stays a pure function of one struct.
    float bandHeightDip = kFullBarHeightDip;

    // Which shape the bar is in (see domain/BarShape.h). Carried HERE for the same reason as the band:
    // the anchor is the one struct every layout function is already given, the shape decides `topY` above,
    // and the window region's resting box reads it back off the same value.
    BarShape shape = BarShape::Capsule;
};

IslandAnchor AnchorForWindow(float originX, float originY, float monitorWidthDip,
                             float bandHeightDip = kFullBarHeightDip,
                             BarShape shape = BarShape::Capsule);

// The pill for a state, as window-local DIPs. The same function the renderer
// draws and the hit test decides with, so the two can never disagree.
RoundedBox BoxForState(IslandState state, const IslandAnchor& anchor);

// The card as the renderer PLACES it: hanging from the RESTING pill's bottom edge, its left edge where
// the frame loop put it, at the card's own size.
//
// Every reader of "where is the card" needs this, and NOT `motion_.Current()` -- which is centred on the
// screen with its top at the PILL's top. Measured on a 2560 monitor: the card is drawn at x 853..1373
// y 50..300, while that box is x 1020..1540 y 6..256 -- 167 DIP to the right and 44 DIP up. The panel's
// tiles were the first thing found hit-tested in the wrong place (HANDOFF 7.81) and the music card's
// transport controls the second (7.86), so the arithmetic lives here now, once.
//
// Replaces a `BoxHangingFrom` that hung a state's box from a left edge and kept the MOTION's top -- which
// is exactly the mistake above, and which nothing called.
inline RoundedBox PlacedCardFor(const RoundedBox& restingPill, float cardLeftDip,
                                float cardWidthDip, float cardHeightDip,
                                bool sharePillTop = false) {
    RoundedBox card;
    card.rect.left = cardLeftDip;
    card.rect.right = cardLeftDip + cardWidthDip;
    card.rect.top = sharePillTop ? restingPill.rect.top
                                 : restingPill.rect.bottom + kCardTopGapDip;
    card.rect.bottom = card.rect.top + cardHeightDip;
    card.radiusX = kCardRadiusDip;
    card.radiusY = kCardRadiusDip;
    return card;
}

inline RoundedBox PlacedCardFor(const RoundedBox& restingPill, float cardLeftDip,
                                float cardHeightDip = kExpandedHeightDip) {
    return PlacedCardFor(restingPill, cardLeftDip, kExpandedWidthDip, cardHeightDip);
}

// The COLUMN a card hangs in: from the bar's underside down to the card's bottom, as wide as the card.
//
// This is the territory the pointer owns while a card is up. It is not the same rectangle as the card, and
// the difference is the 8 DIP gap between the bar's underside and the card's top edge (kCardTopGapDip).
//
// That gap is exactly what a first attempt at this missed. The rules were written as "on the pill" or "on the
// card", and the pointer travelling from one to the other is on NEITHER for those 8 DIP -- so the card was
// given up mid-journey. The test that passed jumped straight from one to the other and never crossed it,
// which is why the bar still failed after the assertion went green.
inline RectF CardColumnFor(const RoundedBox& restingPill, float cardLeftDip,
                           float cardWidthDip, float cardHeightDip,
                           bool sharePillTop = false) {
    const RoundedBox card =
        PlacedCardFor(restingPill, cardLeftDip, cardWidthDip, cardHeightDip, sharePillTop);
    RectF column;
    column.left = card.rect.left;
    column.right = card.rect.right;
    column.top = sharePillTop ? restingPill.rect.top : restingPill.rect.bottom;
    column.bottom = card.rect.bottom;
    return column;
}

inline RectF CardColumnFor(const RoundedBox& restingPill, float cardLeftDip,
                           float cardHeightDip = kExpandedHeightDip) {
    return CardColumnFor(restingPill, cardLeftDip, kExpandedWidthDip, cardHeightDip);
}

// Whether two shapes are the same rectangle. Used to recognise a state change that has no
// distance to cover, so no animation is started for it.
bool SameBox(const RoundedBox& a, const RoundedBox& b);

// --- Timing ----------------------------------------------------------

struct MotionTiming {
    float collapseToHover = 0.22f;   // spreading out: unhurried
    float hoverToCollapse = 0.16f;   // snapping back: must feel immediate
    // Measured: with the card at 36 -> 250 DIP and the ease-in-out curve, a 0.26 s expansion spends
    // its first tenth covering almost nothing and its last tenth the same, so the panel reads as
    // arriving suddenly rather than sliding. Reported as "about a second with no reaction, then it
    // appears" -- there is no delay (the loop draws the first frame in the same millisecond the flag
    // flips, measured), the motion is simply too quick to follow. 0.36 s gives the slide room to be
    // seen while staying inside the range a flyout is expected to take.
    float toExpanded      = 0.36f;   // opening a card
    float fromExpanded    = 0.24f;   // closing it: quicker, because putting something away should feel
                                     // decisive rather than slow
};

// Easing curves, named for what they do rather than for a number.
//
// EaseOutCubic is the one the motion uses, for every pair of states; see the comment on CurveFor for
// the measurements that settled it.
//
// EaseInOutCubic is kept, and asserted by the self-test, as the documented member of the S-curve family
// this project moved away from -- it is what "settles at both ends, and lurches in between" means, and
// the comparison is the reason the ease-out is there.
float EaseOutCubic(float t);     // decelerating: starts fast, settles
float EaseInOutCubic(float t);   // symmetric: still at both ends, fastest in the middle

float         TransitionSeconds(IslandState from, IslandState to, const MotionTiming& timing);
float       (*CurveFor(IslandState from, IslandState to))(float);

// --- Interpolation ---------------------------------------------------

// Blend of two boxes. The corner radius is recomputed from the interpolated
// height rather than blended, so the shape stays a valid stadium throughout.
RoundedBox LerpBox(const RoundedBox& a, const RoundedBox& b, float t);

// The state machine. It owns no timing source: the caller feeds it elapsed
// seconds, which is what lets the self-test step it deterministically instead of
// sleeping and hoping.
class Motion {
public:
    Motion() = default;

    IslandState Target() const { return target_; }

    // The shape the motion is coming FROM -- while a transition is in flight. Once it has settled this is
    // the shape it is IN, so a caller's "is a card involved?" test (`Target() == Expanded ||
    // FromState() == Expanded`) is true for the whole of a card's arrival and departure and false
    // afterwards.
    //
    // Reporting the stored `fromState_` unconditionally made that test true FOR EVER after the first
    // card: the box stayed pinned to a button, the window region stayed card-sized, and -- once the
    // panel's own lifetime was latched to this same test -- a 36 DIP slice of the panel was left drawn
    // under the bar after the panel had been put away. Reported with a screenshot: a strip of tile tops
    // left hanging below the island.
    //
    // A question about what is on screen has to be able to become false again.
    IslandState FromState() const { return animating_ ? fromState_ : target_; }
    bool IsAnimating() const { return animating_; }
    float Progress() const { return progress_; }

    // The transition's progress through its own curve, 0..1. The shape is drawn at this value, so
    // anything that has to stay in step with the shape -- the content's opacity, for one -- wants this
    // rather than the linear progress.
    float Eased() const { return CurveFor(fromState_, target_)(progress_); }

    RoundedBox Current() const { return current_; }

    // Multiplies every transition's duration by `factor`.
    //
    // For diagnosing an animation that is over too fast to read: a transition that is 0.36 s long is
    // ~22 frames at 60 Hz, which is not enough frames to tell a smooth slide from a stutter in a still
    // capture. It scales the DURATIONS and not the frame interval on purpose -- `dt` is the quantity
    // being observed, so scaling it would change the thing under measurement.
    void ScaleDurations(float factor);

    // Sets the layout context. Changing it (a DPI change, a monitor switch)
    // re-derives the current box without disturbing the state.
    void SetAnchor(const IslandAnchor& anchor);

    // Opening a card first grows it sideways, then pulls the height down. The
    // reverse transition retracts height first and width last. Off by default so
    // the legacy themes and their existing motion assertions are unchanged.
    void SetStagedExpansion(bool enabled) { stagedExpansion_ = enabled; }

    // Requests a state. Returns true only if this actually started a
    // transition: asking for the state already targeted is a no-op, which
    // matters because the hover check runs many times a second. Re-targeting
    // mid-flight is allowed and continues from wherever the shape currently is,
    // so reversing direction never jumps.
    bool SetTarget(IslandState state);

    // Advances by dtSeconds. Returns true while the caller should keep
    // redrawing; false once settled, so an idle island stops consuming frames.
    bool Tick(float dtSeconds);

    // Jumps straight to a state with no animation. Used at startup and by the
    // self-test.
    void SnapTo(IslandState state);

private:
    IslandState target_    = IslandState::Collapsed;
    // The state the current transition started from. Kept because the easing
    // curve and duration are properties of the pair, not of the destination
    // alone: collapsing and spreading use different curves.
    IslandState fromState_ = IslandState::Collapsed;

    RoundedBox  current_{};

    RoundedBox from_{};
    RoundedBox to_{};

    float elapsed_   = 0.0f;
    float duration_  = 0.0f;
    float progress_  = 1.0f;
    bool  animating_ = false;

    IslandAnchor anchor_{};
    bool hasAnchor_ = false;

    MotionTiming timing_{};
    bool stagedExpansion_ = false;
};

} // namespace island
