// =====================================================================
//  Renderer.h
//  Direct3D 11 + Direct2D + DirectComposition surface for the bar.
//
//  Ownership: this object owns the swap chain, the composition target and the
//  composition visual. It does NOT own the window, and it does not know about
//  Win32 messages -- the window tells it when to draw and at what size.
//
//  Every COM pointer is released exactly once, in Shutdown, which runs from the
//  destructor. An earlier toolchain revision crashed with 0xC0000005 because a
//  DXGI device was released and then still handed to DCompositionCreateDevice;
//  the rule that came out of it is that a pointer is freed only at the end of
//  the scope that owns it and never before its last use.
// =====================================================================
#pragma once

#include <windows.h>

#include <d2d1_3.h>
#include <string>
#include <d3d11_4.h>
#include <dcomp.h>
#include <dxgi1_6.h>

#include "domain/Geometry.h"
#include "domain/TaskStripLayout.h"   // the island plus the row of application icons
#include "domain/Motion.h"
#include "media/MusicSession.h"
#include "platform/BackdropCapture.h"
#include "services/Notification.h"
#include "render/LensMaterial.h"
#include "render/TextRenderer.h"

namespace island {

// Who owns the card this frame, given what is asking for one and whether the shape is coming or going.
//
// `cardUp` says the shape is a card or on its way to being one; `cardWanted` says the motion is still
// heading to the card (or settled on it) -- a different question from where the shape is now. `current`
// is the owner from the previous frame.
//
// Two halves, and the second is the point:
//
//   * While the card is WANTED the owner is chosen afresh, highest first: the panel (opened
//     deliberately), then a notification (transient -- music is still there a second later), then what is
//     playing. That is the precedence the draw order has always had.
//
//   * While the card is LEAVING the owner does not change. The card outlives the reason it was opened by
//     exactly the length of its exit, and a card that is not drawn during its own exit disappears inside a
//     single frame. Reported for the panel, and true of the other two cards as well.
inline CardOwner NextCardOwner(CardOwner current, bool cardUp, bool cardWanted,
                               bool panelRequested, bool notificationActive, bool musicPresent) {
    if (!cardUp) return CardOwner::None;
    if (!cardWanted) return current;
    if (panelRequested) return CardOwner::SystemPanel;
    if (notificationActive) return CardOwner::Notification;
    if (musicPresent) return CardOwner::Music;
    return current;
}

// What the pill should look like this frame. Kept apart from the drawing code so
// the motion code only has to produce one of these.
struct PillAppearance {
    RoundedBox box;
    D2D1_COLOR_F fill{0.05f, 0.05f, 0.07f, 0.92f};
    float opacity = 1.0f;
};

// One frame: the shape, what is written inside it, and how strongly.
//
// The two strengths content is drawn at while a card is on its way in or out.
//
// TWO NUMBERS, AND THEY ARE NOT THE SAME QUESTION.
//
// `bar` is the island's OWN content: the deck buttons, the application icon row, the clock, and the
// lyric / notice / call line. It does not fade when a card opens -- the card grows BELOW the bar and
// the bar's own rectangle never changes, so there is nothing about it that is half-formed.
//
// `card` is the card's own content AND its fill, faded in with the reveal so the card does not appear
// at full contrast inside an outline that is still growing.
//
// They used to be one number, and the bar blinked on every card transition: with music playing, moving
// the pointer onto the island opens the music card, and the deck, the icon row and the lyric line all
// dimmed to 35% and ramped back over 0.34 s. Reported as "点击系统按钮 或者 播放音乐的时候 鼠标移入灵动岛
// 时候 图片中红框里的会闪一下" with the whole bar circled. The loop's own log named it -- `start
// button: ... opacity 0.43` climbing to 1.00 while the island's geometry never moved a pixel.
struct ContentStrengths {
    float bar = 1.0f;
    float card = 1.0f;
};

// The card's strength, SEQUENCED against the shape rather than moving with it.
//
// The brief this answers, in its own words: "expanding: the pill opens smoothly and the content fades
// in afterwards; collapsing: the content fades out first and the body then shrinks back". Following the
// shape exactly -- which is what this did -- makes the two one object: the words slide out with the
// edge, so on the way open they are already legible inside a card that has not finished making room for
// them, and on the way closed they leave together with the border they were sitting in.
//
// `opening` is which of the two this transition is, and it CANNOT be derived from `easedProgress`: both
// directions run their progress from 0 to 1 (the shape interpolates from `from_` to `to_`, and which of
// those is the card is the caller's knowledge, not this function's).
//
// The lead is a third of the movement: on the way open the card's content stays at its floor while the
// shape takes a third of the room it needs, then fades in over the rest; on the way closed it is gone
// before the shape has finished a third of the way back. Both ends are exact -- 0.35 at the start of an
// opening and 1.0 at its end, and the mirror of that for a closing -- so the settled values are what the
// rest of the renderer has always assumed.
inline constexpr float kCardContentLeadFraction = 0.35f;

inline ContentStrengths ContentStrengthsFor(bool cardUp, float easedProgress, bool opening) {
    ContentStrengths out;
    if (!cardUp) return out;

    float t = easedProgress;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    float sequenced = opening ? (t - kCardContentLeadFraction) / (1.0f - kCardContentLeadFraction)
                              : 1.0f - t / kCardContentLeadFraction;
    if (sequenced < 0.0f) sequenced = 0.0f;
    if (sequenced > 1.0f) sequenced = 1.0f;

    out.card = 0.35f + 0.65f * sequenced;
    return out;
}

struct FrameState {
    PillAppearance pill;

