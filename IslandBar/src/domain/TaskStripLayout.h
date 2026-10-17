// =====================================================================
//  TaskStripLayout.h
//  The island, the two buttons on its left, and the row of application
//  icons on its right.
//
//  The bar has one shape and three things anchored to it:
//
//      [ system ][ Start ]   island   [ icon ] [ icon ] [ icon ] ...
//
//  The SYSTEM button is leftmost and the Start button is between it and the
//  island. Both sit immediately against the island's left edge rather than out
//  at the window's margin, so the cluster reads as part of the island: the
//  island is the bar, and these are the controls attached to it.
//
//  The system indicators used to live here too -- up to four of them, out at the
//  window's left margin. They were removed: the request was that the deck hold
//  the Start and system buttons and nothing else, and a volume or battery glyph
//  beside a Start button says less than the panel behind the system button
//  already says. Their reader, their drawing and their hit test went with them
//  rather than being left as dead code.
//
//  The island does not move. That is deliberate and it is the point of the
//  design: an island that shifted sideways every time an application was opened
//  would be worse than useless. The icon row grows to the right from a fixed
//  left edge, so the island is a fixed point and the icons are what change.
//
//  Everything here is arithmetic over a rectangle, so it is tested directly
//  rather than by looking at a screenshot.
// =====================================================================
#pragma once

#include "domain/Geometry.h"
#include "domain/Motion.h"

#include <algorithm>

