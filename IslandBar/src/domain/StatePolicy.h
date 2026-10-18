// =====================================================================
//  StatePolicy.h
//  What the pointer means for the island's shape.
//
//  This is the rule that decides, once the motion has settled, which shape the island
//  should be in given where the pointer is. It lives here, as a pure function, for two
//  reasons:
//
//   * It is the rule that broke. The promotion from hover to the details card was
//     written inside the branch that LOWERS an already-open card, so nothing ever moved
//     Hover -> Expanded. Every geometry assertion still passed, because the card's
//     rectangle was perfectly reachable -- the state machine simply never asked for it.
//     The symptom was "the pointer goes in and no card appears", with music playing so
//     the resting shape was the lyric strip rather than the plain pill.
//
//   * A test that reproduces a rule instead of calling it proves nothing. The first
//     version of that regression test copied this logic into Diagnostics.cpp, and
//     passing against the broken build is exactly what it did. The rule has to have one
//     home, and the test has to call the same one the frame loop does.
//
//  Deliberately free of window, renderer and Win32 types: it is a function of four
//  values, so it can be exercised for every combination cheaply.
// =====================================================================
#pragma once

#include "domain/Motion.h"

namespace island {

// Everything the policy needs, gathered so the signature does not grow a parameter every time the
// bar learns something new. Six positional arguments of which four are bools is a call site nobody
// can read, and a transposed argument would compile.
struct PointerContext {
    IslandState shape = IslandState::Collapsed;
    IslandState resting = IslandState::Collapsed;
    bool pointerInside = false;
    bool notificationActive = false;
    // ... and whether that notification is a CALL rather than a message.
    //
    // The two want different widths. A message is a line of text and needs the 420 DIP notice strip; a
    // call is an icon, a timer and a meter -- about 150 DIP -- and putting it in the strip left 200 DIP of
    // empty glass on the left of the icon. A call therefore takes the bar cluster, which is what it looked
    // like full screen and what it has to look like everywhere ("玩游戏的时候是右边的这个图，但是不玩
    // 游戏的时候 左边还是之前的那样").
    bool callActive = false;
    // A media session exists -- PLAYING OR PAUSED. The two questions are different: `resting == MusicStrip`
    // means "the pill has something to say right now", while this means "there is a player to TALK TO".
    //
    // That second question is what a card needs: its cover, its progress bar and its three transport keys
    // all come from the session. The pill's own line can also be filled by a player that publishes no
    // session at all -- read from its window title instead (see PlayerWindow.h) -- and that source has
    // words and nothing else, so a card built on it would be a rectangle of empty rows with three dead
    // buttons. Hence the card asks THIS, not `resting`.
    bool musicPresent = false;

    // The row of application icons beside the island.
    int iconCount = 0;
    int hoveredIcon = -1;   // -1 for none

    // An application is full-screen in front of everything.
    //
    // This outranks the pointer and the notification, and it is the only input that does: a
    // full-screen window has asked for the whole screen, so the bar becomes the full-width strip that
    // fills the reserved band instead of a pill in the middle of it.
    bool fullscreenApp = false;

    // The system panel has been opened by pressing its control on the left deck.
    //
    // It wants the same CARD as a notification and the music player -- same shape, same state -- so it
    // is a flag here rather than a state of its own. The difference between the three is which of them
    // gets to compose the card, and that is settled where the card is drawn.
    bool systemPanelOpen = false;

    // The pointer is on one of the left deck's buttons -- the Start button or the panel's toggle.
    //
    // They are their own controls, and the details card belongs to the PILL, so the pointer resting on
    // one of them is not a request for the card. The window computes this (it is the only place that knows
    // where those buttons are); the policy only needs to know the answer.
    bool pointerOnDeck = false;