    // Where the pointer is ACROSS THE PILL, 0 = its left edge, 1 = its right. The specular sweep of the
    // liquid-glass material follows it (round 53): light travelling along the glass as the pointer moves
    // is the one part of Apple's material that is about the pointer rather than about the background.
    // Clamped by SpecularCentreX, so a pointer outside the pill leaves the highlight on the glass.
    float pillPointerFraction = 0.5f;

    // Whether the pointer is actually ON the island. The pointer-following specular is drawn only then:
    // with the pointer elsewhere the bar must look exactly as it does at rest, which is the difference
    // between "light on the glass" and "a decoration that never stops moving".
    bool pointerOnIsland = false;

    // The RESTING pill's box, which is not the same thing as `pill.box`.
    //
    // `pill.box` is the ANIMATED shape: while the bar is opening or closing a card it is a card-sized
    // rectangle. The pill's own fill must use the RESTING box instead, because the card is drawn by its
    // own code and the pill is drawn by the pill code -- and filling a card-sized rounded rectangle with
    // the pill's near-opaque black painted a dark band across the screen for the 200 ms of every
    // transition.
    //
    // It was reported as "a box under the island that should not be there", and four rounds were spent
    // on other explanations (a second process, another window, a clipped region) because the fill code
    // was never the suspect: it is the one draw that had always worked, in the state where it had always
    // been correct.
    RoundedBox restingPill;

    // Whether an outer card -- the system panel or a notification -- is on screen or on its way there.
    //
    // NOT the same question as `systemPanelOpen`, and using that flag instead was two separate bugs:
    //
    //   * While the panel was CLOSING, `systemPanelOpen` was already false, so the card stopped being
    //     pinned to its button and started following the island -- which tracks the pointer, so the
    //     panel visibly drifted with the mouse as it disappeared.
    //   * It also made the card's vertical placement flip mid-transition, so the shape being drawn and
    //     the shape the window's region was set from disagreed for the length of the animation.
    //
    // Set from the MOTION -- is the current shape or the target shape a card? -- so it stays true for
    // the whole of both the opening and the closing animation.
    bool outerCardUp = false;

    // True while a card is being animated or is answering a moving pointer.
    //
    // The card's surface is then drawn WITHOUT its shadow and without its grain: those are the two parts
    // nobody can see while it moves and the two that cost the most (a 22 DIP blur and a tiled noise fill).
    // Everything that makes the surface the surface -- fill, hairline edge, lighting -- is kept, so a moving
    // card and a settled one are the same material. Set from the frame loop; the full material comes back on
    // the first frame that is not moving.
    bool cardSurfaceMoving = false;

    // Whether a FULL-SCREEN APPLICATION is in front of the bar: the bar is not part of the picture then,
    // except for the island itself.
    //
    // Set by the frame loop, which measures it (see island::FullScreenAppInFront). What it suppresses:
    // the deck, the application icon row and the date-and-time line -- the "left and right sides", in the
    // words of the report: "玩游戏的时候 有通知和通话时 只要显示灵动岛就行 左右两侧的不用显示". A call or a
    // message is still the island, in the band, because that is what the state says and this flag does not
    // touch it.
    bool fullscreenApp = false;

    // WHICH card owns the shape this frame -- held from the moment a card is asked for until the shape
    // has finished returning to the bar.
    //
    // The card the bar draws is one card, and three things can ask for it: the system panel, a
    // notification, or what is playing. The question the renderer needs answered is "who owns it right
    // now", and it is NOT `state == Expanded` -- the state is the TARGET, so on the way out it is already
    // the resting shape and the card was not drawn at all for the whole of its own retraction: it
    // disappeared inside a single frame. Reported that way for the panel, and the same was true of the
    // other two cards.
    //
    // See NextCardOwner for the rule, which is a pure function so it can be asserted.
    //
    // `Notification` IS NO LONGER PRODUCED: the notification card was removed on request ("这个卡片不需要"),
    // so a message is the notice strip whatever the pointer does (see NextShape). The value keeps its
    // number -- the frame's own logging prints it -- but nothing on the bar can ask for that card any more,
    // which also means the two things that only existed for it are unreachable: the renderer's
    // CardOwner::Notification case (TextRenderer::DrawNotification) and the sender's icon
    // (NotificationIcon). Both are still covered by the self-test; delete them, with their tests, in the
    // pass that removes this value.
    CardOwner cardOwner = CardOwner::None;
    IslandState    state = IslandState::Collapsed;