namespace island {

// The icon row.
//
// A button is square while at rest and widens on hover to reveal a title, which is why there are
// two widths: the resting icon and the hovered button that contains it.
inline constexpr float kTaskIconButtonDip = 30.0f;   // the square, at rest
inline constexpr float kTaskIconSizeDip   = 20.0f;   // the icon inside it
inline constexpr float kTaskIconGapDip    = 4.0f;
inline constexpr float kTaskIconPadDip    = 8.0f;    // between the island and the first icon

// The widened button on hover: icon, gap, then a title.
// The narrowest title box worth drawing a title in.
//
// Below this the text is trimmed to a lone ellipsis, so a button that is still opening would show
// ... and nothing else -- which is what was reported. A title that cannot be read is worse than no
// title: it reads as a rendering fault.
inline constexpr float kTaskTitleMinWidthDip = 44.0f;

inline constexpr float kTaskWideButtonDip = 132.0f;

// How many icons are shown.
//
// Bounded because the row must not reach past the monitor, and because a bar that shrinks its
// icons without limit ends up with a row nobody can read or hit.
//
// This is the CEILING, not the answer. The real limit is whatever fits between the island and the
// right-hand edge of the window, which depends on the monitor width -- so TaskIconsThatFit is what
// callers should use, and this is the absolute cap that exists so a 16K display does not produce a
// forty-button row.
//
// 20 rather than 12: with the row sized on the RESTING button (see TaskIconsThatFitInWidth) a 2560-wide
// screen has room for about thirty, and the cap is not meant to be the thing that decides -- it was 12,
// and the user's "8 icons and no more" turned out to come from the arithmetic below, not from here.
inline constexpr int kMaxTaskIcons = 20;

// How many icons the row can draw at once, INCLUDING the ones fading out.
//
// It lives here rather than in the renderer's FrameState, where it was, because it is a cap on the row --
// a layout fact -- and two other things need to name it: the frame state that carries the buttons, and the
// bar's own state (app/BarState.h) which holds one entry per slot. A state header that has to include a
// render header to learn how many icons fit is the dependency pointing the wrong way; the renderer keeps a
// one-line alias so every existing `FrameState::kMaxTaskButtons` still reads the same and means the same.
inline constexpr int kMaxTaskButtons = 32;

// The row a button occupies, given the width everything before it used up.
//
// One function for both kinds of button, because they are the same button: the offsets are the
// running sum of the widths, exactly as TaskIconButtonAtAmounts does it, and keeping one
// implementation is what stops the two halves of the row from drifting apart by a pixel each.
inline RectF RowButtonAt(float rowLeft, float centreY, const float* widths, int index) {
    float x = rowLeft;
    for (int i = 0; i < index; ++i) {
        x += widths[i] + kTaskIconGapDip;
    }
    RectF box;
    box.left = x;
    box.right = x + widths[index];
    box.top = centreY - kTaskIconButtonDip * 0.5f;
    box.bottom = box.top + kTaskIconButtonDip;
    return box;
}

// How much room the row has, given where it starts and how wide the window is.
//
// The right margin is not decoration: a button flush against the window's edge would have its
// hover-widened title clipped, and the window's own rounded edge would cut it.
inline constexpr float kRowRightMarginDip = 8.0f;

// The margin at the window's left edge, for the same reason as the right one.
inline constexpr float kRowLeftMarginDip = 16.0f;

// --- the left deck's button size ---------------------------------------------------
//
// Deliberately the SAME SIZE as a resting icon button. A button that looks like the app buttons is
// read as part of the same control group, which is what it is -- the bar's decks hold the same kind
// of thing.
inline constexpr float kStartButtonDip = kTaskIconButtonDip;
inline constexpr float kStartIconDip   = kTaskIconSizeDip;

// --- the left deck: the system button and the Start button, against the island -------------
//
// The deck is anchored to the ISLAND's left edge and grows leftwards from it:
//
//      [ system ][ Start ]   island
//
// System first, then Start, then the island. Read left to right that is "what the machine is doing |
// start something | the bar itself", and it puts the control that opens the panel furthest from the
// island's content, so pressing it cannot be confused with pressing something on the island.
//
// Anchoring to the island rather than to the window's margin is what makes the cluster read as part of
// the bar. At the window's margin the two buttons floated alone in a wide empty strip; against the
// island they are attached to the thing they belong to.
//
// Everything on the left is derived from the island's left edge, so the island's own position never
// depends on how many controls the deck holds.
inline float DeckRight(const RoundedBox& island) {
    return island.rect.left - kTaskIconPadDip;
}

// The rectangle of the nth button on the left deck, counted RIGHT to LEFT from the island.
//
// Index 0 is the button nearest the island (Start) and index 1 is the next one left (system), which is
// the order they are read in. Counting from the island rather than from the window's margin means a
// button cannot move because something was added at the far end of the deck.
inline RectF DeckButtonFor(const RoundedBox& island, int index) {
    RectF box;
    box.right = DeckRight(island) - static_cast<float>(index) * (kStartButtonDip + kTaskIconGapDip);
    box.left = box.right - kStartButtonDip;
    const float centreY = (island.rect.top + island.rect.bottom) * 0.5f;
    box.top = centreY - kStartButtonDip * 0.5f;
    box.bottom = box.top + kStartButtonDip;
    return box;
}

// The Start button: nearest the island on its left.
inline RectF StartButtonFor(const RoundedBox& island) {
    return DeckButtonFor(island, 0);
}

// The system panel's toggle: leftmost, immediately before the Start button.
inline RectF SystemButtonFor(const RoundedBox& island) {
    return DeckButtonFor(island, 1);
}

// Whether a window-local DIP point is on the system panel's toggle.
inline bool SystemButtonHit(const RoundedBox& island, float x, float y) {
    const RectF box = SystemButtonFor(island);
    return x >= box.left && x <= box.right && y >= box.top && y <= box.bottom;
}

// Whether a window-local DIP point is on the Start button.
inline bool StartButtonHit(const RoundedBox& island, float x, float y) {
    const RectF box = StartButtonFor(island);
    return x >= box.left && x <= box.right && y >= box.top && y <= box.bottom;
}

inline float RowAvailableWidthDip(float monitorWidthDip, float rowLeft) {
    const float available = monitorWidthDip - kRowRightMarginDip - rowLeft;
    return available > 0.0f ? available : 0.0f;
}

// The width a button occupies when the pointer is on it and it has opened to show its title.
//
// This is the width the row has to be sized against, NOT the resting 30 DIP. A deck sized at rest
// fits 12 icons on a 2560 monitor and then, the moment the pointer touches one, the row grows by
// 102 DIP and either runs off the window or shoves its neighbours sideways. Sizing against the
// opened width means the row's outer edge never moves, which is what "a bar that does not jump"
// actually requires.
// (kRowSlotWidestDip is gone: it was the slot width with EVERY button open, and sizing the row on it was
// the defect -- see TaskIconsThatFitInWidth.)

// How many buttons fit in `availableWidthDip`, allowing for ONE of them being open.
//
// THE RESTING WIDTH, PLUS ONE OPEN ONE. It used to spend the OPENED width on every slot:
// kTaskWideButtonDip is 132 because an open button shows its TITLE, so the row was reserving four times
// the room it uses -- and that was the cap the user hit. Their bar's own log says it in one line:
//
//   "taskbar: state=hover, 8 buttons from 9 windows listed"   (1162 DIP free, 272 DIP used)
//
// Reported as "灵动岛 右侧现在只能显示8个 再开个窗口就不显示了".
inline int TaskIconsThatFitInWidth(float availableWidthDip) {
    const float slot = kTaskIconButtonDip + kTaskIconGapDip;
    const float openExtra = kTaskWideButtonDip - kTaskIconButtonDip;
    // `+ gap` because N slots have N-1 gaps between them; `- openExtra` because one of the N is open.
    const float room = availableWidthDip - openExtra + kTaskIconGapDip;
    if (room <= 0.0f) return 0;
    int fit = static_cast<int>(room / slot);
    if (fit > kMaxTaskIcons) fit = kMaxTaskIcons;
    return fit;
}

// How many application icons fit on the right deck.
//
// Sized against kRowSlotWidestDip, so the count is the number of buttons that fit with the pointer on
// any one of them. The consequence is a smaller cap than the resting geometry would allow -- 12 icons
// fit at rest and 7 fit opened -- and the smaller one is the honest answer, because the larger one is
// only true while nobody is pointing at anything.
//
// The `indicatorCount` parameter is gone with the indicators themselves. It used to reserve their
// width here first, which was correct while they shared this row; keeping the parameter would have
// subtracted room for controls that no longer exist.
inline int TaskIconsThatFit(float availableWidthDip) {
    return TaskIconsThatFitInWidth(availableWidthDip);
}

// The furthest right any button in the row can reach, including the one that is widened.
//
// Kept here rather than in the renderer because the input region needs the same answer: a row
// whose drawing and whose clickable area disagree produces clicks that land beside the thing
// that was clicked, which is a defect that is invisible in a screenshot.
inline float RowRightEdgeDip(float rowLeft, const float* widths, int count) {
    float right = rowLeft;
    for (int i = 0; i < count; ++i) {
        right += widths[i];
        if (i + 1 < count) right += kTaskIconGapDip;
    }
    return right;
}

// The widths of every button in the row: the application icons, and nothing else.
//
// `widths` is filled with `taskCount` entries, all the resting width -- the row is laid out at rest
// and the HOVERED button is the one that widens, which is why the layout takes the resting widths and
// the position of the hovered button separately.
inline void RowWidths(int taskCount, float* widths) {
    for (int i = 0; i < taskCount; ++i) widths[i] = kTaskIconButtonDip;
}

// Where the island itself goes: centred, regardless of how many icons there are.
inline RoundedBox IslandBoxFor(float monitorWidthDip, float pillWidthDip, float pillHeightDip) {
    const float centreX = monitorWidthDip * 0.5f;

    RoundedBox box;
    box.rect.left = centreX - pillWidthDip * 0.5f;
    box.rect.top = kPillTopMarginDip;
    box.rect.right = box.rect.left + pillWidthDip;
    box.rect.bottom = box.rect.top + pillHeightDip;
    box.radiusX = std::min(pillHeightDip * 0.5f, pillWidthDip * 0.5f);
    box.radiusY = box.radiusX;
    return box;
}

// The row of icons, in window-local DIPs, given the island's own box.
//
// `hovered` picks the widened buttons. The row's LEFT edge is fixed either way, so the icons grow
// to the right on hover rather than pushing the island or shuffling sideways.
struct TaskIconRow {
    float left = 0.0f;      // the row's left edge, where the first button starts
    float top = 0.0f;
    float bottom = 0.0f;
    bool  valid = false;
};

inline TaskIconRow TaskRowFor(const RoundedBox& island, int count, float cardLeftDip = 0.0f,
                              float cardWidthDip = 0.0f) {
    TaskIconRow row;
    if (count <= 0) return row;

    (void)cardLeftDip;
    (void)cardWidthDip;

    // The row does NOT move for the card, and that is a decision with a history.
    //
    // The panel first opened to the RIGHT of the system button, 520 DIP wide, which put its body
    // straight over this row -- so the row was pushed past the card to stay visible, and the user
    // reported the icons "all squeezed out". The panel now opens to the LEFT of the island instead, so
    // its body never reaches this row and nothing has to move for it.
    //
    // The parameters are kept because the input region and the hit test share this signature: removing
    // them would leave two call sites computing the row's start by different rules, which is the defect
    // this whole file exists to prevent.
    row.left = island.rect.right + kTaskIconPadDip;

    // Vertically centred on the island, so the row reads as belonging to it.
    const float centreY = (island.rect.top + island.rect.bottom) * 0.5f;
    row.top = centreY - kTaskIconButtonDip * 0.5f;
    row.bottom = row.top + kTaskIconButtonDip;
    row.valid = true;
    return row;
}

// One icon button's rectangle, from EVERY button's own opening amount.
//
// This is the real definition; everything else is a convenience over it.
//
// The width of each button comes from its own amount, and each button starts after the running sum of
// the ones before it. That is the whole design, and it is stated as a rule because both shortcuts
// failed:
//
//  * Offsetting from the ONE hovered button ignores a button that is still CLOSING. A button closes
//    over about 160 ms, so while the pointer moves along the row one button opens and its neighbour is
//    still shrinking. Making room only for the hovered one let the opening button draw straight over
//    its closing neighbour. The LIVE check caught it:
//
//      OVERLAP: button 5 ends at 1641.0 but button 6 starts at 1632.7 (amounts 0.42 / 0.30)
//
//    The unit tests missed it because they only ever passed ONE non-zero amount -- which is precisely
//    the case that cannot occur in the live bar. A test that cannot express the failing case will not
//    find it, however many assertions it contains.
//
//  * A second animated value deciding the push (there used to be one, `shiftAmount`) agreed with the
//    width at rest and at the end, and disagreed for the whole middle of the transition.
//
// Both are the same mistake: laying the row out from a notion of "the" open button instead of from the
// widths the buttons actually have.
inline RectF TaskIconButtonAtAmounts(const RoundedBox& island, int index, int count,
                                     const float* amounts, float cardLeftDip = 0.0f,
                                     float cardWidthDip = 0.0f) {
    if (index < 0 || index >= count || !amounts) return RectF{};

    const TaskIconRow row = TaskRowFor(island, count, cardLeftDip, cardWidthDip);
    if (!row.valid) return RectF{};

    const auto widthAt = [&](int i) {
        float amount = amounts[i];
        if (amount < 0.0f) amount = 0.0f;
        if (amount > 1.0f) amount = 1.0f;
        return kTaskIconButtonDip + (kTaskWideButtonDip - kTaskIconButtonDip) * amount;
    };

    float left = row.left;
    for (int i = 0; i < index; ++i) left += widthAt(i) + kTaskIconGapDip;

    RectF box;
    box.left = left;
    box.right = left + widthAt(index);
    box.top = row.top;
    box.bottom = row.bottom;
    return box;
}

// One icon button's rectangle, from ONE animated index and how far it has opened.
//
// A convenience over `TaskIconButtonAtAmounts`, which is the real definition. Kept because most callers
// have a single hovered button, and because the unit tests express their cases this way.
inline RectF TaskIconButtonAt(const RoundedBox& island, int index, int count, int hoveredIndex,
                              float hoveredAmount = 1.0f, float cardLeftDip = 0.0f,
                              float cardWidthDip = 0.0f) {
    float amounts[kMaxTaskIcons] = {};
    const int shown = count < kMaxTaskIcons ? count : kMaxTaskIcons;

    float amount = hoveredAmount;
    if (amount < 0.0f) amount = 0.0f;
    if (amount > 1.0f) amount = 1.0f;
    // Every button at rest, except the one named.
    for (int i = 0; i < shown; ++i) amounts[i] = (i == hoveredIndex) ? amount : 0.0f;

    return TaskIconButtonAtAmounts(island, index, count, amounts, cardLeftDip, cardWidthDip);
}

// The icon's square inside a button.
//
// The icon's square inside a button, given that button's rectangle.
//
// The icon is POSITIONED FROM THE BUTTON IT BELONGS TO, so it travels with it. That sounds obvious and
// it was not what the code did: the icon was placed from the button's RESTING rectangle while the
// button itself was placed from its animated one, so as soon as the row made room for a neighbour the
// button slid away and left its icon behind. On screen that reads as the background moving while the
// icons stay put, which is how it was reported.
//
// The reason it was written that way was a real concern -- an icon that drifted sideways while the
// pointer was on it would move the very thing being pointed at. That concern is answered elsewhere:
// the button being pointed at keeps its own left edge (only the buttons AFTER it are pushed along), so
// its icon does not move either. See TaskIconButtonAtAmounts.
inline RectF TaskIconInBox(const RectF& button) {
    if (button.right <= button.left) return RectF{};

    const float centreY = (button.top + button.bottom) * 0.5f;
    RectF icon;
    icon.left = button.left + (kTaskIconButtonDip - kTaskIconSizeDip) * 0.5f;
    icon.top = centreY - kTaskIconSizeDip * 0.5f;
    icon.right = icon.left + kTaskIconSizeDip;
    icon.bottom = icon.top + kTaskIconSizeDip;
    return icon;
}

// The icon's square inside a button, for callers that have the button's rectangle.
inline RectF TaskIconAtInButton(const RoundedBox& island, int index, int count) {
    return TaskIconInBox(TaskIconButtonAt(island, index, count, -1));
}

// Where a title goes, using each button's own opening amount.
//
// The box grows with the button, so it is derived from the same widths the button is drawn with. Using
// one "hovered" amount instead would place the title against a row that is not the row on screen.
inline RectF TaskTitleAtAmounts(const RoundedBox& island, int index, int count, const float* amounts,
                               int hoveredIndex, float cardLeftDip = 0.0f,
                               float cardWidthDip = 0.0f) {
    if (index != hoveredIndex || !amounts) return RectF{};

    const RectF button =
        TaskIconButtonAtAmounts(island, index, count, amounts, cardLeftDip, cardWidthDip);
    const RectF icon = TaskIconInBox(button);
    if (button.right <= button.left) return RectF{};

    RectF title;
    title.left = icon.right + 8.0f;
    title.right = button.right - kTaskIconPadDip;
    title.top = button.top;
    title.bottom = button.bottom;
    return title;
}

// The icon's square inside a button, from the button's own animated rectangle.
//
// Same geometry as `TaskIconInBox`, reached from an index and an amount so the callers that describe a
// single hovered button can use it.
inline RectF TaskIconAt(const RoundedBox& island, int index, int count, int hoveredIndex = -1,
                        float hoveredAmount = 1.0f) {
    return TaskIconInBox(TaskIconButtonAt(island, index, count, hoveredIndex, hoveredAmount));
}

// Where the title goes, inside the growing button.
inline RectF TaskTitleAt(const RoundedBox& island, int index, int count, int hoveredIndex,
                         float hoveredAmount = 1.0f, float cardLeftDip = 0.0f,
                         float cardWidthDip = 0.0f) {
    if (index != hoveredIndex) return RectF{};

    const RectF button = TaskIconButtonAt(island, index, count, hoveredIndex, hoveredAmount,
                                          cardLeftDip, cardWidthDip);
    const RectF icon = TaskIconInBox(button);
    if (button.right <= button.left) return RectF{};

    RectF title;
    title.left = icon.right + 8.0f;
    title.right = button.right - kTaskIconPadDip;
    title.top = button.top;
    title.bottom = button.bottom;
    return title;
}

// How wide the row is with a given button widened.
//
// Used to check that a widened button cannot push the row off the monitor, and to size the hover
// strip so the shape does not change when the pointer moves along the row.
inline float TaskRowWidthDip(int count, int hoveredIndex) {
    if (count <= 0) return 0.0f;
    float total = 0.0f;
    for (int i = 0; i < count; ++i) {
        total += (i == hoveredIndex) ? kTaskWideButtonDip : kTaskIconButtonDip;
        if (i + 1 < count) total += kTaskIconGapDip;
    }
    return total;
}

// Which icon a window-local DIP point lands on, or -1.
//
// The whole button is the target rather than just the icon: a button that can only be hit on its
// picture is a button that feels broken.
inline int TaskIconHit(const RoundedBox& island, int count, int hoveredIndex, float hoveredAmount,
                       float x, float y) {
    for (int i = 0; i < count; ++i) {
        const RectF button = TaskIconButtonAt(island, i, count, hoveredIndex, hoveredAmount);
        if (button.right <= button.left) continue;
        if (x >= button.left && x <= button.right && y >= button.top && y <= button.bottom) {
            return i;
        }
    }
    return -1;
}

// Which icon the pointer is CHOOSING, decided on the RESTING layout.
//
// This is deliberately not TaskIconHit with the animated layout, and the difference is the whole
// point.
//
// Deciding "which icon is the pointer on" from a layout that has already moved for the current
// answer therefore closes a loop: the pointer moves towards the second icon, but the first one's
// growth has pushed the second one further along, so the pointer is still inside the first one's
// extent -- the first stays open and the second can never be reached. The reported symptom was
// exactly that: the pointer arrives at the second icon and something else opens.
//
// On the resting layout the answer is a function of the pointer's position alone and does not depend
// on the answer it is about to produce.
inline int TaskIconUnderPointer(const RoundedBox& island, int count, float x, float y) {
    return TaskIconHit(island, count, -1, 0.0f, x, y);
}

// Whether a point is anywhere on the row, on the resting layout.
//
// Used by the window's hit test, which decides whether the bar accepts the pointer at all. It must
// use the resting layout for the same reason the choice above does: a hit test whose geometry moves
// as the pointer moves makes the bar flicker in and out of click-through.
inline bool TaskRowContains(const RoundedBox& island, int count, float x, float y,
                             float cardLeftDip = 0.0f, float cardWidthDip = 0.0f) {
    if (count <= 0) return false;
    // The row's vertical extent, taken from the widest case so a widened button's title is included.
    const TaskIconRow row = TaskRowFor(island, count, cardLeftDip, cardWidthDip);
    if (!row.valid) return false;
    if (y < row.top || y > row.bottom) return false;

    // Horizontally: from the row's left edge to past the last resting button. A widened button
    // extends further right, and that extra is included so the title is part of the bar.
    const float lastRestingRight =
        TaskIconButtonAt(island, count - 1, count, -1, 0.0f).right;
    const float lastWideRight =
        TaskIconButtonAt(island, count - 1, count, count - 1, 1.0f).right;
    const float right = lastWideRight > lastRestingRight ? lastWideRight : lastRestingRight;
    return x >= row.left && x <= right;
}

// The icon to keep open, given where the pointer is and whichever icon is already open.
//
// The resting layout says which button the pointer has MOVED ON TO, but it cannot say whether the
// pointer is STILL on the row once the row has opened: opening pushes everything after the open
// button to the right, so the button that was under the pointer moves out from under it. Judging only
// from the resting layout therefore reports "the pointer is between buttons" and closes the button
// the user is pointing at -- which showed up as the row flickering and, worse, as the island
// expanding, because "not on an icon" is what permits the island to open.
//
// So the two questions are separated:
//
//   * the resting layout says which button the pointer has ARRIVED at;
//   * the row's full extent says whether the pointer is still on the ROW at all.
//
// `current` is kept unless the pointer has genuinely arrived at a different button. That is the
// hysteresis which stops the two layouts from fighting each other.
inline int TaskIconToKeepOpen(const RoundedBox& island, int count, int current, float x, float y) {
    if (count <= 0) return -1;

    const int arrived = TaskIconUnderPointer(island, count, x, y);
    if (arrived >= 0) return arrived;

    // Not on a resting button, but still on the row -- including the extra room a grown button takes,
    // so the pointer resting on an open button's title counts as still being on it.
    if (current >= 0 && TaskRowContains(island, count, x, y)) return current;

    return -1;
}

// How long an icon takes to arrive or to leave the row.
//
// The row's membership changes in a single frame otherwise: a window opens and its icon is simply THERE,
// a window closes and it is simply gone. Reported as "灵动岛右侧 图标出现和消失有点太突兀了没有动画".
inline constexpr float kTaskIconAppearSeconds = 0.22f;

// One step of an icon's appearance. `amount` moves towards 1 while the window is listed and towards 0
// while it is not, at a fixed rate, so arrivals and departures take the same time.
//
// A pure function because the defect it fixes cannot be seen in a still: "an icon that jumps in" and "an
// icon that fades in" are the same picture once the fade is over.
inline float TaskIconAppearStep(float amount, bool arriving, float dtSeconds) {
    const float dt = dtSeconds > 0.1f ? 0.1f : (dtSeconds < 0.0f ? 0.0f : dtSeconds);
    const float step = dt / kTaskIconAppearSeconds;
    const float target = arriving ? 1.0f : 0.0f;
    if (amount < target) {
        const float next = amount + step;
        return next > target ? target : next;
    }
    if (amount > target) {
        const float next = amount - step;
        return next < target ? target : next;
    }
    return amount;
}

// How many icons fit before the row would leave the monitor, allowing for ONE widened button.
//
// The widened button is what has to fit, not the resting one: the row must not run off the screen
// when the pointer lands on its last icon, which is exactly when it is at its widest.
//
// `wanted` is clamped rather than ignored: the caller says how many windows there are and this says
// how many of them can be shown, so the answer is the smaller of the two. Only the OPENED width is
// spent here -- the row is sized so that any single button can open without the row growing.
inline int TaskIconsThatFit(float monitorWidthDip, int wanted) {
    if (wanted <= 0) return 0;

    // The island is centred, so the row has everything from the island's right edge to the monitor's
    // right edge, less the padding and a small margin.
    const float centreX = monitorWidthDip * 0.5f;
    const float islandRight = centreX + (kCollapsedWidthDip * 0.5f);
    const float available = monitorWidthDip - islandRight - kTaskIconPadDip - 8.0f;
    if (available <= 0.0f) return 0;

    int fit = TaskIconsThatFitInWidth(available);
    if (fit < 1) fit = 1;   // one icon always fits; a row that shows nothing is worse
    return fit < wanted ? fit : wanted;
}

// --- the status strip: the date, the weekday, the time and the weather --------
//
// WHY IT IS HERE. The island's one line holds the clock when the bar is idle and
// something else -- the lyric, a message, a call -- when it is not, so while the island is
// OCCUPIED the bar carries no time at all; and the date, the weekday and the weather have
// never had anywhere to live. Asked for with a photograph of the empty band left of the
// system button: "当放音乐或者灵动岛被占用的时候 就在我红框处显示日期时间星期天气".
//
// SHOWN EXACTLY ONCE, AND THE PILL COMES FIRST. The two places this line can live are the
// island itself and this band, and they are exclusive: at rest the PILL shows the date, the
// weekday, the time and the weather (there is nothing else for its one line to say), and the
// moment that line is taken -- by the lyric, a message, a call -- this strip shows the same
// words beside the deck instead. Asked for in two steps, an hour apart:
//
//     "当放音乐或者灵动岛被占用的时候 就在我红框处显示日期时间星期天气"   [the strip]
//     "是灵动岛不占用时 灵动岛内要这么显示"                            [the pill]
//
// The first version had only the strip, so the date and the weather were visible while the
// island was busy and nowhere at all while it was idle -- which is exactly backwards, and is
// why this rule is now written down as a pair (StatusPlacementFor) rather than as a
// condition at each of the two draw sites.
//
// ANCHORED TO THE SYSTEM BUTTON, like the deck it stands beside, and for the same
// reason: that band belongs to the bar's left cluster rather than floating out at the
// window's margin, and the island's own position must not depend on it. The strip's
// right edge is a fixed gap left of the system button, so the two never touch and the
// strip reads as one more control of the same group.
//
// A FIXED WIDTH, deliberately. The strip's contents change -- the weather arrives late,
// the date rolls over at midnight, a longer weekday comes round -- and a box that
// resized for each of them would make the bar twitch at midnight and again when the
// first reading lands. The text is centred in the fixed box instead, and the self-test
// measures the longest line the strip can print against that box, so a font or a wording
// that outgrew it fails the build instead of clipping a word.
inline constexpr float kStatusStripWidthDip  = 268.0f;
inline constexpr float kStatusStripHeightDip = 26.0f;
inline constexpr float kStatusStripGapDip    = 10.0f;   // between the strip and the system button
// The run's own inset inside whichever box it is drawn in.
//
// 8, and the number is set by the PILL rather than by the band: the same run is drawn inside a
// 220 DIP pill (see StatusPlacementFor), where every DIP of inset is a DIP of room the widest run
// does not have -- measured at 196 DIP of the 204 that 8 DIP of inset leaves. The band is 268
// wide, so its own inset hardly matters; the run is centred either way, so the inset is not a
// layout choice, it is the clearance the text keeps from the capsule's rounded ends.
inline constexpr float kStatusStripPadDip    = 8.0f;

// The strip's rectangle, in window-local DIPs. Vertically centred on the island, like
// the deck buttons, so the whole left cluster shares one line.
//
// A RoundedBox rather than a RectF, because this is also what the strip is DRAWN into and
// the text layer takes one box: a caller that had to wrap it in a radius of its own would be
// a second place that knows the shape is a stadium.
inline RoundedBox StatusStripFor(const RoundedBox& island) {
    const RectF system = SystemButtonFor(island);

    RoundedBox strip;
    strip.rect.right = system.left - kStatusStripGapDip;
    strip.rect.left = strip.rect.right - kStatusStripWidthDip;

    const float centreY = (island.rect.top + island.rect.bottom) * 0.5f;
    strip.rect.top = centreY - kStatusStripHeightDip * 0.5f;
    strip.rect.bottom = strip.rect.top + kStatusStripHeightDip;
    strip.radiusX = kStatusStripHeightDip * 0.5f;
    strip.radiusY = strip.radiusX;
    return strip;
}

// How long the strip takes to come and go.
//
// It has a fade of its own BECAUSE it is the one thing on this bar that comes and goes with
// the ISLAND's line rather than with the bar: music starting moves the date and the time out
// of the pill and into this band, and the user's standing request on every appearance on this
// bar is the same -- nothing may appear in a single frame. 0.2 s is short enough that the
// words have arrived by the time the eye moves to them, and long enough to read as a fade
// rather than as a blit.
inline constexpr float kStatusStripFadeSeconds = 0.20f;

// One step of the strip's fade, from the same family as TaskIconAppearStep: `amount` moves
// towards 1 while the pill's line is taken and towards 0 while it is free, at a fixed rate,
// so the two directions take the same time.
//
// Pure, because "it pops in" and "it fades in" are the same picture once the fade is over --
// the only place the difference can be checked is in the arithmetic.
inline float StatusStripFadeStep(float amount, bool wanted, float dtSeconds) {
    const float dt = dtSeconds > 0.1f ? 0.1f : (dtSeconds < 0.0f ? 0.0f : dtSeconds);
    const float step = dt / kStatusStripFadeSeconds;
    const float target = wanted ? 1.0f : 0.0f;
    if (amount < target) {
        const float next = amount + step;
        return next > target ? target : next;
    }
    if (amount > target) {
        const float next = amount - step;
        return next < target ? target : next;
    }
    return amount;
}

// --- a spring, for the row's own opening --------------------------------------------------------
//
// The pointed-at button's opening used to be a LINEAR ramp: 1/0.16 of the way per second, clamped at the
// ends. It is smooth and it is what "160 ms to open" sounds like, and it is also the single most
// mechanical-looking movement on the bar -- measured against the reference, where nothing arrives at a
// constant speed and stops dead. A spring does the one thing a ramp cannot: it settles, with a little
// residual motion, which is what the eye reads as weight.
//
// Critically-damped-ish on purpose: zeta 0.8 gives about 1.5% of overshoot -- 1.5 DIP on a button that
// opens by 102, which is visible as a settle and not as a bounce. The hit tests share this value (they
// always have: one amount drives the width and the push-along), so an overshoot of a pixel and a half is
// also the largest disagreement between what is drawn and what is clicked, for one frame.
//
// Integrated in fixed sub-steps rather than with the frame's own dt: at 20 ms a frame the explicit
// integration of a 26 rad/s spring overshoots the overshoot and rings forever, and the frame time on this
// machine has been measured as high as 139 ms. A 1/240 s sub-step is 40 multiplications per button per
// frame in the worst case, and it cannot go unstable.
inline constexpr float kRowSpringOmega = 32.0f;      // rad/s: about 0.19 s to the eye's arrival and 0.3 s
                                                     // to the last pixel -- the button's opening, which is
                                                     // a movement rather than a reaction
inline constexpr float kControlSpringOmega = 48.0f;  // rad/s: about 0.13 s -- a REACTION, and the rule of
                                                     // thumb is that anything the pointer causes should be
                                                     // over inside 180 ms or it feels laggy
inline constexpr float kSpringZeta = 0.8f;

// One step. Returns true while it is still moving, which is the caller's "keep drawing" flag.
inline bool SpringStep(float* value, float* velocity, float target, float dtSeconds, float omega) {
    const float damping = 2.0f * kSpringZeta * omega;
    const float substep = 1.0f / 240.0f;
    float remaining = dtSeconds > 0.1f ? 0.1f : (dtSeconds < 0.0f ? 0.0f : dtSeconds);
    while (remaining > 0.0f) {
        const float h = remaining > substep ? substep : remaining;
        const float error = *value - target;
        const float accel = -omega * omega * error - damping * (*velocity);
        *velocity += accel * h;
        *value += *velocity * h;
        remaining -= h;
    }
    const bool moving = std::fabs(*value - target) > 0.001f || std::fabs(*velocity) > 0.002f;
    if (!moving) {
        *value = target;
        *velocity = 0.0f;
    }
    return moving;
}

inline bool RowOpenStep(float* value, float* velocity, float target, float dtSeconds) {
    return SpringStep(value, velocity, target, dtSeconds, kRowSpringOmega);
}

inline bool ControlWashStep(float* value, float* velocity, float target, float dtSeconds) {
    return SpringStep(value, velocity, target, dtSeconds, kControlSpringOmega);
}

// --- the system tray: the applications' own tray icons, at the right-hand END of the bar -------
//
// WHERE IT GOES, and why it is anchored to the MONITOR rather than to the row beside it. The
// application icon row grows and shrinks with the windows that are open, so a tray hung off its right
// edge would slide sideways every time a window opened -- and the tray is the one part of this bar whose
// position the user learns by muscle memory. Anchored to the monitor's right edge it never moves, which
// is also how the shell's own tray behaves.
//
// The two groups still have to not collide, and that is the row's problem: the row is sized from the
// room left over after this much is reserved (see TaskIconsThatFitInWidth's caller in main.cpp).
// The tray's button, and it is a BEAD IN THE SAME ROW as the application icons: the same 30 DIP square
// they use, so the row's rhythm does not break where the tray begins and so the smallest click target on
// the bar is not the smallest thing on it. It was 22 -- one size down from everything else, measured
// against a screenshot of the whole bar: the tray's three dots read as a bead that had been scaled down
// by accident, and it was the one control whose target was smaller than Windows' own guidance.
inline constexpr float kTrayIconDip         = 30.0f;   // the button; the picture inside is smaller
inline constexpr float kTrayPictureDip      = 16.0f;
inline constexpr float kTrayGapDip          = 2.0f;
inline constexpr float kTrayRightMarginDip  = 6.0f;
// Bounded, because an unbounded list is a list nobody can draw -- but generously, and the PAGE fits whatever
// is inside this bound by shrinking its cells (see TrayPageFor). It used to be 10, written when the tray was
// a ROW along the bar; the page dropped everything past the tenth and said nothing, which is how this machine
// came to show ten of its sixteen icons ("托盘卡片里没有显示全啊").
inline constexpr int   kMaxTrayIcons        = 24;

// ONE BUTTON, and the icons behind it, which is the second half of what the user asked for:
//
//     "在红框位置创建一个按钮 点击按钮弹出卡片 卡片里是这些托盘"
//
// The first version drew every tray icon inline at the right-hand end -- ten of them, and a machine with
// more running applications than that had nowhere to put the rest. A button and a card is also what the
// shell itself does (the chevron over the notification area), and it keeps the bar's right-hand end a
// fixed, readable width however many applications are running.
//
// The BUTTON stands immediately to the RIGHT of the island, mirroring the deck on its left:
//
//     [ system ][ Start ]   island   [ tray ]  [ icon ] [ icon ] ...
//
// Anchored to the island's own right edge, exactly as the Start and system buttons are anchored to its
// left edge, so the bar's cluster reads as one thing. Asked for twice: first "在红框位置创建一个按钮" (the
// red box was at the end of a FULL application row, which made it look like the right-hand margin), and
// then "位置不对 应该放在灵动岛右侧".
//
// It is NOT at the window's right margin: that version was built and looked like a stray control floating
// at the far end of the bar, away from everything it belongs to.
inline RectF TrayButtonFor(const RoundedBox& island) {
    RectF box;
    box.left = island.rect.right + kTaskIconPadDip;
    box.right = box.left + kTrayIconDip;
    // On the island's own line, like the deck buttons and the application icons: one row, one centre.
    const float centreY = (island.rect.top + island.rect.bottom) * 0.5f;
    box.top = centreY - kTrayIconDip * 0.5f;
    box.bottom = box.top + kTrayIconDip;
    return box;
}

inline bool TrayButtonHit(const RoundedBox& island, float x, float y) {
    const RectF box = TrayButtonFor(island);
    return x >= box.left && x <= box.right && y >= box.top && y <= box.bottom;
}

// The room the button takes beside the island, for the row's own sizing: the button, the gap between it
// and the icon row, AND the gap between the button and the edge of the one surface the fixed controls
// stand on (see BarClusterFor). The last one is why this is not simply the button's own width plus a gap:
// the row has to start clear of that surface, not 4 DIP inside its rounded end.
inline float TrayButtonReservedDip() { return kTrayIconDip + kTaskIconGapDip * 2.0f; }

// THE ONE SURFACE THE BAR'S FIXED CONTROLS STAND ON: the system button, the Start button, the island and
// the tray button, as a single rounded rectangle.
//
// Those four never move relative to each other -- the deck hangs off the island's left edge and the tray
// off its right -- so drawn separately they were four beads of the same colour with 4-8 DIP gaps, which
// reads as a dotted line rather than as one object. iOS's island is one shape; this is the same idea
// applied to the part of this bar whose geometry is fixed.
//
// The application icon row is deliberately NOT part of it: the row's length changes with the windows that
// are open, and a surface whose edge grows and shrinks with them is the "things move on their own" this
// project has been told off for twice.
//
// The pad is the row's own 4 DIP, so the cluster is one "block" in the bar's rhythm: 8 around the island
// inside it, 4 everywhere else, and 4 DIP clear of it before the icon row (which is what
// TrayButtonReservedDip adds the second `kTaskIconGapDip` for).
inline RoundedBox BarClusterFor(const RoundedBox& island, bool trayButtonShown) {
    RoundedBox out;
    out.rect.left = SystemButtonFor(island).left - kTaskIconGapDip;
    out.rect.right = (trayButtonShown ? TrayButtonFor(island).right : island.rect.right) + kTaskIconGapDip;
    out.rect.top = island.rect.top;
    out.rect.bottom = island.rect.bottom;
    // A stadium, from the island's own height: the ends stay true semicircles whatever the height is, and
    // the deck's 30 DIP buttons sit inside it with 3 DIP above and below.
    out.radiusX = (out.rect.bottom - out.rect.top) * 0.5f;
    out.radiusY = out.radiusX;
    return out;
}

// Whether the deck and the tray button are part of the surface this frame.
//
// One predicate rather than two copies of `opacity > 0.01 && !fullscreen`: the renderer fills the
// cluster, and the material window behind the bar is placed from the SAME cluster. If the two disagreed,
// the blur would end where the drawn surface does not -- a blurred strip beside a sharp one, which is
// exactly the kind of seam a second window makes easy to introduce and hard to see in a log.
inline bool IslandSurfaceShowsDecks(float contentOpacity, bool fullscreenApp) {
    return contentOpacity > 0.01f && !fullscreenApp;
}

// WHAT THE BAR'S OWN SURFACE IS, this frame: the pill, the notice strip, or the cluster that also holds
// the deck and the tray button -- whichever is actually drawn.
//
// The animated box is only the outline while the notice strip is what is being announced (RowAnchorBox
// answers the resting pill for every other wider state, including the card). The cluster then follows
// whatever that outline is. Two callers use this and must agree pixel for pixel: the renderer
// (Renderer::RenderFrame) and the glass window placed under it (app/GlassWindow through BarLoop).
inline RoundedBox IslandSurfaceFromPillBox(const RoundedBox& pillBox, bool showsDecks,
                                           bool trayButtonShown) {
    return showsDecks ? BarClusterFor(pillBox, trayButtonShown) : pillBox;
}

inline RoundedBox IslandSurfaceFor(const RoundedBox& animated, const RoundedBox& restingPill,
                                   bool showsDecks, bool trayButtonShown) {
    const RoundedBox pillBox = (restingPill.rect.right > restingPill.rect.left)
                                   ? RowAnchorBox(animated, restingPill)
                                   : animated;
    return IslandSurfaceFromPillBox(pillBox, showsDecks, trayButtonShown);
}

// The box the application icon row is laid out from, given that the tray button stands between the island
// and the row.
//
// ONE number, and everything the row does -- its left edge, every button, every hit test and every
// assertion -- follows from it, because they all measure from this box's right edge. That is why the
// button needed no parameter threaded through the row's own functions: the row is told where it starts,
// and where it starts is now one button further along.
inline RoundedBox RowAnchorWithTray(const RoundedBox& island, bool trayButtonShown) {
    if (!trayButtonShown) return island;
    RoundedBox out = island;
    out.rect.right += TrayButtonReservedDip();
    return out;
}

// WHERE AN OUTER CARD HANGS FROM: the centre of the control that opened it.
//
// One card, one rule, and three controls: the panel hangs from the system button, the tray page from the
// tray button, and the Start card from the START button. They are spread across the whole bar --
//
//     [ system ][ Start ]   island   [ tray ]  [ icon ] ...
//
// so an answer taken from the wrong one puts the card a few hundred DIP away from the button that was
// pressed. Reported twice, once for each wrong answer: "点击托盘按钮 弹出的卡片不在这个按钮下面" (the tray
// page, centred on the system button) and, photographed rather than reported, the Start card appearing
// under the system button at 853..1373 when the Start button is at 1132..1162.
//
// An enum rather than a bool, so a fourth level cannot be added by somebody who reads `trayPage ? tray :
// system` and assumes the two cases exhaust it -- which is exactly how the Start card got the panel's
// position.
enum class CardHang { SystemButton, StartButton, TrayButton };

inline float DeckCardAnchorCentre(const RoundedBox& island, CardHang hang) {
    RectF button = {};
    switch (hang) {
        case CardHang::StartButton: button = StartButtonFor(island); break;
        case CardHang::TrayButton: button = TrayButtonFor(island); break;
        case CardHang::SystemButton:
        default: button = SystemButtonFor(island); break;
    }
    return (button.left + button.right) * 0.5f;
}

} // namespace island
