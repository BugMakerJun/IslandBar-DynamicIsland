// =====================================================================
//  IslandWindow.h
//  The host window for the bar, and the owner of the island's state.
//
//  Design decisions locked in, with the reason each one exists:
//
//  * WS_EX_NOREDIRECTIONBITMAP + DirectComposition. The window owns no
//    redirection surface; the GPU composes the swap chain straight to the
//    screen, so there is no per-frame CPU copy.
//  * The window is created once at its final size and is NEVER resized.
//    Resizing a composition swap-chain window flickers. Every state change moves
//    the pill inside this fixed window instead.
//  * WS_EX_NOACTIVATE + WS_EX_TOOLWINDOW, and WM_MOUSEACTIVATE answers
//    MA_NOACTIVATE, so hovering or clicking the bar never steals focus and it
//    stays out of Alt-Tab.
//  * Region-level click-through. At rest the window carries WS_EX_TRANSPARENT so
//    clicks land on whatever is underneath; the style is dropped only while the
//    pointer is inside the pill, and WM_NCHITTEST re-checks with the same
//    rounded-rectangle predicate the renderer draws with.
//
//  Events are reported rather than handled: the window records that the pointer
//  entered, left or clicked, and the message loop drains those flags after
//  pumping. Deciding what a click means is policy and does not belong in a
//  window procedure.
// =====================================================================
#pragma once

#include <windows.h>

#include "domain/CardLayout.h"   // CardControl: which control a click landed on
#include "domain/Geometry.h"
#include "platform/Monitor.h"
#include "domain/Motion.h"
#include "render/TextRenderer.h" // the system panel's tiles, which the window hit-tests

namespace island {

class AppBar;

class IslandWindow {
public:
    static constexpr const wchar_t* kClassName = L"IslandBar.IslandWindow";

    IslandWindow() = default;
    ~IslandWindow();

    IslandWindow(const IslandWindow&) = delete;
    IslandWindow& operator=(const IslandWindow&) = delete;

    // Registers the class, creates the window, and derives the anchor.
    bool Create(const MonitorGeometry& monitor, bool show);
    void Destroy();

    // Shows or hides the bar itself, without destroying it.
    //
    // For the full-screen case: a full-screen window is a request for the whole screen, so the bar gets
    // out of the way and comes back when that window does. Hiding rather than destroying keeps the
    // renderer's device, the icon cache and the timers -- all of which are expensive and none of which
    // care whether the window is visible.
    void Show(bool show);

    // Whether the bar is currently being shown.
    bool IsShown() const { return shown_; }

    HWND Handle() const { return hwnd_; }
    const MonitorGeometry& Monitor() const { return monitor_; }
    const RECT& WindowRect() const { return rect_; }
    float Scale() const { return scale_; }

    // The bar that has claimed the strip this window occupies, if any.
    //
    // Held rather than owned. The shell sends ABN_POSCHANGED and
    // ABN_FULLSCREENAPP to the WINDOW, so the callback has to be routed from the
    // window procedure; there is no way for the AppBar to listen on its own.
    void SetAppBar(AppBar* appBar) { appBar_ = appBar; }

    // --- state ---
    // The pill to draw right now, in window-local DIPs.
    RoundedBox CurrentBox() const { return motion_.Current(); }

    // The layout anchor, exposed for diagnostics. When the pill's local
    // coordinates look wrong, the question is always whether the anchor or the
    // motion interpolation is at fault, and only the anchor answers that.
    const IslandAnchor& Anchor() const { return anchor_; }
    IslandState State() const { return motion_.Target(); }

    // The shape the motion is coming FROM, for callers that must stay anchored across a transition.
    IslandState FromState() const { return motion_.FromState(); }
    bool IsAnimating() const { return motion_.IsAnimating(); }
    float Progress() const { return motion_.Progress(); }

    // How far the transition has been pushed along by its OWN curve, 0..1.
    //
    // Anything that has to move in step with the shape wants this rather than Progress(), which is the
    // linear clock: the shape is drawn at the eased value, so a second quantity driven by the raw
    // fraction would drift away from it and then meet it again at both ends.
    float EasedProgress() const { return motion_.Eased(); }