    // What the bar shows when nothing is being asked of it: Collapsed, or MusicStrip while something is
    // playing. NOT the same question as `state`, which is the TARGET and is therefore Expanded for the
    // whole of a card.
    //
    // This exists because the pill's one line has to be chosen by what the bar IS, not by what it is
    // currently growing into: pressing the system button with music playing replaced the lyric with the
    // clock, for a panel that has nothing to do with the music. See the pill's text rule in Renderer.cpp.
    IslandState    restingState = IslandState::Collapsed;

    ClockStyle     clock = ClockStyle::Compact;

    // --- the date, the weekday, the time and the weather ----
    //
    // One line with two possible homes, and the island comes first: at rest it is drawn INSIDE the
    // pill (where the clock has always been), and while the pill's one line is taken -- by the lyric,
    // a message, a call -- the same words are drawn in the band left of the deck instead. See
    // StatusPlacementFor, which is the rule, and StatusStripFor, which is where the band is.
    //
    // `statusStripOpacity` is the strip's own fade, 0 at rest and 1 while the words are beside the
    // deck. The pill's copy needs no fade: it is the pill's own content, and the pill is always
    // there.
    //
    // The reading, in the two pieces the line draws separately: the word ("小雨") and the
    // temperature ("17°"). Either may be null, and both are null in the ordinary case -- before the
    // first reading arrives, and when neither the machine nor weather.txt can say where it is. The
    // line is then the weekday, the date and the time alone.
    //
    // Two fields rather than one string because the reference sets them apart: the glyph stands
    // before the word, the word and the temperature are separated by a gap, and a single joined
    // string could not be laid out that way.
    //
    // Storage belongs to the caller and must live as long as the frame (see main.cpp's buffers).
    const wchar_t* statusSky = nullptr;
    // The weather code the sky word and its mark BOTH come from: the line draws a mark per sky (see
    // DrawSkyMark), and the two must be answers to the same question. -1 means no reading.
    int statusCode = -1;
    const wchar_t* statusTemperature = nullptr;
    float          statusStripOpacity = 0.0f;

    // 0..1 strength of the BAR's own content: the deck, the icon row, the clock, the lyric / notice /
    // call line. 1.0 unless something deliberately holds the bar back -- see ContentStrengthsFor, which
    // is the one place that answers "how strong is anything this frame" for both halves.
    float          contentOpacity = 1.0f;
    // 0..1 strength of the CARD's own content and fill, faded in with its reveal.
    float          cardOpacity = 1.0f;
    // 0..1 progress of a lyric line CHANGE: 1 once it has settled. The pill's line ROLLS -- the new line
    // rises into it from below while the old one climbs out through the top, both by one line height -- so
    // a change reads as the words arriving rather than as a jump. Two half rows are on screen for the length
    // of the roll, which is what the roll IS. See LyricLineRollFor.
    float          lyricLineRoll = 1.0f;
    // The line that was on screen before the change, which is the half of the roll that is leaving.
    // Points at storage the CALLER owns and keeps valid for the frame (the provider's own text would
    // do, but a track change replaces that store, so the loop keeps a copy).
    const char*    lyricLinePrev = nullptr;
    // The leaving line's own marquee offset, so a long line does not jump sideways as it leaves.
    float          lyricLinePrevScrollDip = 0.0f;
    // The audio wave behind the pill's line: the output's peak reading, how far its shape has drifted,
    // and the colour taken from the cover. See TextRenderer::DrawAudioWave.
    float          musicLevel = 0.0f;
    bool           musicLevelKnown = false;
    float          musicWavePhase = 0.0f;
    D2D1_COLOR_F   musicAccent{0.40f, 0.80f, 0.95f, 1.0f};

    // The notification to show, if any. Not owned; null means "just the clock".
    const Notification* notification = nullptr;

    // The notification sender's own icon, as decoded BGRA premultiplied pixels (the same form the album art
    // is passed in). Optional: a sender whose process could not be read simply gets no icon, and the card
    // draws the text alone.
    //
    // `notificationIconRevision` changes when the icon does, and the text renderer keys its uploaded bitmap
    // on it -- decoding and uploading per frame would be work for an image that changes once per message.
    const uint8_t* notificationIconPixels = nullptr;
    int            notificationIconWidth = 0;
    int            notificationIconHeight = 0;
    unsigned long long notificationIconRevision = 0;

    // What is playing, if anything. Not owned; null means "show nothing".
    //
    // A notification takes precedence over this when both are present: the card has
    // room for one of them, and a notification is the transient one -- music is
    // still there a second later, a notification is not.
    const MusicInfo* music = nullptr;

    // The transport glyph to draw beside the artist ("||" or ">"), chosen by the
    // caller so this header stays free of playback policy.
    const char* transportGlyph = nullptr;

    // 0..1 playback position, or <= 0 when the duration is unknown.
    float progressFraction = 0.0f;