    // The pointer is over the ISLAND -- the pill -- rather than anywhere else on the bar.
    //
    // "On the bar" and "on the island" are different questions, and the details card is about the island
    // only. Everything on the bar accepts the pointer (the icons, the deck's buttons, and the stretch past
    // the row), so asking the looser question made the media card appear in the empty area to the right of
    // the icons. Reported exactly that way: "the card should come up only when the pointer enters the
    // island".
    bool pointerOnIsland = false;

    // The pointer is in the CARD's COLUMN: the card that hangs below the bar, and the few DIP between it and
    // the bar.
    //
    // Both halves, because the pointer has to be able to WALK from the pill onto the card -- the transport
    // buttons live there -- and the 8 DIP gap between them is part of that walk. A version that knew only
    // about the card itself gave the card up mid-crossing.
    bool pointerInCardColumn = false;
};

// True when the pointer is on one of the application icons rather than on the island.
inline bool PointerIsOnIcon(const PointerContext& c) {
    return c.iconCount > 0 && c.hoveredIcon >= 0;
}

// True when a card that is up should be GIVEN UP because the pointer has moved to a part of the bar that is
// not the island.
//
// Two such parts: an application icon -- which is asking about its own window, and widens itself instead --
// and one of the deck's buttons, which are controls of their own. The card belongs to the pill, so the pill
// is where it stays; hovering anything else on the bar puts it away.
//
// Only a CARD is given up. The resting shapes are left alone, because "the pointer is on an icon" is
// already answered for them by ShouldPromoteToTaskbar, and returning them to rest here would take the bar
// out of the hover shape the pointer just asked for.
//
// The pill's territory is the pill AND the card hanging from it. Taking "not on the island" literally was
// the mistake: the card is below the bar, so the pointer left the island on its way to the card, the card
// closed, and its buttons could not be pressed -- reported as "the card disappears when I move the mouse
// onto it".
inline bool ShouldGiveUpCard(const PointerContext& c) {
    if (!c.pointerInside) return false;
    if (c.pointerOnIsland || c.pointerInCardColumn) return false;
    return c.shape == IslandState::Expanded;
}

// True when the pointer sitting inside the island should open the details card.
//
// Two things ask for it:
//
//   * A notification, which outranks the pointer -- a message must not be hidden by where the mouse
//     happens to be.
//
//   * Something PLAYING. The resting shape is the lyric strip exactly while a session reports Playing
//     (the now-playing policy in main.cpp rests a paused session as the plain pill), so a strip under the
//     pointer means "the details are one hover away". The card is the pill's expanded form -- the same
//     state and shape as the system panel -- so opening it costs no geometry; what it costs is the hover,
//     which is the deliberate gesture the README has promised since the first phase:
//
//         Hover -> Expanded: art, title, artist, and previous / play-pause / next
//
// It was restricted to notifications for several rounds, on the argument that a hover should show the
// taskbar instead. The taskbar is the ICON ROW, and that is on screen in every state, so the hover showed
// it either way -- and the effect of the restriction was that the media card became unreachable, reported
// as "moving the mouse in no longer gives me the detailed music card".
inline bool ShouldPromoteToCard(const PointerContext& c) {
    // ON THE ISLAND, not merely on the bar.
    //
    // One condition replaces three. "On the bar" includes the icon row, the deck's buttons and the empty
    // stretch past the row, and each of those used to open the card for its own wrong reason -- reported
    // one at a time, as "not on the icons" and then "not in the empty area to the right". The card belongs
    // to the pill; the pill is the only place that asks for it.
    //
    // A notification is the exception, and it never reaches here: NextShape returns the card for a message
    // before any pointer rule runs, so a message is not hidden by where the pointer happens to be.
    if (!c.pointerInside || !c.pointerOnIsland) return false;
    if (c.shape != IslandState::Hover && c.shape != IslandState::Collapsed &&
        c.shape != IslandState::MusicStrip) {
        return false;
    }
    // A notification is NOT a reason any more: a message is the strip and stays the strip (see NextShape).
    //
    // A PAUSED SESSION STILL OPENS THE CARD, which is what makes the play/pause button usable: the card
    // used to need `resting == MusicStrip`, which is true only WHILE PLAYING, so pressing pause retracted
    // the card in the same breath -- the button vanished and the next press, the one that would resume,
    // could never be made. Reported as "暂停按钮点击没有变化".
    //
    // A SESSION, not merely a line on the pill: `resting == MusicStrip` is also true for a player read
    // from its window title (see PlayerWindow.h), and that source has no cover, no position and no
    // transport keys -- every field this card is made of. So the card asks for the session itself.
    return c.musicPresent;
}

// True when the pointer should make the island show its row of application icons widened.
//
// NOT when the pointer is on an icon. That distinction is the whole point of this function: hovering
// one icon widens THAT icon and nothing else, so the island must keep its shape. An earlier version
// treated "the pointer is anywhere on the bar" as one condition, so resting on an icon expanded the
// island as well -- which is not what a Dynamic Island does, and not what was asked for.
inline bool ShouldPromoteToTaskbar(const PointerContext& c) {
    if (!c.pointerInside || c.notificationActive) return false;
    if (PointerIsOnIcon(c)) return false;
    // ... and neither is a deck button. Hovering one leaves the bar at rest -- which keeps the lyric strip
    // on the pill instead of replacing it with the clock, and is what "its own control" means.
    if (c.pointerOnDeck) return false;
    // EXCEPT while there is a player: there the card is what the pointer is asking for, so pulling it down
    // to the hover shape and letting `ShouldPromoteToCard` open it again on the next frame would be a
    // visible flicker -- the card would start retracting and then turn around.
    //
    // `musicPresent` as well as `resting == MusicStrip`, because the card can now be open over a PAUSED
    // session: with only the first test, hovering a paused player opened the card and this rule closed it
    // again on the following frame -- measured in the log as `state: expanded` and `state: hover` swapping
    // every frame, and reported as "音乐卡片会出现消失的来回".
    //
    // The question is the SAME one the card itself asks (a session to control), so the two cannot
    // disagree about whether a card is up over a player.
    if (c.shape == IslandState::Expanded && c.musicPresent) {
        return false;
    }
    // `Expanded` is included for the case where the card has LOST THE REASON IT WAS OPENED while the
    // pointer is still on the bar -- the system panel's control pressed a second time, or its click
    // elsewhere. No rule below matches Expanded with the pointer inside, so the state stayed on the card
    // for ever: the panel was a flag on top of a card that never retracted. Measured consequence -- the
    // first press opened with an animation and the second and third had none at all, because only the
    // REQUEST was changing and the state never moved. The card is held by a notification (guarded above)
    // or by the panel; once neither wants it, the shape it belongs to is the pill, which is the hover
    // shape -- same rectangle, so the retraction is a plain animation from the card back to it.
    return c.shape == IslandState::Collapsed || c.shape == IslandState::MusicStrip ||
           c.shape == IslandState::Expanded;
}

// True when the island should fall back to its resting shape because the pointer left.
//
// Covers both the card and the widened hover shape; a settled Hover with the pointer outside is the
// case that used to be missed, because only Expanded was handled.
inline bool ShouldReturnToResting(const PointerContext& c) {
    if (c.notificationActive || c.pointerInside) return false;
    return c.shape == IslandState::Expanded || c.shape == IslandState::Hover;
}

// True when the RESTING shape itself has changed and the island is still showing the old
// one -- for example playback started while the pointer was away, so the resting shape is
// now the lyric strip but the island is still the plain pill.
inline bool ShouldAdoptNewResting(const PointerContext& c) {
    if (c.notificationActive || c.pointerInside) return false;
    if (c.shape == c.resting) return false;
    // The NOTICE STRIP is in this list for the reason this whole file exists: a shape that no rule matches
    // is a shape the island stays in for ever. With the pointer away and the message expired there is
    // nothing left asking for the strip, and without this the island sat widened with yesterday's message in
    // it -- found by the policy test the moment the strip was added, which is what the test is for.
    return c.shape == IslandState::Collapsed || c.shape == IslandState::MusicStrip ||
           c.shape == IslandState::NoticeStrip;
}

// The shape the island should move to, or the same shape to leave it alone. One call, one answer,
// so the frame loop has no policy of its own to drift.
//
// Order matters and is the point of having this in one place. A notification outranks everything,
// because a message that arrives while the pointer happens to be resting on the bar must still be
// shown -- an earlier version let the hover win, and the notification was cut off the instant the
// pointer moved.
inline IslandState NextShape(const PointerContext& c) {
    // A FULL-SCREEN APPLICATION GETS THE SCREEN -- except for the two things that must not wait.
    //
    // Asked for in these words: "现在的灵动岛一行的在玩游戏的时候也显示出来了，只有通话 或者 通知的时候
    // 灵动岛显示". While a game is in front the bar paints NOTHING (see Renderer::RenderFrame) and accepts
    // no pointer at all (see IslandWindow::ShouldAcceptPointer), so the island cannot appear over a game
    // and a pointer crossing the top of the screen cannot open a card. A CALL OR A MESSAGE still comes up,
    // because those are the two things a full-screen application is not allowed to swallow.
    //
    // The ORDER HERE IS THE CHANGE. The full-screen state used to outrank a message -- "because there is
    // nowhere else to put a card" -- and that was written when the answer to a message WAS a card. It is a
    // strip now, the strip fits in the band the full-screen application leaves, and the two rules no longer
    // compete for the same space.
    //
    // And a full-screen application NEVER opens the details card, which is the second half of the report:
    // "如果音乐的时候也显示那就鼠标放灵动岛的时候 不要 显示出音乐卡片". Nothing below this point is
    // reached while it is in front, so hovering the island during a game cannot open the media card.
    if (c.fullscreenApp) {
        if (c.notificationActive && c.callActive) return IslandState::Hover;
        // THE PILL, NOT THE FULL-WIDTH BAND.
        //
        // This returned FullBar, and the two rules it was written for contradict each other: FullBar shapes
        // the island to the whole screen width (box 0..2560), while everything else about a full-screen
        // application -- the renderer (no decks, no icon row) and the hit test (only the island is the bar)
        // -- is built for the island ALONE. The result was reported from inside a game with a photograph of
        // a dark band running to both edges of the screen, taking the lyric line with it: "玩游戏的时候
        // 现在显示的不对". The resting state is the pill, which is the shape the full-screen rules already
        // describe, and the band was never asked for.
        return c.notificationActive ? IslandState::NoticeStrip : c.resting;
    }

    // A CALL IS THE BAR CLUSTER, IN EVERY STATE.
    //
    // Its content is an icon, a timer and a level meter; the 420 DIP notice strip was built for a line of
    // text and left 200 DIP of empty glass beside the call. Placed before the notification rule so the call
    // never reaches the strip, and before the card rules so a call is not swallowed by a hover.
    if (c.notificationActive && c.callActive) {
        return IslandState::Hover;
    }

    // Leaving it: the full-width bar is not a shape any other rule knows about, so the state has to
    // come back to something the resting rules can reason about. Without this the bar would be stuck
    // full-width, because no rule below matches FullBar and NextShape would return it unchanged.
    if (c.shape == IslandState::FullBar) {
        return c.resting;
    }

    // The system panel asks for the card, and it outranks a message: it was opened DELIBERATELY, and someone
    // who just pressed the control is waiting for it, while a notification arriving can wait a moment. (The
    // order between these two never mattered before -- both answers were the card -- and it matters now that
    // one of them is the notice strip.)
    if (c.systemPanelOpen) {
        return IslandState::Expanded;
    }
    if (c.notificationActive) {
        // THE STRIP, WHATEVER THE POINTER IS DOING -- the card is gone.
        //
        // "有通知的时候 灵动岛变宽 在里面显示 … 是左右宽展" is what this state is for, and the strip
        // already carries the line (scrolling it when it is longer than the island). Widening into the
        // details card on hover came later, and was removed on request: "这个卡片不需要".
        //
        // What it cost to remove: the notification card was the only thing that showed the full body, and
        // the only caller of the sender's icon (NotificationIcon, which 8.20 hardened). Both went with it
        // rather than being left unreachable -- a card nobody can open is the next template to copy.
        return IslandState::NoticeStrip;
    }
    // ... and the card belongs to the PILL: the pointer resting on an icon or on a deck button is asking
    // about something else, so the card goes away. Checked after the two explicit requests above, because a
    // message must not be hidden by where the pointer happens to be and the panel was opened deliberately.
    if (ShouldGiveUpCard(c)) {
        return c.resting;
    }
    if (ShouldPromoteToCard(c)) {
        return IslandState::Expanded;
    }
    if (ShouldPromoteToTaskbar(c)) {
        return IslandState::Hover;
    }
    if (ShouldReturnToResting(c)) {
        return c.resting;
    }
    if (ShouldAdoptNewResting(c)) {
        return c.resting;
    }
    return c.shape;
}

// The older five-argument form, kept for callers that have no icon row to describe.
inline IslandState NextShape(IslandState shape, IslandState resting, bool pointerInside,
                             bool notificationActive) {
    PointerContext c;
    c.shape = shape;
    c.resting = resting;
    c.pointerInside = pointerInside;
    c.notificationActive = notificationActive;
    // A strip at rest in this older form means "a session is up" -- the window-title source (see
    // PlayerWindow.h) is the only other way to fill the pill's line, and it is newer than this
    // signature, which has no room to say "line, but no card". Callers that need that distinction
    // describe the pointer fully, and the distinction has its own case in the policy test.
    c.musicPresent = resting == IslandState::MusicStrip;
    // With no icon row and no deck described, "the pointer is inside the bar" can only mean "on the island"
    // -- there is nothing else on this bar for it to be on. The distinction is exercised by the callers that
    // DO describe the row, which is where it matters.
    c.pointerOnIsland = pointerInside;
    return NextShape(c);
}

// --- where the date, the weekday, the time and the weather go ------------------------
//
// TWO PLACES, EXACTLY ONE OF THEM, and which one depends on whether the island's own line is
// free. That line holds one thing, and the bar's priority is "what the user would rather not
// miss": a call, then a message, then the lyric. While one of those is up the date and the time
// have nowhere to be on the island, so they stand beside the deck instead; while the island is
// idle they sit in it, which is what the island's own clock has always been.
//
// Asked for in two steps an hour apart -- the strip while the island is occupied ("当放音乐或者
// 灵动岛被占用的时候 就在我红框处显示日期时间星期天气"), and then the island itself at rest ("是灵动岛
// 不占用时 灵动岛内要这么显示").
//
// A PAIR rather than one boolean, because the thing that must not happen is the same words in
// both places at once. Written as one function so "shown exactly once" is a property of the
// rule, and not of two draw sites that happen to agree today.
struct StatusPlacement {
    bool inIsland = true;    // the date, the weekday, the time and the weather, inside the pill
    bool besideDeck = false; // ... or in the band left of the deck, while the pill is busy
};

// `islandLineTaken` is "the pill has something else to say": the lyric strip with words to show,
// or the notice strip carrying a message or a call. The two callers that know this -- the frame
// loop and the renderer -- must use this same test, or the words appear twice or nowhere.
inline StatusPlacement StatusPlacementFor(bool islandLineTaken) {
    StatusPlacement out;
    out.inIsland = !islandLineTaken;
    out.besideDeck = islandLineTaken;
    return out;
}

} // namespace island