    // Stretches every transition's duration. For diagnosing an animation that is over too fast to see;
    // see Motion::ScaleDurations for why it is the duration and not the frame interval.
    void ScaleTransitionDurations(float factor) { motion_.ScaleDurations(factor); }

    // How many times the window region has actually been re-applied. See ApplyInputRegion.
    unsigned long long RegionApplications() const { return regionApplications_; }

    // The region currently in force, for diagnostics.
    RECT AppliedRegion() const { return appliedRegion_; }

    // Requests a state. Returns true if a transition actually started.
    bool SetState(IslandState state);

    // Advances the animation. Returns true while more frames are needed.
    bool Tick(float dtSeconds);

    // --- events, drained by the caller ---
    // pointerInside is true once the pointer is over the pill, and is updated by
    // the pointer poll. Consumed on read.
    bool TakePointerInsideChanged(bool* pointerInside);
    bool TakeLayoutChanged();

    // Drains a left-click that landed on the pill.
    //
    // The window only records that it happened; what a click means is policy and
    // lives in the frame loop. While the window is click-through it receives no
    // button messages at all, so a click only arrives when the pointer is on the
    // pill -- which is exactly the "clickable while unread" case.
    //
    // COUNTED, not a flag. A flag holds one click however many arrive, so two clicks inside a single
    // frame become one and the second is silently dropped -- and a dropped click is indistinguishable
    // from a click that was misunderstood. A count at least answers "did the message arrive", which is
    // the question that decides whether the fault is in the window procedure or in the routing.
    bool TakeClick();

    // The same for a RIGHT click, which only the tray page's cells use so far: right-clicking one of the
    // shell's tray icons is how its application's own menu is opened -- the menu with 退出 in it -- and
    // there is no UI Automation pattern for a right click, so it has to arrive as one. See TrayPress.
    bool TakeRightClick();

    // Takes a left-button PRESS, reported with where it landed. A press and a click are different events:
    // the click says "this control was chosen", the press says "the drag has started", and a slider needs
    // both -- see the handler for WM_LBUTTONDOWN.
    bool TakePress(POINT* screenPoint);

    // Whether this window is holding the mouse, i.e. whether a press it accepted has not been released yet.
    //
    // Tracked from the window's OWN messages rather than asked of GetAsyncKeyState: that reports a state
    // that disagreed with reality for synthetic input (it read "up" five milliseconds into a press that was
    // still held), and a drag that ends on the first frame of itself is a slider that does not move.
    bool LeftButtonDown() const { return leftDown_; }

    // --- pointer ---
    bool IsPointerInside() const { return forcedPointerMode_ > 0 ? true : pointerInside_; }

    // Where the pointer was the last time the window looked, in SCREEN pixels. The liquid-glass sweep
    // follows it along the pill (see FrameState::pillPointerFraction); ToLocalDip turns it into the
    // window's own DIP first.
    POINT LastPointer() const { return lastPointer_; }

    // The pass-through decision as a pure function, so a test can check it for
    // arbitrary points without moving the physical cursor (which races the
    // window's own poll and makes the result depend on which ran last).
    //
    // `iconCount` and `hoveredIcon` describe the row of application icons beside the island. They
    // are part of the hit test because the icons are part of the bar: a pointer over an icon must be
    // accepted, or the window stays click-through there and the icon can never be clicked or
    // hovered. The first version tested only the island's own shapes, so the icons were decorative
    // -- visible and completely inert.
    // The hovered index is deliberately NOT a parameter: the decision must not depend on which icon
    // is currently widened, or the answer would move as the pointer moves. See TaskIconUnderPointer.
    //
    // `cardLeftDip` is where the frame loop placed an outer card (its LEFT edge), or 0 when no card is
    // up. It is needed because the card is not inside the state's own shape: it hangs below the bar and
    // is pinned to the control that opened it, so a test built only from the shapes misses it.
    // `fullscreenApp` is the one argument here that is not geometry: while an application has the screen
    // only the ISLAND is the bar, because only the island is DRAWN then (see Renderer::RenderFrame). It is
    // passed in rather than measured here so the predicate stays a function of its arguments, which is what
    // lets the self-test ask about both answers.
    static bool ShouldAcceptPointer(const MonitorGeometry& monitor, const RECT& windowRect,
                                    POINT screenPoint, IslandState state, int iconCount = 0,
                                    float cardLeftDip = 0.0f, bool fullscreenApp = false,
                                    int trayIconCount = 0,
                                    const RoundedBox& currentShape = RoundedBox{},
                                    bool noticeStripUp = false);