    // The playback position to print, in milliseconds. Separate from the music snapshot so
    // the caller can project it forward between polls and keep the time and the bar in step.
    long long musicPositionMs = 0;

    // The total length to print, in milliseconds, when the caller has one and the snapshot does not.
    //
    // A session is not obliged to publish a timeline and NetEase Cloud Music never does, so the card
    // had no total (`00:00`) and its bar could not fill. The lyric provider reads the length from the
    // catalogue it just searched (see Lyrics::trackDurationMs), and this is where it reaches the
    // drawing code. Zero means "nobody knows", and the card prints `--:--`.
    long long musicDurationMs = 0;

    // Decoded album art, BGRA premultiplied. Owned by the caller; null when there
    // is none. Passed as pixels rather than as a bitmap so the renderer does not
    // have to own a cache or a lifetime.
    const uint8_t* artworkPixels = nullptr;
    int            artworkWidth = 0;
    int            artworkHeight = 0;

    // The blurred artwork used as the card's background wash, decoded from the PNG
    // WicImage's MakeBlurredBackgroundPng produced. Optional: without it the card
    // falls back to the plain fill.
    const uint8_t* backgroundPixels = nullptr;
    int            backgroundWidth = 0;
    int            backgroundHeight = 0;

    // Change counters for the two images above. The renderer caches their GPU bitmaps
    // keyed on these, so an unchanged image is uploaded once instead of every frame.
    unsigned long long artworkRevision = 0;
    unsigned long long backgroundRevision = 0;

    // --- the taskbar: the row of window buttons in the hover state ---
    //
    // The hover state exists to hold these, so this is what the bar is for rather than an extra.
    // Passed as a plain array plus a count: the renderer must draw exactly as many buttons as the
    // click handler will hit-test, and a count the renderer silently adjusted would break that.
    // The row's cap, owned by the layout (see domain/TaskStripLayout.h for why it moved there). Kept as a
    // member so every `FrameState::kMaxTaskButtons` in the renderer and in main.cpp reads the same as before.
    static constexpr int kMaxTaskButtons = island::kMaxTaskButtons;
    TextRenderer::TaskButton taskButtons[kMaxTaskButtons] = {};
    int taskButtonCount = 0;
    // Each icon's appearance: 0 while it is arriving, 1 when it is settled, and back down while it is
    // leaving. `taskButtonCount` is the UNION of the listed windows and the ones still on their way out
    // (listed first, in the list's order, then the departing ones), so a departure can be drawn.
    //
    // `taskButtonAppearValid` because a zero-initialised array means "every icon is invisible": a frame
    // that has not filled it -- every test frame, and the previews -- would draw an empty row. Unset is
    // therefore "all settled", which is what those frames meant before this field existed.
    float taskButtonAppear[kMaxTaskButtons] = {};
    bool  taskButtonAppearValid = false;

    // The system indicators used to be here, as a second range drawn at the end of the same row. They
    // were removed from the bar -- the left deck holds the Start and system buttons and nothing else --
    // and their slots, their count and their hover amounts went with them rather than being left as
    // fields nothing reads.

    // The icon button under the pointer, or -1. Only this one widens and shows its title; hovering
    // one icon must not widen the whole row. Chosen by the window, which is the only place that
    // knows where the pointer is.
    int taskHoveredIndex = -1;

    // THE SYSTEM TRAY: the applications' own tray icons, at the right-hand end of the bar.
    //
    // The list is owned by the frame loop (it reads the registry on a timer) and borrowed here; the count
    // is what the loop was told fits, and `trayRevision` is the list's own so the decoded bitmaps in the
    // text layer survive from frame to frame. See TrayIcons.h for where the icons come from and
    // TaskStripLayout.h for why the group is anchored to the monitor rather than to the row beside it.
    const TrayIcon* trayIcons = nullptr;
    int             trayIconCount = 0;
    unsigned long long trayRevision = 0;
    // Which cell of the card's grid the pointer is on, and whether the button at the right-hand end is
    // under it. Both are the window's answers, taken from the geometry in TrayPageFor/TrayButtonFor.
    int             trayHoveredIndex = -1;
    bool            trayButtonHovered = false;
    // How far the wash under it has come up, 0 to 1. The BOOL above says whether the pointer is there;
    // this is the same answer with a spring on it, because a 16% white wash that appears in one frame is
    // the one thing on the bar that reacts instantly -- and "reacts instantly" reads as "hard", not as
    // "responsive". Same arrangement for the Start button and the system panel's toggle below.
    float           trayButtonWash = 0.0f;
    // Whether the card is showing the tray page rather than the panel's tiles. The same card, the same
    // place, the same animation -- only the contents change (see TrayPageFor).
    bool            trayPageOpen = false;
    // The page's own heading. Supplied by the caller, because this class holds no user-facing strings of its
    // own (see SoundPageText, which is here for the same reason). Empty means no heading.
    const wchar_t*  trayPageTitle = nullptr;

    // THE TRAY'S OWN CONTEXT MENU (CardLayout.h): the bar's answer to a right click on a cell, instead of
    // the shell's panel and the application's menu at the far corner. Opened data, like everything else
    // here -- where its rows are, which one is under the pointer, and what each one says.
    bool            trayMenuOpen = false;
    RectF           trayMenuRows[kMaxTrayMenuEntries] = {};
    int             trayMenuRowCount = 0;
    int             trayMenuHovered = -1;
    const wchar_t*  trayMenuLabels[kMaxTrayMenuEntries] = {};

    // --- the Start card -------------------------------------------------------------------------------
    //
    // The same card, one level further: the user's own pins, the shell's full list behind the picker, and
    // the footer's two things. See TextRenderer::StartCardText, which this fills in one place so the
    // drawing and the hit tests cannot be given different answers.
    TextRenderer::StartCardText startCard = {};
    bool startPageOpen = false;

    // Whether the pointer is on the Start button, which lives on the other side of the
    // island from the application icons.
    bool startHovered = false;
    float startWash = 0.0f;

    // The system panel: whether its toggle is hovered, whether the panel is open, and what it says.
    //
    // The card is the SAME state as the music player, so these are what decide which of the two things
    // the card is showing. A separate IslandState would have meant a second card geometry, a second set
    // of policy rules and a second transition, all for a shape that is already drawn.
    bool systemButtonHovered = false;
    float systemButtonWash = 0.0f;
    // Whether that button -- and the Start button, and the tray button -- should LOOK open. One of them at
    // most, decided by the level the card is showing (see SystemButtonShowsOpen and the two beside it),
    // which is NOT the same question as "is a card up": `systemPanelOpen` is true for every level, so the
    // system button's chevron used to point up whichever control had opened the card.
    bool systemButtonOpen = false;
    bool startButtonOpen = false;
    bool systemPanelOpen = false;
    TextRenderer::SystemPanelText systemPanel = {};

    // The panel's SECOND level: the 声音输出 page, reached from the output-device button at the right of
    // the volume row.
    //
    // A flag and a payload rather than a CardOwner of its own, because it is not a different card: the
    // shell's own flyout keeps its size, its place and its frame and simply shows a different level, and
    // every rule that holds the panel open -- the motion, the region, the pointer policy -- applies to it
    // unchanged. `systemPanelOpen` stays true while this is up; that is what "one level down" means here.
    bool soundPageOpen = false;
    TextRenderer::SoundPageText soundPage = {};

    // Where the CARD's left edge goes, or 0 for "leave it centred".
    //
    // A card opens from the control that was pressed, the way a flyout does; a panel that appears in
    // the middle of the screen when the button is at the far left reads as unrelated to the click.
    // The value is computed by the caller because the anchor is the caller's knowledge -- the window's
    // layout is asked for the control's rectangle, and the renderer never sees it.
    float cardLeftDip = 0.0f;

    // The window each button stands for, so the renderer can cache that window's icon.
    //
    // Cached by WINDOW, not by process. Fetching through the process needs OpenProcess and
    // QueryFullProcessImageName to succeed, and both are refused for a protected process -- which
    // left such windows showing a generic folder icon. The window's own icon does not need those
    // permissions, so it is both more reliable and the icon the window actually chose.
    HWND taskButtonWindow[kMaxTaskButtons] = {};

    // The lyric line to show in the resting strip, and how far it has scrolled. When
    // this is null the renderer falls back to the track name, and then to the clock.

    // The source application's icon, decoded from the PNG MusicSession extracted from its
    // executable.
    const uint8_t* iconPixels = nullptr;
    int            iconWidth = 0;
    int            iconHeight = 0;
    unsigned long long iconRevision = 0;

    const char* lyricLine = nullptr;
    const char* fallbackLine = nullptr;
    // The line the NOTICE STRIP shows: the sender and the message, in the one line the strip has room for.
    //
    // A separate field rather than a reuse of `lyricLine`, because the two say different things and the
    // renderer chooses between them by STATE: a lyric strip drawing a notification's text (or the reverse)
    // would be a confused bar rather than a missing feature.
    const char* noticeLine = nullptr;

    // ... and whether that strip is showing a CALL rather than a message.
    //
    // A call gets the strip's shape and its own drawing -- an avatar, a green running timer, and a level meter
    // -- because that is what was asked for ("微信通话应该做成这样的", with the iOS Dynamic Island as the
    // reference). The shape is the same, so nothing about the geometry, the region or the decks changes.
    bool callUp = false;
    TextRenderer::CallStripText callStrip = {};
    float       lyricScrollDip = 0.0f;
};

class Renderer {
public:
    Renderer() = default;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Builds the whole chain: D3D11 device -> DXGI swap chain -> D2D device
    // context -> DComposition device/target/visual.
    //
    // `swapChainSize` is in physical pixels and must match the window's client
    // area, because the swap chain is never resized afterwards.
    // `scale` maps DIPs to those pixels and goes on the composition visual, so
    // rendering code can stay entirely in DIPs.
    bool Initialize(HWND hwnd, UINT widthPx, UINT heightPx, float scale);