    // Whether a call or a message is being announced. The window cannot work this out for itself: while a
    // card is open the state is Expanded, and a notice is not a state -- it is something the bar is SAYING.
    // Set once per frame by the frame loop, and read only to choose the box the bar's own controls are laid
    // out from (see the note in ShouldAcceptPointer).
    void SetNoticeStripUp(bool up) { noticeStripUp_ = up; }

    // Told by the frame loop, which measures it (see island::FullScreenAppInFront). The hit test is
    // answered here, so it has to be told the same thing the renderer was.
    void SetFullscreenApp(bool fullscreenApp);

    // The tray: how many icons the CARD holds, whether the pointer is on its button, and which cell of the
    // card's grid the pointer is on. The count comes from a list the window does not own, which is why it
    // is told rather than found out -- the same arrangement as the application icon count.
    void SetTrayIconCount(int count);
    int  TrayIconCount() const { return trayIconCount_; }
    bool TrayButtonHovered() const { return trayButtonHovered_; }
    // Whether a screen point is on the tray button. The frame loop's click handler asks this, because the
    // button is a control and the click has to land on the same rectangle that is drawn.
    bool TrayButtonAtScreen(POINT screenPoint) const;
    // The tray icon (a cell of the card's grid) under the pointer, or -1. Chosen here because this is the
    // only place that knows where the pointer is; the geometry comes from TrayPageFor, so the hit test and
    // the drawing agree.
    int  TrayCellHovered() const { return trayCellHovered_; }

    // The tray's own context menu, when one is up (see TrayMenuFor in CardLayout.h): the rows it was
    // opened with, in card-local DIP, told to the window by the frame loop -- the same arrangement as the
    // tray's own count, and for the same reason (the window knows where the pointer is, the loop knows
    // what was drawn).
    //
    // OPENED BY THE WINDOW, not handed a layout: this is the only place that owns the card's geometry --
    // the same geometry its own hover test runs against, so the menu cannot be drawn somewhere other than
    // where its rows are hit-tested. It hangs off the bottom-right corner of the cell that was right
    // clicked, clamped into the card (see TrayMenuFor).
    void OpenTrayMenu(int cell, int entryCount);
    void CloseTrayMenu() { trayMenuOpen_ = false; trayMenuRowHovered_ = -1; }
    bool TrayMenuIsOpen() const { return trayMenuOpen_; }
    const island::TrayMenuLayout& TrayMenu() const { return trayMenuLayout_; }
    int  TrayMenuRowHovered() const { return trayMenuRowHovered_; }

    // --- the Start card -------------------------------------------------------------------------------
    //
    // What the pointer is on while the Start card is the level that is up. The page, the two counts and
    // the picker's scroll offset are TOLD to the window, the way the tray's count is: the window owns no
    // application list, but it is the only place that knows where the pointer is -- and a hit test against
    // geometry the drawing did not use is the defect this project has paid for more often than any other.
    enum class StartHitTarget { None, Pin, PickerRow, PowerRow, PowerButton, EmptyGrid, AllApps };
    struct StartHit {
        StartHitTarget target = StartHitTarget::None;
        int index = -1;
    };

    // `pageOpen` false means the Start card is not the level that is up, and every hit test answers None.
    void SetStartState(bool pageOpen, int page, int pinCount, int listCount, int pickerScrollRow);
    // Moves the picker's first visible row by `notches` and CLAMPS it, which is the window's job for the
    // same reason the sound page's scroll is: it is the one that knows the card's geometry, so the offset
    // the frame loop reads back cannot leave the list.
    void ScrollStartPicker(int notches);
    int  StartPickerScroll() const { return startPickerScroll_; }
    StartHit StartHitAtScreen(POINT screenPoint) const;
    StartHit StartHitHovered() const { return startHit_; }