    // Points the composition target at the swap chain and commits. Call after
    // Initialize and after every DpiChanged.
    bool BindToWindow(HWND hwnd, float scale);

    // Clears to fully transparent and draws the frame: the pill, then whatever
    // the frame says belongs inside it.
    //
    // The clear matters: the window is larger than the pill, and every pixel
    // outside it must stay invisible for the pass-through regions to look like
    // nothing is here.
    void Render(const FrameState& frame);

    // True when the capture has a backdrop the lens has not drawn yet: the loop redraws then, and only
    // then, so a still desktop keeps the bar idle.
    bool TakeLensFrame();

    // Convenience for paths that only care about a shape with no content.
    void Render(const PillAppearance& pill) {
        FrameState frame;
        frame.pill = pill;
        frame.contentOpacity = 0.0f;   // no text: the shape alone
        Render(frame);
    }

    // Starts a GPU-driven opacity animation on the composition effect group.
    //
    // Opacity is the one property worth animating on the compositor rather than
    // per frame: it needs no geometry, so the GPU can interpolate it even while
    // the CPU is busy. Geometry (width, height, corner radius) still has to be
    // interpolated per frame, because the size of a rounded rectangle is not
    // something the compositor can animate.
    //
    // Returns false when the device could not supply an animation, in which case
    // the caller should keep setting opacity per frame.
    bool AnimateOpacity(float from, float to, double seconds);

    // Text. Not owned and may be null, in which case no text is drawn.
    void SetTextRenderer(TextRenderer* text) { text_ = text; }

    // How long the last frame's drawing took, in microseconds, plus the average over
    // every frame drawn so far.
    //
    // Kept because rendering cost is otherwise invisible: "the card feels laggy" is not
    // falsifiable, while "one card frame costs 900 us" is, and a fix can be shown to move
    // that number. The card's bitmaps used to be rebuilt every frame -- about 150 KB of
    // pixels re-uploaded per frame for a picture that had not changed.
    double LastFrameMicroseconds() const { return lastFrameMicroseconds_; }
    double AverageFrameMicroseconds() const {
        return frameCount_ > 0 ? frameMicrosecondsTotal_ / static_cast<double>(frameCount_)
                               : 0.0;
    }
    long long FrameCount() const { return frameCount_; }
    void ResetFrameStats() {
        lastFrameMicroseconds_ = 0.0;
        frameMicrosecondsTotal_ = 0.0;
        frameCount_ = 0;
    }

    // Blocks until the compositor is ready for the next frame, or the timeout
    // expires. Returns true when a frame tick actually arrived.
    //
    // This replaces the old Sleep(16) pacing: the bar wakes when the compositor
    // ticks instead of polling a timer, which costs nothing while the desktop is
    // idle. Returns false on builds that predate the API so the caller can fall
    // back to a timed loop.
    bool WaitForCompositorClock(DWORD timeoutMs);

    // Reads the last rendered frame back from the GPU and reports what is
    // actually in it: the alpha at a point inside the pill, and the alpha at a
    // point outside it.
    //
    // This exists because "Present returned S_OK" and "the pill is on screen"
    // are different claims. A composition surface can be built, committed and
    // presented successfully while the compositor shows nothing, and no HRESULT
    // anywhere reports that. Reading the pixels separates the two failures --
    // nothing was drawn, versus something was drawn but not composited -- which
    // is otherwise indistinguishable from the outside.
    bool ReadBackFrame(float insideXDip, float insideYDip,
                       float outsideXDip, float outsideYDip,
                       BYTE* insideAlpha, BYTE* outsideAlpha,
                       BYTE* insideRgb);

    // Same read, but reports which step failed and with what HRESULT. The plain
    // ReadBackFrame answers only "did it work", which makes a failure impossible
    // to act on: the last time this broke, every step returned S_OK and the only
    // visible symptom was a false return.
    bool ReadBackFrameVerbose(float insideXDip, float insideYDip,
                              float outsideXDip, float outsideYDip,
                              BYTE* insideAlpha, BYTE* outsideAlpha, BYTE* insideRgb,
                              wchar_t* step, size_t stepCapacity);

    // Mean luminance of a rectangle of the last rendered frame, 0..255.
    //
    // This is how "did the text actually get drawn" is answered numerically. The
    // card is a near-black fill with light text, so a region containing glyphs is
    // measurably brighter than the same region without them -- which distinguishes
    // "the text is there" from "the draw call returned S_OK and produced nothing",
    // the failure mode that has already bitten this project once.
    //
    // Returns false when the rectangle is empty or falls outside the surface.
    bool RegionLuminance(float leftDip, float topDip, float rightDip, float bottomDip,
                         float* outLuminance);

    // Whether the cached bitmap for this slot was built from these exact pixels.
    //
    // Exposed for the self-test, which asks the question the cache got wrong: two DIFFERENT images carrying
    // the SAME revision must not be served the same bitmap ("歌曲切换了 但是封面图没有换"). The media session's
    // artwork revision used to reset to 1 whenever a track published no cover, so the next track's cover
    // carried the same number as the previous one and the card drew the previous album art while the log said
    // the new cover had been decoded. The provider's counter is monotonic now, and this is the assertion that
    // keeps the cache honest either way.
    bool CachedArtworkIs(const uint8_t* pixels, int width, int height,
                         unsigned long long revision) const {
        const BitmapCache& cache = bitmapCache_[static_cast<int>(BitmapSlot::Artwork)];
        return cache.bitmap != nullptr && cache.revision == revision && cache.width == width &&
               cache.height == height && cache.pixels == pixels;
    }

    // Renders an ASCII map of a region's alpha, for diagnosing "is anything drawn
    // here". Digits are tenths of full alpha, '.' is empty. Purely a debugging aid.
    void DescribeRegionAlpha(float leftDip, float topDip, float rightDip, float bottomDip,
                             int columns, int rows, wchar_t* out, size_t capacity);

    bool IsInitialized() const { return swapChain_ != nullptr; }

    // Releases the whole chain. Called by the destructor; exposed because a
    // device-removed or target-recreate path needs to rebuild from scratch.
    void Shutdown();

    // Which spacing was applied to glyphs and shapes, for reporting.
    UINT Dpi() const { return dpi_; }

    // How much memory the driver has charged this process on the GPU, and whether the adapter could
    // be asked at all. LOCAL is the GPU's own segment (VRAM on a discrete card, the same system memory
    // on an integrated one); NON_LOCAL is what the driver reserved outside it. Reported by the memory
    // line, because a GPU allocation with no ceiling shows up here and nowhere else.
    bool VideoMemory(unsigned long long* localBytes, unsigned long long* nonLocalBytes);

    // --- pacing -----------------------------------------------------------------------------------
    //
    // The swap chain's own frame-latency waitable object is the signal the frame loop should block
    // on: it is the compositor saying "the next frame may go now", so a wait on it ends when there is
    // something to wait for. Without it the loop can only guess, which is what a fixed 4 ms timer is.
    // Null when the swap chain was built without DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT or
    // the runtime refused the object; the loop then falls back to the compositor clock, then to a timer.
    HANDLE FrameLatencyWaitable() const { return frameLatencyWaitable_; }
    bool PacingIsWaitable() const { return frameLatencyWaitable_ != nullptr; }
    bool HasCompositorClock() const { return waitForClock_ != nullptr; }

    // Frames handed to the compositor. Reported per second by the throttle line: a count far above the
    // monitor's refresh rate is the sign that the pacing is not pacing anything.
    unsigned long long FramesPresented() const { return framesPresented_; }

    // How many decoded images and window icons the renderer is holding. This is the number that
    // separates "a cache filled up" from "something is leaking" on the memory line: Direct2D cannot be
    // asked how many bitmaps exist, but this program can count the ones it made.
    void CacheCounts(int* bitmaps, int* windowIcons) const;

private:
    bool CreateDeviceResources();
    bool CreateSwapChain(UINT widthPx, UINT heightPx);

    // The two decks: the application icon row on the right and the Start/toggle/indicator row on the
    // left.
    //
    // A function of its own because of WHERE it is called from. The decks were drawn inline before the
    // card, so opening the system panel painted its card over both of them -- the Start button, the
    // panel's own toggle, the indicators and every application icon simply vanished, and stayed gone
    // until the panel was closed. Their positions were unchanged and their hit tests still worked, which
    // is why nothing looked wrong in any layout assertion.
    //
    // On Windows the quick-settings flyout opens BELOW the taskbar and never over its buttons; this
    // reproduces that by drawing the decks last.
    void DrawDecks(const RoundedBox& box, const FrameState& frame);
    void ReleaseTarget();
    void RenderFrame(const FrameState& frame);
    // Keeps the lens capture pointed at the window directly below the island.
    void UpdateLensCapture(POINT at);