    // Tells the window how many application icons are on screen.
    //
    // The window needs this to decide whether the pointer is on one, and only the frame loop knows
    // it -- the count comes from the window list, which the window itself does not own.
    // (The declarations live with the indicator accessors below, which is where the distinction
    // between application icons and system indicators is explained.)

    // The icon button under the pointer, or -1. Recomputed on every pointer poll.
    //
    // A forced index can stand in for the pointer, which is a diagnostic: the icon row exists for the
    // pointer, and a script cannot move the pointer on this machine. `-1` means "whichever icon is last",
    // which is the one whose title is cut when the row is sized from its resting width -- see
    // ISLANDBAR_HOVER_ICON. `-2` (the default) means no forcing.
    int  HoveredIcon() const {
        if (forcedHoverIcon_ == kForcedHoverLast) {
            const int last = RowButtonCount() - 1;
            return last >= 0 ? last : -1;
        }
        if (forcedHoverIcon_ >= 0) return forcedHoverIcon_;
        return hoveredIcon_;
    }

    void SetForcedHoverIcon(int index) { forcedHoverIcon_ = index; }

    // Treat the pointer as being on the bar, whatever it is really doing.
    //
    // Diagnostic only: hovering is what opens the media card, and a script cannot move the pointer on this
    // machine -- so the one gesture that shows the detail view cannot otherwise be photographed. See
    // ISLANDBAR_FORCE_INSIDE: 0 = off, 1 = on the pill, 2 = on the bar but NOT on the island -- the empty
    // stretch past the icon row, which is the case that made the media card appear where it should not.
    void SetForcedPointerInside(int mode) { forcedPointerMode_ = mode; }

    // Treat the pointer as resting on a deck button (the Start button or the panel's toggle).
    //
    // Diagnostic only, for the same reason as the two above: the "give the card up" rule is about where the
    // pointer is, and a script cannot put it there. See ISLANDBAR_FORCE_DECK.
    void SetForcedDeckHover(bool forced) { forcedDeckHover_ = forced; }

    // Whether the pointer is over the ISLAND itself -- the pill -- rather than somewhere else on the bar.
    //
    // The two are different questions and the difference is the whole reason the media card popped up in
    // the empty stretch past the icon row: everything on the bar accepts the pointer, and the card rule was
    // asking "is the pointer on the bar".
    //
    // Mode 1 of the diagnostic counts as being on the island, because that is what it means ("the pointer is
    // on the pill"); mode 2 says the opposite on purpose.
    bool IsPointerOnIsland() const {
        if (forcedPointerMode_ > 0) return forcedPointerMode_ == 1;
        return pointerOnIsland_;
    }

    // Whether the pointer is in the CARD's COLUMN -- the card and the gap above it -- which is the pill's
    // territory as much as the pill is.
    //
    // The column rather than the card, because the pointer has to be able to WALK from the pill onto the
    // card and the 8 DIP between them belongs to that journey. Diagnostic modes 3 (on the card) and 4 (in
    // the gap) stand in for the two halves, so both can be checked without a pointer.
    bool IsPointerInCardColumn() const {
        if (forcedPointerMode_ > 0) return forcedPointerMode_ == 3 || forcedPointerMode_ == 4;
        return pointerInCardColumn_;
    }

    // Which application icon a screen point is over, or -1.
    //
    // The window owns the row's geometry, so it owns this question too. The alternative -- rebuilding
    // the row in the frame loop -- would be a second copy of the layout, and this project has already
    // been through what happens when one object's position is computed in two places: the background
    // moved and the icons did not.
    //
    // Answered from the RESTING row, the same geometry the pointer's position is judged against, so a
    // button that has opened around the pointer cannot make the pointer appear to be elsewhere.
    int  IconAtScreen(POINT screenPoint) const;

    // Whether a screen point is on the Start button, which is on the other side of the
    // island from the icons. Owned here for the same reason: the window owns the layout.
    bool StartAtScreen(POINT screenPoint) const;