    // --- device chain ---
    ID3D11Device*            d3dDevice_   = nullptr;
    BackdropCapture          capture_;
    LensMaterial             lens_;
    bool                     lensActive_  = false;
    HWND                     lensWindow_  = nullptr;
    HWND                     window_      = nullptr;
    ULONGLONG                lensRetryAt_ = 0;
    // The pill's size the lens was last shaped for. A change re-warps the last captured frame, because
    // WGC is silent while the window behind is static.
    int                      lensPillW_   = 0;
    int                      lensPillH_   = 0;
    // The captured frame is consumed at most ~12 times a second: the readback is the expensive part of
    // the lens, and a live wallpaper would otherwise ask for it at 30 Hz for a subtle rim bend.
    ULONGLONG                lensNextAt_  = 0;
    // The refracted backdrop, when the theme asks for it and the capture started.
    ID3D11DeviceContext*     d3dContext_  = nullptr;
    IDXGISwapChain1*         swapChain_   = nullptr;
    // The same swap chain as IDXGISwapChain2, which is where the frame-latency limit and its waitable
    // object live. Kept as its own pointer because a failed QueryInterface must leave the older
    // interface usable -- the bar draws perfectly well without a waitable object.
    IDXGISwapChain2*         swapChain2_  = nullptr;
    // Kept for the memory line only: DXGI 1.4's adapter is the one that can report how much the driver
    // has charged this process, and nothing else in the bar needs an interface newer than 1.2.
    IDXGIAdapter3*           dxgiAdapter3_ = nullptr;
    // Owned by this class: the swap chain hands it over and expects CloseHandle (the swap chain must
    // outlive it, which Shutdown's ordering guarantees).
    HANDLE                   frameLatencyWaitable_ = nullptr;
    unsigned long long       framesPresented_ = 0;
    ID2D1Factory3*           d2dFactory_  = nullptr;
    ID2D1Device2*            d2dDevice_   = nullptr;
    ID2D1DeviceContext2*     d2dContext_  = nullptr;
    ID2D1Bitmap1*            d2dTarget_   = nullptr;

    // --- composition ---
    IDCompositionDevice*     compDevice_  = nullptr;
    IDCompositionTarget*     compTarget_  = nullptr;
    IDCompositionVisual*     compVisual_  = nullptr;
    IDCompositionEffectGroup* effectGroup_ = nullptr;   // opacity lives here, not on Visual3
    IDCompositionAnimation*   opacityAnimation_ = nullptr;

    // True while the composition effect group owns the opacity value. Setting
    // opacity per frame would fight the running animation, so the renderer
    // remembers which of the two is in charge.
    bool opacityAnimated_ = false;

    // --- dynamic entry points (absent on older Windows builds) ---
    typedef DWORD (WINAPI *WaitForCompositorClockFn)(UINT, const HANDLE*, DWORD);
    WaitForCompositorClockFn waitForClock_ = nullptr;

    // Which cached image a slot holds: the album art, or the card's blurred background
    // wash. Declared here so CachedBitmap's signature can name it.
    enum class BitmapSlot { Artwork = 0, Background = 1, AppIcon = 2, Count = 3 };

    // One cached GPU bitmap and the revision it was built from.
    struct BitmapCache {
        ID2D1Bitmap1*      bitmap = nullptr;
        unsigned long long revision = 0;
        int                width = 0;
        int                height = 0;
        // The pixels this bitmap was built from, compared as a pointer. See CachedBitmap: the revision alone
        // is only as good as the provider's promise that it changes with the image.
        const uint8_t*     pixels = nullptr;
    };
    BitmapCache bitmapCache_[static_cast<int>(BitmapSlot::Count)];

    ID2D1Bitmap1* CachedBitmap(const uint8_t* pixels, int width, int height,
                               unsigned long long revision, BitmapSlot slot);

    void ReleaseBitmapCache();

    // --- the taskbar's per-process icon cache ---
    //
    // One small PNG per process, fetched from that process's executable and decoded once. Keyed by
    // process id because several windows of the same application share an icon, and because a
    // process id is stable for the life of the process -- which is exactly as long as the icon is
    // valid.
    //
    // Entries are kept rather than evicted when a process exits: the array is small, and a window
    // disappearing and reappearing is common enough (minimising, switching desktops) that
    // refetching every time would be worse. The list is bounded, so a long session reaches a
    // steady state rather than growing.
    static constexpr int kMaxWindowIcons = 32;

    struct WindowIcon {
        HWND          window = nullptr;
        ID2D1Bitmap1* bitmap = nullptr;
    };
    WindowIcon windowIcons_[kMaxWindowIcons];
    int windowIconCount_ = 0;

    // Returns the icon bitmap for a window, fetching and decoding it on first use. Null when the
    // window has no icon, which is normal for a few system windows and not an error.
    ID2D1Bitmap1* IconForWindow(HWND window);
    void ReleaseWindowIcons();

    // A scratch copy of the frame's buttons with the icons filled in. A member rather than a local
    // so the array does not live on the stack for the duration of a draw.
    //
    // Sized from FrameState's own limit rather than a second constant, so the two cannot disagree:
    // a scratch array smaller than the frame's would silently drop buttons, and one larger would
    // invite the caller to pass more than the renderer can hold.
    TextRenderer::TaskButton taskButtons_[FrameState::kMaxTaskButtons];

    // The last row geometry written to the log, so the trace reports changes rather than every frame.
    std::wstring lastRowSummary_;

    // The live row-overlap check reports once per run: the first overlap is the finding.
    bool reportedOverlap_ = false;

    TextRenderer* text_ = nullptr;

    // Frame timing, for the assertions and the log.
    double    lastFrameMicroseconds_ = 0.0;
    double    frameMicrosecondsTotal_ = 0.0;
    long long frameCount_ = 0;

    UINT  dpi_    = 96;
    float scale_  = 1.0f;
    UINT  width_  = 0;
    UINT  height_ = 0;
};

} // namespace island