    // Whether a screen point is on the toggle for the system panel.
    bool SystemButtonAtScreen(POINT screenPoint) const;
    bool SystemButtonHovered() const { return forcedDeckHover_ ? true : systemHovered_; }

    // WHICH card is on screen, named by the frame loop every frame.
    //
    // The window's hit tests each answer about one card's content, and two of them cannot tell that their card
    // is not the one showing: the panel's tiles and the media card's transport buttons are both laid out in
    // the window at all times (the frame loop fills the panel's text whether or not the panel is up). The
    // CLICK path never had to care, because the loop gates each test on its own flag before asking; the cursor
    // has to ask the same question without the loop in between, so it is told the answer.
    //
    // Measured without it: hovering the MEDIA card's cover reported a hand, because a panel TILE was behind
    // that point.
    void SetCardOwner(CardOwner owner) { cardOwner_ = owner; }

    // True when a screen point is over a CONTROL rather than over the bar's own surface.
    //
    // The cursor is the only feedback this bar has that is not a paint: a hand over a button is how a
    // pointer learns it can be pressed. Asked for in as many words -- "这些按钮 鼠标移上去 应该变成小手", over a
    // photograph of the media card's previous / play-pause / next -- and the same answer is right for every
    // other control the bar has: the icon row, the two deck buttons, the panel's tiles, speakers, slider and
    // its page arrows, and the sound page's rows and sliders.
    //
    // The ISLAND ITSELF is deliberately not one of them: hovering it opens the card, which is not the same as
    // pressing it, and a hand over the whole bar would claim "clickable" about the one thing that is mostly a
    // surface. A message on the notice strip is not one either, for the same reason -- see the note in the
    // .cpp about what a click there does now.
    bool PointerOverControl(POINT screenPoint) const;

    // The island box both ends of the bar are laid out from, in window-local DIP.
    RoundedBox LayoutIslandFor() const;

    // Whether the pointer is on the Start button right now.
    bool StartHovered() const { return forcedDeckHover_ ? true : startHovered_; }

    // --- the application icon row ------------------------------------------------
    //
    // The row holds the application icons and NOTHING ELSE. The system indicators used to be appended
    // to it and hit-tested in the same index space; they were removed from the bar, so this index space
    // is now exactly the application icons and there is no second range to keep straight.
    void SetTaskIconCount(int count);
    int  IconCount() const { return taskButtonCount_; }

    // How many buttons the row holds. Kept as a named accessor because `IconAtScreen` answers in this
    // index space and naming it is what stops a caller inventing its own bound.
    int  RowButtonCount() const { return taskButtonCount_; }
    int  TaskButtonCount() const { return taskButtonCount_; }

    // Narrows the window's own area to the island and its icon row.
    //
    // The window is a 2560x240 strip across the top of the screen, and everything outside the pill is
    // transparent. Transparency of PIXELS is not transparency of INPUT: without this, the strip is a
    // real window over the top of the desktop, and whether a click reaches the desktop depends entirely
    // on WM_NCHITTEST answering HTTRANSPARENT for that point -- which is correct but was reported as a
    // band below the island where nothing could be clicked.
    //
    // A window region answers the same question a level lower, where the window manager asks it: input
    // outside the region belongs to whatever is underneath, with no message handling involved.
    //
    // The region is deliberately the BOUNDING BOX of everything the bar can draw, not the exact shape.
    // It exists to stop the strip swallowing clicks, not to trace the pill -- an exact region would clip
    // the animation and the widened icon buttons, and WM_NCHITTEST already does the precise test inside
    // it.
    void ApplyInputRegion();

    // The region, in window coordinates, for a given state. Separated so the geometry can be asserted
    // without a window.
    // `current` is the ANIMATED shape; a default-constructed box sizes the region from the target
    // state alone. It exists because a region sized from the target while the drawing is still the
    // previous shape CLIPS the transition -- which is what turned the bar into a flat dark bar.
    // `taskIconCount` is how many application icons the row is actually showing, so the region covers
    // exactly those. A negative value means "the most that could fit", which is what a caller with no
    // window list wants.
    static RECT InputRegionFor(const MonitorGeometry& monitor, IslandState state,
                               const IslandAnchor& anchor,
                               const RoundedBox& current = RoundedBox{},
                               float cardLeftDip = 0.0f, int taskIconCount = -1,
                               float statusStripWidthDip = 0.0f, int trayIconCount = 0,
                               bool noticeStripUp = false);

    // The anchor the current box geometry is derived from.
    IslandAnchor CurrentAnchor() const;

    // The thickness of the band the shell's own taskbar keeps at the top of the screen, in DIPs --
    // the height of the full-screen state, and therefore of the region that has to cover it. Measured
    // from this window's own monitor. See IslandAnchor::bandHeightDip.
    float BandHeightDip() const;

    // Polls the pointer and updates the enter/leave decision.
    //
    // Public because the frame loop calls it: relying only on the window timer turned out to be
    // unreliable, and the failure was silent (see main.cpp).
    void UpdatePointerState();

    bool HitTestScreen(POINT screenPoint) const;
    void ToLocalDip(POINT screenPoint, float* x, float* y) const;

    // Which music-card control a screen point lands on, or CardControl::None.
    //
    // The window owns this because it owns the layout: the buttons' rectangles come
    // from the same CardLayout the renderer draws with, and the frame loop uses this to
    // tell a click on a control from a click anywhere else on the card.
    CardControl MusicControlAtScreen(POINT screenPoint) const;

    // What the panel's controls are under a screen point -- a tile, a paging arrow or the settings button.
    //
    // One call for all of them, because they are laid out by the same function: the alternative is one accessor
    // per control, each with its own copy of where the card is, which is how the tiles and the music controls
    // came to be hit-tested in the wrong places (HANDOFF 7.81, 7.86).
    SystemPanelHit SystemPanelHitAtScreen(POINT screenPoint) const;

    // The tiles currently shown, and where the card is placed. Both are set by the frame loop, which is
    // the only place that knows the machine's state and the pressed control.
    void SetSystemPanel(const TextRenderer::SystemPanelText& panel, float cardLeftDip);

    // Whether the status strip is up, told to the window so the input region can cover it.
    //
    // The width rather than a flag, because the region's arithmetic is in DIPs and the strip's own box
    // comes from StatusStripFor -- so the caller passes the constant while the strip is showing and zero
    // while it is not, and a non-zero width means exactly "the strip is up".
    //
    // It has to reach the window at all for the reason every other shape does: the region is what decides
    // which pixels belong to this window, and a region narrower than the drawing CLIPS it -- the strip
    // would be a capsule with its left end cut off. The region is re-applied here when the answer changes,
    // so nothing has to remember to do it.
    void SetStatusStripWidth(float widthDip);

    // The tiles currently shown, for the click handler to read an action from. Const because the
    // handler decides what to DO; it does not get to change what is displayed.
    const TextRenderer::SystemPanelText& SystemPanel() const { return systemPanel_; }

    // --- the "声音输出" page, which is the panel's second level ---------------------------------
    //
    // It is the SAME card in the SAME place -- the shell's own flyout does not move when it goes one level
    // down -- so all this needs is which level is showing, what the page says, and how far it has been
    // scrolled. The card's own geometry comes from SetSystemPanel's `cardLeftDip` as before.
    void SetSoundPage(const TextRenderer::SoundPageText& page, float scroll);

    // Back to the panel's first level. Called every frame the page is not showing, so the hit test cannot
    // be left answering for a level that is no longer there.
    void CloseSoundPage();

    // What the page's own controls are under a screen point: the back arrow, a device row, a spatial row, a
    // mixer slider, the mixer's gear, the footer, or the scrollbar.
    SoundPageHit SoundPageHitAtScreen(POINT screenPoint) const;

    // The scroll offset, clamped to what the page can actually scroll -- which the window can work out
    // because it is the one that knows how many rows the page has.
    float SoundPageScroll() const { return soundScroll_; }
    void SetSoundPageScroll(float scroll);

    // A wheel notch over the card, if the page is up, as a signed number of steps (one notch is 1, up is
    // positive). REPORTED rather than applied: the frame loop owns the offset, and a window that scrolled
    // itself would be a second owner of the same value -- which is exactly how the first version of this
    // dropped every wheel event, because the frame loop then wrote its own offset back over the window's.
    bool TakeSoundWheel(int* notches);

    // Where a press on the scrollbar's track wants the page to go: -1 for a screenful towards the top, +1
    // towards the bottom, 0 for the thumb itself (which is dragged rather than jumped).
    int SoundScrollJumpFor(POINT screenPoint) const;

    // The fraction of a slider for a drag that is ALREADY RUNNING, taken from the pointer's x alone.
    //
    // The hit tests above answer "what is under this point", which is the right question for a press and the
    // wrong one for a drag: once the gesture has started the pointer leaves the row, the card and sometimes
    // the bar itself, and the value still has to follow it (clamped at the ends). Asking the hit test instead
    // is what made the handle freeze half-way through a drag.
    // `row` is which slider: 0 the volume, 1 the brightness (SystemPanelHit::index carries the same
    // numbering from the press that started the drag).
    bool PanelSliderFractionForDrag(int row, POINT screenPoint, float* fraction) const;
    bool MixerSliderFractionForDrag(int row, POINT screenPoint, float* fraction) const;

    // Applies an already-made pass-through decision. Used by the self-test so the
    // extended-style transition can be checked deterministically.
    void ApplyPointerDecision(bool accept);

    // Re-reads the monitor and re-applies position, size and z-order.
    void SyncToMonitor();

    // Pumps messages. Returns false once WM_QUIT arrives or the window is gone.
    bool PumpMessages();

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);

    // Recomputes the pill's anchor from the current monitor and window rect.
    void Layout();



    HWND            hwnd_ = nullptr;
    MonitorGeometry monitor_{};
    RECT            rect_{};
    float           scale_ = 1.0f;

    Motion       motion_{};
    IslandAnchor anchor_{};

    bool  pointerInside_ = false;
    POINT lastPointer_{};
    bool  havePointer_ = false;

    bool  pointerInsideChanged_ = false;

    // The pointer trace prints on the first poll and on every change of the accepted answer, so
    // the log gets one line per enter and one per leave instead of four a second.
    bool  haveTrace_ = false;
    bool  lastAccepted_ = false;

    // How many application icons the bar is currently showing, and which one the pointer is on.
    //
    // Owned by the window because the hit test needs them, but SET by the frame loop, which is the
    // only place that knows the window list. `hoveredIcon_` indexes the application icon row, which is
    // now the only row there is.
    int   taskButtonCount_ = 0;
    int   hoveredIcon_ = -1;
    // -2 no forcing, -1 the last icon, 0..n that icon. Diagnostics only; see HoveredIcon.
    static constexpr int kForcedHoverNone = -2;
    static constexpr int kForcedHoverLast = -1;
    int   forcedHoverIcon_ = kForcedHoverNone;
    // Diagnostics only: 0 off, 1 on the pill, 2 on the bar but off the island, 3 on the card. See
    // SetForcedPointerInside.
    int   forcedPointerMode_ = 0;
    // Diagnostics only: pretend the pointer is on a deck button. See SetForcedDeckHover.
    bool  forcedDeckHover_ = false;
    // Whether the pointer is over the pill itself. See IsPointerOnIsland.
    bool  pointerOnIsland_ = false;
    // Whether the pointer is in the column the card hangs in. See IsPointerInCardColumn.
    bool  pointerInCardColumn_ = false;
    bool  startHovered_ = false;
    bool  systemHovered_ = false;

    // The system panel: its tiles, and where the card was placed for them.
    //
    // Held on the window because the HIT TEST needs both, and the hit test has to agree with the
    // drawing. Recomputing the card's position here would be a second copy of the placement rule, and
    // this project has already been through what happens when one object's position is computed in two
    // places: the background moved and the icons did not.
    TextRenderer::SystemPanelText systemPanel_ = {};
    // The 声音输出 page, which is the panel's second level: what it says, and how far it is scrolled. Held
    // here for the same reason the tiles are -- the hit test has to agree with the drawing, and both come
    // from SoundPageFor.
    TextRenderer::SoundPageText soundPage_ = {};
    float soundScroll_ = 0.0f;
    bool  soundPageOpen_ = false;
    // Wheel notches seen over the card, waiting to be taken by the frame loop. Positive is up.
    int   soundWheelNotches_ = 0;
    // The wheel's raw delta that has not yet added up to a notch. A precision touchpad, and a wheel that
    // reports its motion in fractions of a notch, send deltas smaller than WHEEL_DELTA -- and an integer
    // division by WHEEL_DELTA throws every one of those away, so the list stood still under a hand that
    // was clearly scrolling. Kept here, with the notches, because the two are the same sentence.
    int   soundWheelRemainder_ = 0;
    // Where the frame loop placed the card's LEFT edge, or 0 when no card is up. Named for the card and
    // not for the panel: every one of the three cards is placed the same way, and the name it had before
    // ("the system panel's card left") is part of why the music card's controls were hit-tested against
    // the wrong rectangle -- see PlacedCardFor.
    float cardLeftDip_ = 0.0f;
    // Non-zero while the status strip (the date, the time and the weather) is up, in DIPs. Held here so
    // the input region can cover it; see SetStatusStripWidth.
    float statusStripWidthDip_ = 0.0f;
    // See SetNoticeStripUp.
    bool  noticeStripUp_ = false;
    // Whether a full-screen application is in front, as measured by the frame loop. Held here because the
    // hit test is answered here: while it is set, only the island is the bar. See SetFullscreenApp.
    bool fullscreenApp_ = false;
    // The tray, for the same reason: how many icons are on the bar, and which one the pointer is on.
    int trayIconCount_ = 0;
    bool trayButtonHovered_ = false;
    int trayCellHovered_ = -1;
    // The tray's own menu: whether it is up, the rows it was opened with, and the row under the pointer.
    bool trayMenuOpen_ = false;
    island::TrayMenuLayout trayMenuLayout_ = {};
    int trayMenuRowHovered_ = -1;
    // The Start card, told the same way: whether it is the level that is up, which page of it, and the two
    // counts and the scroll offset the hit tests are measured against. The lists themselves stay in the
    // frame loop -- this window owns no application list, exactly as it owns no tray icon list.
    bool startPageOpen_ = false;
    int  startPage_ = 0;
    int  startPinCount_ = 0;
    int  startListCount_ = 0;
    int  startPickerScroll_ = 0;
    StartHit startHit_ = {};
    // The last hit reported to the log, so the card's hover trace reports changes rather than every poll.
    // A separate pair from the icon row's: the two answers are about different things and one moving must
    // not silence the other.
    int  lastReportedStartTarget_ = -2;
    int  lastReportedStartIndex_ = -2;
    // Which card the frame loop says is up: what the cursor's hit tests are gated on. See SetCardOwner.
    CardOwner cardOwner_ = CardOwner::None;
    // The last value written to the log, so the hover trace reports changes rather than every poll.
    int   lastReportedHovered_ = -2;

    // Not owned: see SetAppBar.
    AppBar* appBar_ = nullptr;

    bool  layoutChanged_ = false;
    int   pendingClicks_ = 0;
    // Right clicks, counted the same way and for the same reason: what one means is policy and belongs in
    // the frame loop. See TakeRightClick.
    int   pendingRightClicks_ = 0;
    int   pendingPresses_ = 0;
    POINT lastPress_ = {};
    bool  leftDown_ = false;
    bool  destroyed_ = false;

    // The window region currently applied, so an unchanged one is not re-applied.
    //
    // Zeroed to mean "nothing applied yet", which no real region can be -- a region always has at least
    // the 1x1 fallback that InputRegionFor guarantees.
    RECT  appliedRegion_ = {};

    // How many SetWindowRgn calls got through. A count rather than a flag: "one per state change" and
    // "one per frame" are the two designs, and the number tells them apart while the picture cannot.
    unsigned long long regionApplications_ = 0;

    // Whether the window is currently shown, so Show() is idempotent and the
    // full-screen path cannot fight over it.
    bool  shown_ = false;

    UINT_PTR pointerTimer_ = 0;
};

} // namespace island

