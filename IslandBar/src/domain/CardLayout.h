// =====================================================================
//  CardLayout.h
//  Where the interactive parts of the expanded card are.
//
//  This lives apart from both the window and the renderer because three different
//  places need to agree on it exactly:
//
//    * the renderer draws the transport button,
//    * WM_NCHITTEST decides whether a click lands on the card at all,
//    * the frame loop decides whether a click was the transport button or
//      somewhere else on the card.
//
//  If those disagree, the button is either unpaintable or unclickable, and the
//  symptom looks like a rendering bug. One definition, one source of truth --
//  the same rule that Geometry.h's rounded-rectangle test follows.
// =====================================================================
#pragma once

#include "domain/Geometry.h"
// The tray page's grid is bounded by the same limit the tray itself has (kMaxTrayIcons), so the two cannot
// disagree about how many icons exist.
#include "domain/TaskStripLayout.h"

namespace island {

// --- the lower band of the card --------------------------------------
//
// The card's top half carries the clock and date. Everything below is content:
// either a notification or the now-playing display. Both start at the same line so
// the card does not appear to jump when one replaces the other.
inline constexpr float kCardContentTopFraction    = 0.52f;
inline constexpr float kCardContentBottomFraction = 0.94f;

// Album art: a square at the left of the content band.
inline constexpr float kArtworkSizeDip = 44.0f;
inline constexpr float kArtworkInsetDip = 14.0f;

// The transport buttons: three squares at the right of the content band, sized to
// be comfortable click targets rather than to match the text.
inline constexpr float kTransportButtonDip = 26.0f;
inline constexpr float kTransportGapDip = 2.0f;

// The progress bar, drawn as a hairline across the bottom of the content band.
inline constexpr float kProgressBarHeightDip = 3.0f;

// Window-local DIPs, derived from the pill's box.
struct CardContentLayout {
    RectF content;      // the whole lower band
    RectF artwork;      // album art square (empty when there is no art)
    RectF text;         // where title and secondary line go, right of the artwork
    RectF previous;     // previous track
    RectF playPause;    // play/pause
    RectF next;         // next track
    RectF progress;     // the progress bar
};

// Which control a point landed on.
enum class CardControl {
    None = 0,
    Previous,
    PlayPause,
    Next,
};

// Which card the bar is showing. One card, four answers: nothing, the panel, a message, or what is playing.
//
// Declared HERE rather than in Renderer.h, where it used to live, because it is not the renderer's business
// alone: the WINDOW needs it too (the cursor is decided there and has to know which card's controls are the
// ones on screen -- see IslandWindow::SetCardOwner), and a window header that had to include the renderer to
// name a card would be the wrong way round. This header is the one thing they all already share.
enum class CardOwner {
    None = 0,
    SystemPanel,   // opened deliberately from the left deck
    Notification,  // a message that arrived
    Music,         // what is playing, shown once no message is in the way
};

// --- the tray page: the applications' tray icons, inside the panel's own card ------------------
//
// A PAGE of the panel rather than a fourth card, for the same reason the 声音输出 page is one: it is the
// same card in the same place with the same open/close animation, and only its contents change -- which is
// what the shell's own flyouts do too, and it means no new card geometry, no new policy rule and no new
// transition. See Renderer::RenderFrame's panel branch.
//
// A WRAPPING GRID, because the number of icons is whatever the user has running: three to a row in the
// system panel's own tile size would hold six, and the machine this was built on has sixteen.
inline constexpr int   kTrayPageColumns      = 5;
inline constexpr float kTrayPageCellDip      = 48.0f;   // the cell a 16 DIP picture sits in
inline constexpr float kTrayPageCellGapDip   = 6.0f;
// The grid FITS itself to the card (see TrayPageFor), and these are its two limits: it may use more columns
// than kTrayPageColumns when that is what makes the rows fit, and a cell may not shrink past the point where
// the 24 DIP picture inside it is still a target a finger can find.
inline constexpr int   kTrayPageMaxColumns   = 8;
inline constexpr float kTrayPageMinCellDip   = 38.0f;
// The ceiling for a cell that has room to grow (see the growth step in TrayPageFor): an icon page shares
// the card's space like the panel's tiles do, up to here. It is also the size at which the picture inside
// stops growing -- see the icon side in DrawTrayPage, which is half the cell.
inline constexpr float kTrayPageMaxCellDip   = 68.0f;

struct TrayPageLayout {
    RectF title;          // the page's own line, at the top
    RectF cells[kMaxTrayIcons];
    int   count = 0;
    int   columns = 0;
};

// Where the page's contents go, inside the card the renderer was given. Pure, so the hit test and the
// drawing cannot disagree about which cell an icon is in.
inline TrayPageLayout TrayPageFor(const RoundedBox& card, int iconCount) {
    TrayPageLayout layout;
    if (iconCount < 0) iconCount = 0;
    if (iconCount > kMaxTrayIcons) iconCount = kMaxTrayIcons;
    layout.count = iconCount;
    if (iconCount == 0) return layout;

    const float cardWidth = card.rect.right - card.rect.left;
    const float titleHeight = 34.0f;
    // 16 DIP, the music card's own padding, written here as a literal because that constant is declared
    // further down this file and a page's heading is not worth reordering the file for.
    layout.title.left = card.rect.left + 16.0f;
    layout.title.right = card.rect.right - 16.0f;
    layout.title.top = card.rect.top + 6.0f;
    layout.title.bottom = layout.title.top + titleHeight - 8.0f;

    // THE GRID FITS THE CARD, however many icons there are.
    //
    // It did not, and the limit was invisible: five columns times two rows held ten, this machine has
    // sixteen, and the page drew the first ten and silently dropped the rest -- reported as "托盘卡片里没有
    // 显示全啊" over a photograph of five-by-two with empty space beneath it. The cap that dropped them
    // (kMaxTrayIcons used to be 10) was written for the ROW this page replaced, and nothing here checked
    // that the grid it computed was inside the card it was drawn in. The self-test did not catch it because
    // its counts were 1, 3, 6 and 10 -- exactly the numbers that fit. (Same lesson as HANDOFF 8.71: an
    // assertion whose parameters are the lucky ones measures the luck.)
    //
    // So the column count and the cell size are DERIVED, from the card: as many columns as the width takes,
    // up to kTrayPageMaxColumns, and the cell that makes those rows fit the height, never below
    // kTrayPageMinCellDip -- and if the cell would go below it, one more column (one fewer row) and try
    // again. Everything below is measured from the card, so this cannot disagree with `TrayPageCellAt`
    // (which calls this function) and cannot overflow however many icons the registry holds.
    const float cardHeight = card.rect.bottom - card.rect.top;
    const float availableWidth = cardWidth - 32.0f;              // 16 DIP either side
    const float availableHeight = cardHeight - titleHeight - 18.0f;

    int columns = kTrayPageColumns < iconCount ? kTrayPageColumns : iconCount;
    float cell = kTrayPageCellDip;
    if (iconCount > 1) {
        // FULL SIZE FIRST. A grid whose cells are still 48 DIP beats one that fits in fewer columns by
        // shrinking them: sixteen icons came out as 5/5/5/1 with 45 DIP cells, which reads as a row that
        // lost its icons, while six columns holds the same sixteen in THREE rows at full size. So the
        // search takes the first column count that keeps a full-size cell, and only then the first that
        // merely clears kTrayPageMinCellDip -- and if neither does, the widest grid this page allows.
        int bestColumns = 0;
        float bestCell = 0.0f;
        for (int candidate = kTrayPageColumns; candidate <= kTrayPageMaxColumns; ++candidate) {
            const int rows = (iconCount + candidate - 1) / candidate;
            const float byWidth = (availableWidth - (candidate - 1) * kTrayPageCellGapDip) / candidate;
            const float byHeight = (availableHeight - (rows - 1) * kTrayPageCellGapDip) / rows;
            float candidateCell = kTrayPageCellDip;
            if (byWidth < candidateCell) candidateCell = byWidth;
            if (byHeight < candidateCell) candidateCell = byHeight;

            const bool fullSize = candidateCell >= kTrayPageCellDip - 0.01f;
            const bool pressable = candidateCell >= kTrayPageMinCellDip - 0.01f;
            if (fullSize) {
                bestColumns = candidate;
                bestCell = kTrayPageCellDip;
                break;
            }
            if (!bestColumns && pressable) {
                bestColumns = candidate;
                bestCell = candidateCell;
            }
            if (candidate == kTrayPageMaxColumns && !bestColumns) {
                bestColumns = candidate;
                bestCell = candidateCell;
            }
        }
        if (bestColumns > 0) {
            columns = bestColumns < iconCount ? bestColumns : iconCount;
            cell = bestCell;
        }
    }
    if (cell < 1.0f) cell = 1.0f;

    layout.columns = columns;

    const int rows = (iconCount + columns - 1) / columns;
    // ... AND THEN EACH CELL GROWS INTO THE ROOM, capped, the way the panel's tiles share the space that is
    // left. Without this a three-row page of sixteen icons is 152 DIP of tiles inside a 280 DIP card: the
    // grid is centred and correct and the CARD looks half empty, which is what "看着不协调" was about. The
    // cap is what keeps a page of five icons from turning into five postage stamps.
    {
        const float byWidth = (availableWidth - (columns - 1) * kTrayPageCellGapDip) / columns;
        const float byHeight = (availableHeight - (rows - 1) * kTrayPageCellGapDip) / rows;
        float grown = byWidth < byHeight ? byWidth : byHeight;
        if (grown > kTrayPageMaxCellDip) grown = kTrayPageMaxCellDip;
        if (grown > cell) cell = grown;
    }
    const float gridWidth = columns * cell + (columns - 1) * kTrayPageCellGapDip;
    const float gridHeight = rows * cell + (rows - 1) * kTrayPageCellGapDip;
    // Centred BOTH ways: a page of four icons is not pinned to the left while a page of five fills the card,
    // and a page that only needs one row sits in the middle of the room rather than under the heading.
    const float left = card.rect.left + (cardWidth - gridWidth) * 0.5f;
    const float top = card.rect.top + titleHeight + 8.0f + (availableHeight - gridHeight) * 0.5f;

    for (int i = 0; i < iconCount; ++i) {
        const int column = i % columns;
        const int row = i / columns;
        // THE INCOMPLETE LAST ROW IS CENTRED under the full rows. Left-aligning it made a 5+2 page read as
        // a row that had lost three icons -- the empty columns all on the right, where the eye expects the
        // grid to continue -- and on this machine the tray page is 5+2 more often than not.
        const int firstInRow = row * columns;
        const int remaining = iconCount - firstInRow;
        const int rowCount = remaining < columns ? remaining : columns;
        const float rowWidth = rowCount * cell + (rowCount - 1) * kTrayPageCellGapDip;
        const float rowLeft = left + (gridWidth - rowWidth) * 0.5f;
        RectF placed;
        placed.left = rowLeft + column * (cell + kTrayPageCellGapDip);
        placed.top = top + row * (cell + kTrayPageCellGapDip);
        placed.right = placed.left + cell;
        placed.bottom = placed.top + cell;
        layout.cells[i] = placed;
    }
    return layout;
}

// Which cell a card-local DIP point is on, or -1.
inline int TrayPageCellAt(const RoundedBox& card, int iconCount, float x, float y) {
    const TrayPageLayout layout = TrayPageFor(card, iconCount);
    for (int i = 0; i < layout.count; ++i) {
        const RectF& cell = layout.cells[i];
        if (x >= cell.left && x <= cell.right && y >= cell.top && y <= cell.bottom) return i;
    }
    return -1;
}

// --- the tray's own context menu (2026-10-14, HANDOFF 8.80) ---------------------------
//
// WHY THIS EXISTS. A right click on a tray cell used to ask the SHELL for the application's menu: show the
// taskbar invisibly, open the overflow panel, move the pointer onto the icon, click, put the pointer back.
// That works (and is the only way to the items only the application knows -- "退出微信"), but it costs
// three to six hundred milliseconds and the menu appears at the icon's own screen position, which is the
// top-right corner rather than under the pointer. This menu is the bar's own: instant, under the pointer,
// drawn in the surface the bar already owns. The application's menu is still reachable, from the last
// entry, which is what turns the two into one usable thing instead of two half-answers.
inline constexpr float kTrayMenuWidthDip   = 176.0f;
inline constexpr float kTrayMenuRowDip     = 30.0f;
inline constexpr float kTrayMenuPadDip     = 6.0f;
inline constexpr int   kMaxTrayMenuEntries = 6;

enum class TrayMenuAction : int {
    Open,             // the icon's own default action -- exactly what a left click on it does
    ActivateWindow,   // bring a window of that application to the front
    OpenLocation,     // the folder the executable lives in, with the file selected
    CloseWindow,      // ask the application to close its window (WM_CLOSE: graceful, it may prompt)
    QuitApplication,  // END the application's processes -- the forceful one, behind a second press
    ApplicationMenu,  // the application's own menu, through the shell (slow, and at the icon's corner)
};

// The label a user reads. Wide, because two of the four are Chinese and the renderer wants wide strings.
//
// `ownMenuUnreachable` is the one fact that changes a WORD rather than adding a row (2026-10-16, round 44):
// an icon whose application has answered neither of the two shapes a right press may send (see
// TrayPress.cpp's right-note file) can still be offered the row -- the press will simply open this bar's own
// menu -- but the row has to say so. A row that promises the application's own menu and delivers this bar's
// is the kind of quiet lie this project keeps paying for.
inline const wchar_t* TrayMenuLabel(TrayMenuAction action, bool ownMenuUnreachable = false) {
    if (action == TrayMenuAction::ApplicationMenu && ownMenuUnreachable) return L"应用菜单（本机拿不到）";
    switch (action) {
        case TrayMenuAction::Open:            return L"打开";
        case TrayMenuAction::ActivateWindow:  return L"显示窗口";
        case TrayMenuAction::OpenLocation:    return L"文件位置";
        case TrayMenuAction::CloseWindow:     return L"关闭窗口";
        case TrayMenuAction::QuitApplication: return L"关闭应用";
        case TrayMenuAction::ApplicationMenu: return L"应用菜单";
    }
    return L"";
}

// The same label for an entry that is ARMED -- waiting for the press that carries it out.
//
// Only one entry has a second state, and it is the one that cannot be taken back: 结束应用 ends the
// application's processes, so it gives the application no chance to save. The first press says so in
// words ("再点一次"), which is the same arrangement the Start card's power list uses for sign-out,
// restart and shutdown.
inline const wchar_t* TrayMenuArmedLabel(TrayMenuAction action) {
    if (action == TrayMenuAction::QuitApplication) return L"再点一次：关闭应用";
    return TrayMenuLabel(action);
}

// What the icon can actually do, so that the menu never offers a row that does nothing.
struct TrayMenuFacts {
    bool hasWindow = false;    // the application has a window to bring forward
    bool hasExePath = false;   // the registry recorded where its executable lives
    // ... AND WHETHER ITS TRAY CALLBACK IS KNOWN. The two rows that are "the icon's own action" cannot do
    // anything without it: 打开 sends the application its own left-click callback (TrayPress) and 应用自己的
    // 菜单… sends it the right-click one, and an application whose callback has never been learned is simply
    // not sendable to. Measured by the user on 2026-10-15: pressing 应用自己的菜单… on one of those cells
    // "did nothing", because all it could do was re-open the same menu -- a row that does nothing is worse
    // than a row that is absent, and this struct is what the rule is stated in.
    bool hasCallback = false;
    // ... AND WHETHER ITS OWN MENU IS REACHABLE AT ALL: false once both shapes a right press can send have been
    // tried and the application answered neither (the press writes that down per icon). The row stays -- the
    // press falls back to this bar's menu -- but its label says what it can actually deliver.
    bool ownMenuUnreachable = false;
};

// Which entries the menu shows, in order, for one icon -- PURE, so the rule is pinned by the self-test
// rather than by looking at the screen. The application's menu is always last: it is the slow one, and it
// is the one that leaves the bar.
inline int TrayMenuEntriesFor(const TrayMenuFacts& facts, TrayMenuAction* out, int capacity) {
    if (!out || capacity <= 0) return 0;
    int count = 0;
    const auto add = [&](TrayMenuAction action) {
        if (count < capacity) out[count++] = action;
    };
    // THE FOUR ROWS THE USER ASKED FOR (2026-10-16): 打开 / 文件位置 / 应用菜单 / 关闭应用 -- in his
    // order. 打开 is the icon's own default action (the application's own left click, sent as its own
    // callback), and 应用菜单 is the one row that goes and gets the application's OWN menu, which is the
    // slow route and the only one that can offer what only the application knows ("退出微信").
    if (facts.hasCallback) add(TrayMenuAction::Open);
    if (facts.hasExePath) add(TrayMenuAction::OpenLocation);
    if (facts.hasCallback) add(TrayMenuAction::ApplicationMenu);
    add(TrayMenuAction::QuitApplication);
    return count;
}

struct TrayMenuLayout {
    RectF box;
    RectF rows[kMaxTrayMenuEntries];
    int   count = 0;
};

// Where the menu goes for a pointer at a card-local DIP point: just below and to the right of it, flipped
// up or to the left when that would leave the card, and never outside the card at all -- the card is what
// the window's input region covers, so a menu beyond it would be drawn where clicks cannot land.
//
// Called ONCE, when the menu opens, and the box is kept: a menu whose position is recomputed from the
// pointer every frame walks away from the pointer that is on it.
inline TrayMenuLayout TrayMenuFor(const RoundedBox& card, float x, float y, int entryCount) {
    TrayMenuLayout layout;
    if (entryCount < 0) entryCount = 0;
    if (entryCount > kMaxTrayMenuEntries) entryCount = kMaxTrayMenuEntries;
    layout.count = entryCount;
    if (entryCount == 0) return layout;

    const float height = entryCount * kTrayMenuRowDip + kTrayMenuPadDip * 2.0f;
    float left = x + 6.0f;
    float top = y + 6.0f;
    if (left + kTrayMenuWidthDip > card.rect.right - 4.0f) left = x - 6.0f - kTrayMenuWidthDip;
    if (top + height > card.rect.bottom - 4.0f) top = y - 6.0f - height;
    // The last word: inside the card, whatever the flips decided.
    if (left < card.rect.left + 4.0f) left = card.rect.left + 4.0f;
    if (left + kTrayMenuWidthDip > card.rect.right - 4.0f) {
        left = card.rect.right - 4.0f - kTrayMenuWidthDip;
    }
    if (top < card.rect.top + 4.0f) top = card.rect.top + 4.0f;
    if (top + height > card.rect.bottom - 4.0f) top = card.rect.bottom - 4.0f - height;

    layout.box.left = left;
    layout.box.right = left + kTrayMenuWidthDip;
    layout.box.top = top;
    layout.box.bottom = top + height;
    for (int i = 0; i < entryCount; ++i) {
        layout.rows[i].left = layout.box.left + kTrayMenuPadDip;
        layout.rows[i].right = layout.box.right - kTrayMenuPadDip;
        layout.rows[i].top = layout.box.top + kTrayMenuPadDip + i * kTrayMenuRowDip;
        layout.rows[i].bottom = layout.rows[i].top + kTrayMenuRowDip;
    }
    return layout;
}

// Which row a card-local DIP point is on, or -1. Against the box the menu was OPENED with, not a fresh
// one: see TrayMenuFor.
inline int TrayMenuRowAt(const TrayMenuLayout& layout, float x, float y) {
    for (int i = 0; i < layout.count; ++i) {
        const RectF& row = layout.rows[i];
        if (x >= row.left && x <= row.right && y >= row.top && y <= row.bottom) return i;
    }
    return -1;
}

inline CardContentLayout CardContentFor(const RoundedBox& cardBox) {
    const float height = cardBox.rect.bottom - cardBox.rect.top;
    const float top = cardBox.rect.top + height * kCardContentTopFraction;
    const float bottom = cardBox.rect.top + height * kCardContentBottomFraction;

    CardContentLayout layout;

    // The progress bar is carved off the bottom first, so the artwork and the text
    // are sized against what is left rather than overlapping it.
    layout.progress.left = cardBox.rect.left + kArtworkInsetDip;
    layout.progress.right = cardBox.rect.right - kArtworkInsetDip;
    layout.progress.bottom = bottom;
    layout.progress.top = bottom - kProgressBarHeightDip;

    layout.content.left = cardBox.rect.left + kArtworkInsetDip;
    layout.content.right = cardBox.rect.right - kArtworkInsetDip;
    layout.content.top = top;
    layout.content.bottom = layout.progress.top - 4.0f;

    // Artwork on the left, square, aligned to the top of the band.
    layout.artwork.left = layout.content.left;
    layout.artwork.top = layout.content.top;
    layout.artwork.right = layout.artwork.left + kArtworkSizeDip;
    layout.artwork.bottom = layout.artwork.top + kArtworkSizeDip;

    // Three buttons on the right, in reading order: previous, play/pause, next.
    const float rowWidth = kTransportButtonDip * 3.0f + kTransportGapDip * 2.0f;
    const float rowLeft = layout.content.right - rowWidth;
    const float rowTop = layout.content.top +
                         (kArtworkSizeDip - kTransportButtonDip) * 0.5f;
    const float rowBottom = rowTop + kTransportButtonDip;

    layout.previous.left = rowLeft;
    layout.previous.right = rowLeft + kTransportButtonDip;
    layout.playPause.left = layout.previous.right + kTransportGapDip;
    layout.playPause.right = layout.playPause.left + kTransportButtonDip;
    layout.next.left = layout.playPause.right + kTransportGapDip;
    layout.next.right = layout.next.left + kTransportButtonDip;

    layout.previous.top = rowTop;
    layout.previous.bottom = rowBottom;
    layout.playPause.top = rowTop;
    layout.playPause.bottom = rowBottom;
    layout.next.top = rowTop;
    layout.next.bottom = rowBottom;

    // Text fills what is between the artwork and the buttons.
    layout.text.left = layout.artwork.right + 10.0f;
    layout.text.right = layout.previous.left - 10.0f;
    layout.text.top = layout.content.top;
    layout.text.bottom = layout.content.bottom;

    return layout;
}

// Which control, if any, a window-local DIP point lands on.
inline CardControl ControlAt(const RoundedBox& cardBox, float x, float y) {
    const CardContentLayout layout = CardContentFor(cardBox);
    const auto inside = [x, y](const RectF& rect) {
        return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
    };
    if (inside(layout.previous)) return CardControl::Previous;
    if (inside(layout.playPause)) return CardControl::PlayPause;
    if (inside(layout.next)) return CardControl::Next;
    return CardControl::None;
}

// True when a window-local DIP point is inside any transport button.
inline bool HitsTransportButton(const RoundedBox& cardBox, float x, float y) {
    return ControlAt(cardBox, x, y) != CardControl::None;
}

// --- the lyric strip -------------------------------------------------

// Above this many DIPs of text the strip scrolls instead of centring.
//
// ONE predicate, used by both the renderer to decide how to place the line and by
// the frame loop to decide whether to keep advancing the scroll. They used to work
// it out separately, from slightly different numbers, which meant the renderer could
// centre a line while the loop scrolled it -- the text would jump sideways for no
// visible reason. A line that fits is centred and still; a longer one scrolls.
inline bool LyricNeedsScroll(float textWidthDip, float stripWidthDip) {
    return textWidthDip > stripWidthDip - 24.0f;
}

// How far inside each end of the strip a SCROLLING line fades out.
//
// A line too long for the strip scrolls, and the strip clips it to its own STADIUM shape -- so the
// character leaving on the left is cut by a curve, mid-glyph, and the line reads as chopped rather than as
// moving. Reported with a screenshot of exactly that ("歌词 长的话能不能会这样", the leading "na" sliced in
// half at the island's left edge). The words still have to leave the strip; what changes is that they fade
// over the last few DIP first, so the two ends read as a slide instead of a cut. Asked for in the iOS
// vocabulary the rest of this bar is built in.
//
// Capped at a quarter of the strip per end, so a narrow strip still has a readable middle: two fades that
// met would dim the whole line rather than its edges.
inline float LyricEndFadeDip(float stripWidthDip) {
    if (stripWidthDip <= 0.0f) return 0.0f;
    constexpr float kWantedDip = 20.0f;
    const float limit = stripWidthDip * 0.25f;
    return kWantedDip < limit ? kWantedDip : limit;
}

// --- the music card --------------------------------------------------
//
// Laid out to match the Windows 11 media flyout, which is what the user asked for
// and what the rest of the system already looks like:
//
//   +--------------------------------------------------+
//   | [icon] QQ Music                          70% [v] |   app row
//   |  +------+  涓滃ぇ琛?                                 |
//   |  | art  |  寮犺秴娲?路 涓滃ぇ琛?                         |   art + title
//   |  +------+                                         |
//   |  01:33 [=========================] 05:40          |   times + progress
//   |            [|<]     ( > )     >|                  |   controls
//   +--------------------------------------------------+
//
// These are the dimensions the reference image measures out to, scaled to the card.

// The card's own padding, used by both the layout and the background artwork so the
// art lines up with the scrim it sits under.
inline constexpr float kMusicCardPadDip = 16.0f;

inline constexpr float kMusicAppRowHeightDip  = 20.0f;
// Sized from the reference image's PROPORTIONS, not by eye.
//
// The reference card is 384 DIP wide and its cover is 111 DIP -- 29% of the width. The cover here
// was 80 DIP against a 520 DIP card, which is 15%: half the reference's share, and the single
// biggest reason the card was reported as looking unfinished. The cover is the card's largest
// element and it was being drawn as a thumbnail; this round takes it to 108.
inline constexpr float kMusicArtworkSizeDip   = 108.0f;
inline constexpr float kMusicGapAfterArtDip   = 20.0f;
inline constexpr float kMusicTimeRowDip       = 16.0f;
inline constexpr float kMusicProgressBarDip   = 4.0f;
// The source icon, in the same proportion: the reference draws it at about 12% of the card width.
inline constexpr float kMusicAppIconSizeDip   = 22.0f;
// The controls' row and the click target are the SAME constant.
//
// They used to be separate (a 40 DIP row holding a 38 DIP target), which the layout
// test caught: the buttons were taller than the size they advertise, so a click just
// outside the drawn disc was still accepted. The PLAY disc is bigger than the two skips: it is the
// card's anchor and the one control a hand goes to without looking.
inline constexpr float kMusicControlSizeDip   = 38.0f;
inline constexpr float kMusicPlaySizeDip      = 46.0f;
inline constexpr float kMusicControlGapDip    = 26.0f;

// Where everything on the music card goes.
//
// The vertical order is the reference image's, and it is load-bearing: app row, then the
// cover and text, then the time row with the progress bar, then the controls. An earlier
// version put the time row alongside the cover, and the elapsed time ended up drawn
// underneath the album art.
//
// THE CARD HAS NO VOLUME CONTROL. It used to: a speaker at the row's right-hand end that opened a
// percentage and a 64 DIP bar, and three rectangles (`volumeIcon`, `volumeReadout`, `volumeBar`) plus
// `MusicVolumeAt` / `VolumeFractionAt` / `VolumeFractionForDrag` to hit-test and drag it. The user asked for
// the speaker to go ("把音乐卡片里右上角的图标去掉吧"), and the whole control went with it, because the speaker
// was its only handle -- a bar that nothing can open is not a control, it is a dead hit region. Nothing was
// lost: the level it moved was the DEVICE's master volume, which the system panel's own slider still owns,
// and the 声音输出 page's mixer still moves a single application's volume (ISimpleAudioVolume) -- which is the
// one place a player's own level can actually be reached from.
struct MusicCardLayout {
    RectF artBackground;   // the whole card, for the blurred artwork
    RectF appRow;          // the source strip along the top: the application's icon and name
    RectF appIcon;         // the source application's own icon
    RectF appText;         // "QQ闊充箰"
    float appIconSize = 0.0f;
    RectF artwork;         // the album cover
    RectF title;           // track title
    RectF subtitle;        // artist 路 album
    RectF elapsed;         // "01:33", left of the bar
    RectF remaining;       // "05:40", right of the bar
    RectF progress;        // the bar itself
    RectF controls;        // the row of three buttons
    RectF previous;
    RectF playPause;
    RectF next;
};

// Where the card goes horizontally.
//
// The card is NOT centred on the screen. It opens from the control that was pressed, the way a
// flyout does -- pressing a button on the far left of the bar and getting a panel in the middle of
// the screen reads as unrelated to the thing that was clicked.
//
// `anchorLeftDip` is the left edge of the control the user pressed, in window coordinates. The card
// is placed so that edge lines up with the card's own left edge, then pushed back inside the window
// if that would run it off the right side. A control far enough right that neither alignment fits
// gets the card clamped to the right margin, which is the best available answer and still keeps the
// whole card on screen.
inline float CardLeftFor(float anchorLeftDip, float cardWidthDip, float windowWidthDip) {
    constexpr float kMargin = 8.0f;

    float left = anchorLeftDip;
    if (left + cardWidthDip > windowWidthDip - kMargin) {
        left = windowWidthDip - kMargin - cardWidthDip;
    }
    if (left < kMargin) left = kMargin;
    return left;
}

// WHICH LEVEL OF THE PANEL'S CARD IS SHOWING, held for the card's whole life.
//
// One card, three levels, and only one of them at a time: the tiles, the 声音输出 page the output-device
// row opens, and the tray page the button beside the island opens. Every one of them is drawn in the same
// card, at the same place, with the same animation -- and the card is on screen for the whole of its own
// EXIT, which is where a per-frame answer goes wrong:
//
//   * CLOSING while the tray page is up switched the content on the first frame of the retraction: the
//     panel's tiles appeared where the grid had been and then shrank away. Reported as "点击展开 再点击收
//     回去 会显示出系统卡片的东西". The same thing happened closing from 声音输出.
//
// and in two more places that want the same answer: where the card STANDS (it hangs under the button that
// opened it -- the tray button is at the island's right, the system button at its left, 296 DIP apart, so
// the tray page's card was under the wrong end of the bar: "点击托盘按钮 弹出的卡片不在这个按钮下面") and
// whether the tray grid is what the pointer is over.
//
// So the LEVEL is what is held, and everything else follows from it. The requests stay authoritative while
// the card is still wanted -- the 声音输出 page's Back control is a request to go back to the tiles with
// the card still up, and it has to be obeyed -- and only the retraction holds:
//
//   * card gone                       -> nothing is showing; the next open decides afresh
//   * the panel has been put away     -> keep the level the card was opened with, for the whole exit
//   * a page is asked for, card up    -> that page (and it replaces the other one, because one card cannot
//                                        show two levels at once)
//   * neither page asked, card up     -> the tiles (that is the Back control, and the panel's own button)
//
// `held` is the previous answer, so the caller feeds this back to itself exactly as it does with
// NextCardOwner. Every row of that table is asserted in Diagnostics.
// `Start` is the fourth level: the user's own Start card (see StartPageFor).
enum class PanelLevel { Tiles, Sound, Tray, Start };

inline PanelLevel NextPanelLevel(PanelLevel held, bool cardUp, bool panelWanted, bool trayWanted,
                                 bool soundWanted, bool startWanted) {
    if (!cardUp) return PanelLevel::Tiles;
    if (!panelWanted) return held;   // put away: the card keeps its level while it leaves the screen
    if (trayWanted) return PanelLevel::Tray;
    if (soundWanted) return PanelLevel::Sound;
    if (startWanted) return PanelLevel::Start;
    return PanelLevel::Tiles;
}

// WHETHER A CONTROL MAY ANSWER THE POINTER.
//
// All four levels are one card in one rectangle, so each of them has controls where another has EMPTY
// SPACE -- and empty space that answers is how dragging across the tray page's lower band moved the
// volume, because the panel's slider lies exactly there. Only the level that is SHOWING may answer, for
// every kind of control: the tiles and their slider, the 声音输出 rows, the tray page's cells, the Start
// card's tiles.
//
// A function rather than a comparison at each site, because the four questions are asked in five places
// (the press, the click chain, the right click, the wheel) and the fifth one is exactly what gets missed.
inline bool LevelAnswersInput(PanelLevel showing, PanelLevel controls) { return showing == controls; }

// WHICH CONTROL SHOWS THAT IT IS OPEN.
//
// One card, four levels, and three controls that can open one of them -- so "which button looks pressed"
// is a question about the LEVEL, not about whether a card is up at all. Asked as three questions here, and
// the self-test asserts that exactly one of them is true for every level: the user's report was the
// symptom of them not being asked separately ("点击 ⊞ 系统按钮也跟着变") -- the system button's chevron
// pointed up whenever ANY card was up, because the frame told it `systemPanelOpen`, which the bar sets for
// every level so that the card exists at all.
//
// The sound page counts as the panel's own because it is reached from inside the panel, and the system
// button is what puts it away.
inline bool SystemButtonShowsOpen(PanelLevel level) {
    return level == PanelLevel::Tiles || level == PanelLevel::Sound;
}

inline bool TrayButtonShowsOpen(PanelLevel level) { return level == PanelLevel::Tray; }

inline bool StartButtonShowsOpen(PanelLevel level) { return level == PanelLevel::Start; }

// ... AND NONE OF THEM WHEN NO CARD IS UP.
//
// The level alone cannot answer that: `Tiles` is both "the panel is showing its tiles" and "no page was
// asked for", so the first version of this lit the system button's chevron on a bar that had just started
// with no card on screen at all -- photographed, one command after the fix that made it light for the same
// reason in reverse. `cardUp` is the frame's own `outerCardUp`, which is true for the whole of the exit as
// well, so a control stays lit while the card it opened is still leaving the screen.
struct ControlLit {
    bool system = false;
    bool tray = false;
    bool start = false;
};

inline ControlLit ControlLitFor(PanelLevel level, bool cardUp) {
    ControlLit lit;
    if (!cardUp) return lit;
    lit.system = SystemButtonShowsOpen(level);
    lit.tray = TrayButtonShowsOpen(level);
    lit.start = StartButtonShowsOpen(level);
    return lit;
}

// --- the Start card -------------------------------------------------------------------------------
//
// The user's own words for what this is: "创建一个空白的卡片 然后里面只有红框里的 关机键 和用户头像,
// 上面的应该可以让用户手动添加" -- a BLANK card whose footer holds the account picture and the power
// button, and whose upper area the user fills himself.
//
// So the card's upper area is a GRID OF PINS that starts empty, and the footer is the two things the
// shell's Start menu puts in its own footer. The card itself is the SAME card as the panel's -- 520 x 250,
// hanging from the same button, with the same open/close animation -- because a Start card is one more
// level of a thing that already exists, not a new kind of window.
inline constexpr int   kStartColumns    = 6;
inline constexpr int   kStartRows       = 3;
inline constexpr float kStartCellWidth  = 76.0f;
// 50, not 60: three rows plus two 4 DIP gaps have to fit the 162 DIP between the card's top padding and
// the footer (250 - 16 top - 16 bottom - 46 footer - 10 gap). Asserted, and it failed at 60 -- which is
// what the assertion is for.
inline constexpr float kStartCellHeight = 50.0f;
inline constexpr float kStartCellGap    = 4.0f;
inline constexpr float kStartFooterDip  = 46.0f;
inline constexpr float kStartAvatarDip  = 28.0f;
inline constexpr float kStartPowerDip   = 30.0f;   // the power button's square
inline constexpr float kStartRowHeight  = 34.0f;   // one line of the picker or the power list
inline constexpr float kStartHeaderDip  = 30.0f;   // the row the 全部 button sits in, above the grid
inline constexpr float kStartAllAppsWidth = 30.0f;   // an icon button, not a word
inline constexpr float kStartAllAppsHeight = 22.0f;

struct StartPageLayout {
    RectF grid;         // where the pinned tiles live
    RectF footer;       // the account picture and the power button
    RectF avatar;
    RectF powerButton;
    RectF allApps;      // the 全部 button, in the header row at the card's top-right
    bool  valid = false;
};

// `card` is the card's own rectangle, the same one every other level is laid out in.
inline StartPageLayout StartPageFor(const RoundedBox& card) {
    StartPageLayout layout;
    constexpr float pad = 16.0f;
    if (card.rect.right - card.rect.left < pad * 2.0f + 1.0f ||
        card.rect.bottom - card.rect.top < kStartFooterDip + pad * 2.0f + 1.0f) {
        return layout;
    }

    layout.footer.left = card.rect.left + pad;
    layout.footer.right = card.rect.right - pad;
    layout.footer.bottom = card.rect.bottom - pad;
    layout.footer.top = layout.footer.bottom - kStartFooterDip;

    // The grid starts below the card's top edge and stops a gap above the footer. Its columns are spread
    // across the card's full inner width, centred: the cell size is fixed and the leftover is split.
    const float innerWidth = card.rect.right - card.rect.left - pad * 2.0f;
    const float gridWidth =
        kStartColumns * kStartCellWidth + (kStartColumns - 1) * kStartCellGap;
    layout.grid.left = card.rect.left + pad + (innerWidth - gridWidth) * 0.5f;
    layout.grid.right = layout.grid.left + gridWidth;
    // The header row belongs to the 全部 button: the grid starts under it, so the button can never sit on
    // top of a tile (it would have covered the first row's last application).
    layout.grid.top = card.rect.top + pad + kStartHeaderDip;
    layout.grid.bottom = layout.footer.top - 10.0f;

    layout.allApps.right = card.rect.right - pad;
    layout.allApps.left = layout.allApps.right - kStartAllAppsWidth;
    layout.allApps.top = card.rect.top + pad - 2.0f;
    layout.allApps.bottom = layout.allApps.top + kStartAllAppsHeight;

    layout.avatar.right = layout.footer.left + kStartAvatarDip;
    layout.avatar.left = layout.footer.left;
    const float footerCentreY = (layout.footer.top + layout.footer.bottom) * 0.5f;
    layout.avatar.top = footerCentreY - kStartAvatarDip * 0.5f;
    layout.avatar.bottom = layout.avatar.top + kStartAvatarDip;

    layout.powerButton.right = layout.footer.right;
    layout.powerButton.left = layout.powerButton.right - kStartPowerDip;
    layout.powerButton.top = footerCentreY - kStartPowerDip * 0.5f;
    layout.powerButton.bottom = layout.powerButton.top + kStartPowerDip;

    layout.valid = true;
    return layout;
}

// One pinned application's cell, by index. Rows are read left to right, top to bottom, which is also the
// order they were pinned in.
inline RectF StartPinCellFor(const StartPageLayout& layout, int index) {
    RectF cell;
    if (!layout.valid || index < 0) return cell;
    const int column = index % kStartColumns;
    const int row = index / kStartColumns;
    cell.left = layout.grid.left + static_cast<float>(column) * (kStartCellWidth + kStartCellGap);
    cell.top = layout.grid.top + static_cast<float>(row) * (kStartCellHeight + kStartCellGap);
    cell.right = cell.left + kStartCellWidth;
    cell.bottom = cell.top + kStartCellHeight;
    return cell;
}

// How many pins the grid can hold, which is what says whether a pin has to wait for a second page.
inline int StartPinCapacity() { return kStartColumns * kStartRows; }

// One row of the picker, counting from the top of the grid area: `rowOnScreen` is after the scroll.
inline RectF StartPickerRowFor(const RoundedBox& card, int rowOnScreen) {
    const StartPageLayout layout = StartPageFor(card);
    RectF row;
    if (!layout.valid || rowOnScreen < 0) return row;
    row.left = layout.grid.left;
    row.right = layout.grid.right;
    row.top = layout.grid.top + static_cast<float>(rowOnScreen) * kStartRowHeight;
    row.bottom = row.top + kStartRowHeight;
    if (row.bottom > layout.grid.bottom + kStartRowHeight * 0.5f) {
        // Past the bottom of the grid area: not on screen, and the caller skips it.
        return row;
    }
    return row;
}

// One row of the power list, which stands ABOVE the power button and is right-aligned to it -- the
// placement the shell's own Start uses, so the menu reads as belonging to the button it came from.
inline constexpr float kStartPowerRowWidth = 168.0f;
inline RectF StartPowerRowFor(const RoundedBox& card, int index) {
    const StartPageLayout layout = StartPageFor(card);
    RectF row;
    if (!layout.valid || index < 0) return row;
    row.right = layout.powerButton.right;
    row.left = row.right - kStartPowerRowWidth;
    row.bottom = layout.footer.top - 8.0f - static_cast<float>(index) * kStartRowHeight;
    row.top = row.bottom - kStartRowHeight;
    return row;
}

// The rectangles for the tiles that get a wash. Kept here with the geometry so the hit tests and the
// drawing cannot disagree about where a cell is.
inline bool StartRectHasPoint(const RectF& rect, float x, float y) {
    return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
}

// Whether the Start card's pointer or its scroll has MOVED, which is a question about a change rather
// than about a state -- and the one thing in this file that is not geometry.
//
// It lives here, and as a function rather than as an expression inside the frame loop, because of what
// its absence cost: the frame loop's redraw condition listed the clock, the animations, the music, the
// lyrics and the notification, and NOT these three numbers. The window was answering "which row is under
// the pointer" every 8 ms and nobody was asking, so the highlight and the list moved only on the next
// clock tick -- once a second, reported by the user as "鼠标移上去卡卡的 添加应用滚动也卡卡的", and visible
// in the bar's own report as `redraws: clock=4` with every other term at zero while the wheel was turning.
//
// The three inputs are the ONLY things that change when the pointer moves over the card or its list
// scrolls: what is under the pointer (`target`, `index`) and how far the list has moved (`scroll`).
inline bool StartCardAsksForFrame(int target, int index, int scroll, int lastTarget, int lastIndex,
                                  int lastScroll) {
    return target != lastTarget || index != lastIndex || scroll != lastScroll;
}

inline int StartPinCellAt(const StartPageLayout& layout, int count, float x, float y) {
    if (!layout.valid) return -1;
    if (!StartRectHasPoint(layout.grid, x, y)) return -1;
    // By ARITHMETIC rather than by testing each cell: the cells are separated by a 4 DIP gap, and a click
    // two pixels off a tile landing in that gap and doing nothing is a dead zone in a grid the user is
    // meant to fill by hand. Dividing the offset by the pitch makes the whole area belong to some tile.
    const float pitchX = kStartCellWidth + kStartCellGap;
    const float pitchY = kStartCellHeight + kStartCellGap;
    const int column = static_cast<int>((x - layout.grid.left) / pitchX);
    const int row = static_cast<int>((y - layout.grid.top) / pitchY);
    if (column < 0 || column >= kStartColumns || row < 0 || row >= kStartRows) return -1;
    const int index = row * kStartColumns + column;
    const int capacity = StartPinCapacity();
    const int limit = count < capacity ? count : capacity;
    return index < limit ? index : -1;
}

inline int StartPickerRowAt(const RoundedBox& card, int scrollRow, int count, float x, float y) {
    const StartPageLayout layout = StartPageFor(card);
    if (!layout.valid) return -1;
    const int rowsOnScreen =
        static_cast<int>((layout.grid.bottom - layout.grid.top) / kStartRowHeight);
    for (int visible = 0; visible <= rowsOnScreen; ++visible) {
        const int index = scrollRow + visible;
        if (index < 0 || index >= count) break;
        const RectF row = StartPickerRowFor(card, visible);
        if (row.bottom > layout.grid.bottom) break;
        if (StartRectHasPoint(row, x, y)) return index;
    }
    return -1;
}

// One cell of the ALL-APPLICATIONS grid: the same 6-column cells the pins use, scrolled by whole rows.
// Returns the LIST index (scrollRow * kStartColumns + the cell), not the cell index, because that is what
// the drawing and the click need; -1 off the grid or past the end of the list.
inline int StartPickerCellAt(const RoundedBox& card, int scrollRow, int count, float x, float y) {
    const StartPageLayout layout = StartPageFor(card);
    if (!layout.valid || !StartRectHasPoint(layout.grid, x, y)) return -1;
    const float pitchX = kStartCellWidth + kStartCellGap;
    const float pitchY = kStartCellHeight + kStartCellGap;
    const int column = static_cast<int>((x - layout.grid.left) / pitchX);
    const int row = static_cast<int>((y - layout.grid.top) / pitchY);
    if (column < 0 || column >= kStartColumns || row < 0 || row >= kStartRows) return -1;
    const int index = (scrollRow + row) * kStartColumns + column;
    return index < count ? index : -1;
}

inline int StartPowerRowAt(const RoundedBox& card, float x, float y) {
    for (int i = 0; i < 5; ++i) {
        if (StartRectHasPoint(StartPowerRowFor(card, i), x, y)) return i;
    }
    return -1;
}

// --- the system panel --------------------------------------------------------------
//
// What the row of system indicators summarises, opened out. Its structure follows the shell's own
// quick-settings flyout, because that is the shape a user already knows for this content:
//
//     [ tile ] [ tile ] [ tile ]     2 rows of 3
//     [ tile ] [ tile ] [ tile ]
//     [ speaker  -----------o----- ]  the volume slider
//     [ sun      ---------o------- ]  the brightness slider, when the display answers
//     [                        (gear) ] the settings button
//
// The tiles have an ON and an OFF appearance rather than being a list of readings. A row that says
// "Wi-Fi: not connected" tells the user something; a tile that is dark tells them the same thing AND
// what pressing it will do.
//
// Like the music card, this is one CARD -- same state, same shape, same radius -- because it is the
// same kind of thing: something that opens downward from the bar to show more than the bar has room
// for.
//
// THE HEIGHT, because the window is a fixed 240 DIP and nothing may be clipped:
//
//     padding top                      14
//     2 rows of 46 tiles + 8 gap       100
//     gap                              10
//     slider row                       22
//     gap                               8
//     settings row                     28
//     padding bottom                   14
//                                     ----
//                                     196
//
// and the card starts at the pill's margin of 6, so it ends at 202 with 38 DIP to spare. The music
// card uses the same 212 DIP height, since both are the same state and the outline must not jump
// between them.
inline constexpr float kSystemTileHeightDip = 46.0f;
inline constexpr float kSystemTileGapDip = 8.0f;

// The tallest a tile is allowed to become when the card has room to spare.
//
// The tiles SHARE the space that is left rather than taking their designed height and leaving the rest empty
// -- see the grid in SystemPanelFor -- and this keeps a very tall card from turning them into slabs.
inline constexpr float kSystemTileMaxHeightDip = 76.0f;

inline constexpr int kSystemTileColumns = 3;
inline constexpr int kSystemTileRows = 2;
inline constexpr int kMaxSystemTiles = kSystemTileColumns * kSystemTileRows;

// How many tiles the panel can HOLD, as against how many it shows at once: the shell's flyout pages through
// its tiles, and this is the ceiling on the whole set.
inline constexpr int kMaxSystemPanelTiles = 3 * kMaxSystemTiles;

struct SystemPanelLayout {
    // The tile's BOX -- the rounded square the glyph sits in. Its caption is `tileLabels`, which is BELOW this
    // rectangle, not inside it: that is how the shell's own flyout is built (the box carries the glyph and,
    // where the tile opens something, a chevron; the caption under it carries the state or the name).
    RectF tiles[kMaxSystemTiles];
    RectF tileIcons[kMaxSystemTiles];
    RectF tileLabels[kMaxSystemTiles];   // the CAPTION box, under the tile
    RectF sliders[2];         // the volume slider, and the brightness slider if there is one
    RectF speakerIcon;        // at the left of the first slider
    // At the left of the second slider. Its own rectangle rather than a shared one, because the two rows
    // carry different marks -- a speaker and a sun -- and one rectangle for both drew the speaker glyph
    // twice, which is how the first attempt at this looked.
    RectF brightnessIcon;
    // The output-device button at the RIGHT of the volume slider: the device glyph and the chevron
    // that opens the "声音输出" page, which is what the shell's own flyout puts there.
    RectF volumeButton;
    RectF gearButton;         // bottom right
    // The paging column, on the grid's right: the dots and the two arrows that page the tiles, as the shell's
    // own flyout has them. Empty when there is only one page.
    RectF pageDots[4];
    RectF pageUp;
    RectF pageDown;
    int pageDotCount = 0;
    int pageIndex = 0;
    int tileCount = 0;
    int sliderCount = 0;
};

// The panel's geometry for a card box and a number of tiles, laid out from the bottom up.
//
// Bottom-up for the same reason the music card is: the slider and the settings button are pinned to
// the card's lower edge, and the tile grid takes whatever room is left above them. Laying it out
// top-down would mean the slider moved whenever a tile was added, and the slider is the control a
// finger goes straight for.
inline SystemPanelLayout SystemPanelFor(const RoundedBox& box, int tileCount, int sliderCount,
                                        bool paging = false, int pageIndex = 0, int pageCount = 1) {
    SystemPanelLayout layout;

    if (tileCount < 0) tileCount = 0;
    if (tileCount > kMaxSystemTiles) tileCount = kMaxSystemTiles;
    if (sliderCount < 0) sliderCount = 0;
    if (sliderCount > 2) sliderCount = 2;
    layout.tileCount = tileCount;
    layout.sliderCount = sliderCount;

    const float pad = 14.0f;
    const float left = box.rect.left + pad;
    const float right = box.rect.right - pad;

    // The paging column is reserved OUT of the grid, the way the shell's flyout does it: the dots and the two
    // arrows sit in a narrow strip against the card's right edge, and the tiles stop short of it.
    const float pagingWidth = paging ? 20.0f : 0.0f;
    const float gridRight = right - pagingWidth;
    const float innerWidth = gridRight - left;

    // The output-device button, at the right of the volume row. Part of the row rather than of the
    // slider, so the slider can stop short of it. The shell's own flyout has exactly this: a slider,
    // then the device that is playing out of it, then an arrow into the page that lists the others.
    // 40 DIP: the mark is a 20 DIP square and the chevron needs its own 14 beside it. Measured off the
    // first version at 34 -- the two overlapped into one blob.
    const float deviceButtonWidth = 40.0f;

    // The settings button, on the card's bottom right.
    const float gearSize = 28.0f;
    layout.gearButton.right = right;
    layout.gearButton.left = right - gearSize;
    layout.gearButton.bottom = box.rect.bottom - pad;
    layout.gearButton.top = layout.gearButton.bottom - gearSize;

    // The sliders, stacked upward from the gear row.
    const float sliderHeight = 22.0f;
    const float sliderGap = 8.0f;

    for (int i = sliderCount - 1; i >= 0; --i) {
        const float bottom = layout.gearButton.top - sliderGap -
                             static_cast<float>(sliderCount - 1 - i) * (sliderHeight + sliderGap);
        layout.sliders[i].left = left + 26.0f;   // clear of the speaker glyph
        // The VOLUME row stops short of the output-device button, which owns its right end. The brightness
        // row has no such button, so its track uses the whole width -- and it is the row that is drawn
        // second, which is why the two cannot share one right edge.
        layout.sliders[i].right = i == 0 ? right - deviceButtonWidth - 6.0f : right;
        layout.sliders[i].bottom = bottom;
        layout.sliders[i].top = bottom - sliderHeight;
    }

    // The speaker glyph, at the left of the first slider.
    if (sliderCount > 0) {
        const float centreY = (layout.sliders[0].top + layout.sliders[0].bottom) * 0.5f;
        layout.speakerIcon.left = left;
        layout.speakerIcon.right = left + 20.0f;
        layout.speakerIcon.top = centreY - 10.0f;
        layout.speakerIcon.bottom = centreY + 10.0f;

        layout.volumeButton.right = right;
        layout.volumeButton.left = right - deviceButtonWidth;
        layout.volumeButton.top = centreY - 12.0f;
        layout.volumeButton.bottom = centreY + 12.0f;
    }

    // The sun, at the left of the brightness row.
    if (sliderCount > 1) {
        const float centreY = (layout.sliders[1].top + layout.sliders[1].bottom) * 0.5f;
        layout.brightnessIcon.left = left;
        layout.brightnessIcon.right = left + 20.0f;
        layout.brightnessIcon.top = centreY - 10.0f;
        layout.brightnessIcon.bottom = centreY + 10.0f;
    }

    // The tile grid, filling the space that is left above the controls.
    const int rows = (tileCount + kSystemTileColumns - 1) / kSystemTileColumns;
    if (rows > 0) {
        const float gridBottom =
            (sliderCount > 0 ? layout.sliders[0].top : layout.gearButton.top) - 10.0f;
        const float gridTop = box.rect.top + pad;
        const float available = gridBottom - gridTop;

        // The paging column: the dots in the middle of the grid, the up arrow above them and the down arrow
        // below, so the group is centred on the grid it pages.
        if (paging) {
            const float centreY = (gridTop + gridBottom) * 0.5f;
            const float dotRadius = 2.5f;
            const float dotGap = 9.0f;
            const int dots = pageCount < 4 ? pageCount : 4;
            layout.pageDotCount = dots;
            layout.pageIndex = pageIndex;
            const float dotsTop = centreY - (static_cast<float>(dots - 1) * dotGap) * 0.5f;
            const float columnCentreX = gridRight + pagingWidth * 0.5f;
            for (int i = 0; i < dots; ++i) {
                const float cy = dotsTop + static_cast<float>(i) * dotGap;
                layout.pageDots[i].left = columnCentreX - dotRadius;
                layout.pageDots[i].right = columnCentreX + dotRadius;
                layout.pageDots[i].top = cy - dotRadius;
                layout.pageDots[i].bottom = cy + dotRadius;
            }
            const float arrow = 14.0f;
            layout.pageUp.left = columnCentreX - arrow * 0.5f;
            layout.pageUp.right = columnCentreX + arrow * 0.5f;
            layout.pageUp.bottom = dotsTop - 6.0f;
            layout.pageUp.top = layout.pageUp.bottom - arrow;
            layout.pageDown.left = layout.pageUp.left;
            layout.pageDown.right = layout.pageUp.right;
            layout.pageDown.top = dotsTop + static_cast<float>(dots - 1) * dotGap + 6.0f;
            layout.pageDown.bottom = layout.pageDown.top + arrow;
        }
        // The tiles SHARE the space that is left, instead of taking their designed height and leaving the
        // remainder empty.
        //
        // The designed height was being used as a ceiling. On the card's fixed 250 DIP the six tiles needed
        // 98 of the 154 DIP available, so the panel wore a 56 DIP band of nothing between the grid and the
        // slider -- which is the difference between "a grid of controls" and "a grid of controls with a hole
        // under it". Sharing the space is also what the shell's own quick settings does: the tiles are as
        // tall as the flyout allows.
        float tileHeight =
            (available - static_cast<float>(rows - 1) * kSystemTileGapDip) / static_cast<float>(rows);
        if (tileHeight < 20.0f) tileHeight = 20.0f;
        if (tileHeight > kSystemTileMaxHeightDip) tileHeight = kSystemTileMaxHeightDip;

        const float tileWidth = (innerWidth -
                                 static_cast<float>(kSystemTileColumns - 1) * kSystemTileGapDip) /
                                static_cast<float>(kSystemTileColumns);

        // A slot is a BOX with a CAPTION under it, which is how the shell's own quick settings is built: the
        // box carries the glyph (and a chevron where the tile opens something) and the caption under it carries
        // the state or the name.
        const float captionHeight = 18.0f;
        const float captionGap = 5.0f;
        const float boxHeight = tileHeight - captionHeight - captionGap;

        for (int i = 0; i < tileCount; ++i) {
            const int row = i / kSystemTileColumns;
            const int column = i % kSystemTileColumns;
            const float tileLeft =
                left + static_cast<float>(column) * (tileWidth + kSystemTileGapDip);
            const float slotTop =
                gridTop + static_cast<float>(row) * (tileHeight + kSystemTileGapDip);

            layout.tiles[i].left = tileLeft;
            layout.tiles[i].right = tileLeft + tileWidth;
            layout.tiles[i].top = slotTop;
            layout.tiles[i].bottom = slotTop + boxHeight;

            // The glyph at the box's LEFT, vertically centred -- not in a corner. The box is a control, and its
            // picture belongs where the eye starts reading it.
            const float glyphSize = 20.0f;
            const float boxCentreY = (layout.tiles[i].top + layout.tiles[i].bottom) * 0.5f;
            layout.tileIcons[i].left = tileLeft + 12.0f;
            layout.tileIcons[i].right = layout.tileIcons[i].left + glyphSize;
            layout.tileIcons[i].top = boxCentreY - glyphSize * 0.5f;
            layout.tileIcons[i].bottom = layout.tileIcons[i].top + glyphSize;

            // The caption, CENTRED under the box, filling the rest of the slot.
            layout.tileLabels[i].left = tileLeft;
            layout.tileLabels[i].right = tileLeft + tileWidth;
            layout.tileLabels[i].top = layout.tiles[i].bottom + captionGap;
            layout.tileLabels[i].bottom = slotTop + tileHeight;
        }
    }

    return layout;
}

// Which panel tile a point lands on, or -1.
//
// Answered from the SAME layout the drawing uses, so a click cannot land beside the tile that was
// clicked. The card passed in must already be the PLACED card -- the tiles are laid out inside it, and
// the card is moved to hang from its button before either drawing or hit testing.
//
// Declared after SystemPanelFor because it calls it; a forward use of a function defined below does not
// compile in a header that is included in this order.
// What a point on the panel lands on.
//
// One function rather than a growing pile of `...AtScreen` accessors on the window: the panel has several kinds
// of target now (the tiles, the paging arrows, the settings button) and every one of them has to be answered
// from the SAME layout the drawing uses. That rule is why this file exists; a second copy of any of this
// arithmetic is a second chance for a control to be painted where it cannot be pressed.
enum class SystemPanelTarget {
    None = 0,
    Tile,
    PageUp,
    PageDown,
    Gear,
    // The output-device button at the right of the volume row: it opens the "声音输出" page, which is
    // where the devices and the per-application mixer live.
    VolumeDevice,
    // The speaker glyph at the left of the slider. It is a control, not decoration: pressing it mutes,
    // which is what the shell's own flyout does with it.
    Speaker,
    // The slider's track. `fraction` in the hit says where along it the press landed.
    Slider,
};

struct SystemPanelHit {
    SystemPanelTarget target = SystemPanelTarget::None;
    int index = -1;      // the tile, when the target is a tile
    float fraction = 0.0f;   // 0..1 along the track, when the target is the slider
};

inline SystemPanelHit SystemPanelHitFor(const RoundedBox& card, int tileCount, int sliderCount,
                                        int pageIndex, int pageCount, float x, float y) {
    SystemPanelHit hit;
    const bool paging = pageCount > 1;
    const SystemPanelLayout layout = SystemPanelFor(card, tileCount, sliderCount, paging, pageIndex,
                                                    pageCount);
    const auto inside = [x, y](const RectF& r) {
        return r.right > r.left && r.bottom > r.top && x >= r.left && x <= r.right && y >= r.top &&
               y <= r.bottom;
    };

    for (int i = 0; i < layout.tileCount; ++i) {
        if (inside(layout.tiles[i])) {
            hit.target = SystemPanelTarget::Tile;
            hit.index = i;
            return hit;
        }
    }
    if (paging && inside(layout.pageUp)) {
        hit.target = SystemPanelTarget::PageUp;
        return hit;
    }
    if (paging && inside(layout.pageDown)) {
        hit.target = SystemPanelTarget::PageDown;
        return hit;
    }
    if (inside(layout.volumeButton)) {
        hit.target = SystemPanelTarget::VolumeDevice;
        return hit;
    }
    if (inside(layout.speakerIcon)) {
        hit.target = SystemPanelTarget::Speaker;
        return hit;
    }
    for (int i = 0; i < layout.sliderCount; ++i) {
        // The track's own height is 4 DIP, which is far too thin to aim at. The row it sits in is the
        // target, and the fraction is taken from the track -- so a press near the edge of the row still
        // lands somewhere sensible on the bar instead of being ignored.
        const RectF& track = layout.sliders[i];
        const RectF row = {track.left, track.top, track.right, track.bottom};
        if (x >= row.left && x <= row.right && y >= row.top && y <= row.bottom) {
            const float width = track.right - track.left;
            float fraction = width > 0.0f ? (x - track.left) / width : 0.0f;
            if (fraction < 0.0f) fraction = 0.0f;
            if (fraction > 1.0f) fraction = 1.0f;
            hit.target = SystemPanelTarget::Slider;
            hit.index = i;
            hit.fraction = fraction;
            return hit;
        }
    }
    if (inside(layout.gearButton)) {
        hit.target = SystemPanelTarget::Gear;
    }
    return hit;
}

// The tile a point lands on, or -1. Kept as a convenience for the callers that only care about the tiles, and
// defined in terms of SystemPanelHitFor so there is still exactly one piece of arithmetic behind it.
inline int SystemTileAt(const RoundedBox& card, int tileCount, float x, float y) {
    const SystemPanelHit hit = SystemPanelHitFor(card, tileCount, 1, 0, 1, x, y);
    return hit.target == SystemPanelTarget::Tile ? hit.index : -1;
}

// =====================================================================
//  The "声音输出" page -- the panel's second level
//
//  Reached from the output-device button at the right of the volume row, and it is the shell's own page:
//  a back arrow and a title, then 输出设备 (the list, with the current one marked), 空间音效, and 音量合成器
//  (one slider per application that is playing), with 更多音量设置 along the bottom.
//
//  ONE page that scrolls, not three pages: the reference pictures that were handed over are the top and
//  the bottom of the SAME page -- the header and the footer are identical in both -- so the cards are
//  built as one column whose height is allowed to exceed the card, with a scrollbar where the shell has
//  one. Splitting it into three pages would have been a different product.
//
//  Geometry for a page, laid out from the top down: unlike the panel, whose slider is pinned to the
//  bottom, this is a list. The header and the footer do NOT scroll -- they are the page's frame -- and
//  everything between them does.
// =====================================================================

inline constexpr int kMaxSoundDevices = 8;    // output devices listed
inline constexpr int kMaxSoundSessions = 10;  // application rows in the mixer
inline constexpr int kSoundSpatialRows = 2;   // 关, and 用于耳机的 Windows Sonic
// The mixer's rows: the output device itself, which the shell lists first, and then one per application.
inline constexpr int kMaxSoundMixRows = kMaxSoundSessions + 1;

inline constexpr float kSoundHeaderHeightDip = 40.0f;
inline constexpr float kSoundFooterHeightDip = 34.0f;
inline constexpr float kSoundSectionHeightDip = 28.0f;   // a section's own label row
inline constexpr float kSoundDeviceRowHeightDip = 40.0f;
inline constexpr float kSoundSpatialRowHeightDip = 34.0f;
inline constexpr float kSoundMixerRowHeightDip = 36.0f;
inline constexpr float kSoundIndentDip = 14.0f;

struct SoundPageLayout {
    RectF backButton;          // the arrow, top left
    RectF title;               // "声音输出"
    RectF headerRule;          // the hairline under the header

    RectF deviceLabel;         // "输出设备"
    RectF deviceRows[kMaxSoundDevices];
    RectF deviceIcons[kMaxSoundDevices];
    RectF deviceLabels[kMaxSoundDevices];
    RectF deviceMarks[kMaxSoundDevices];   // the accent bar down the left of the current device

    RectF spatialLabel;        // "空间音效"
    RectF spatialRows[kSoundSpatialRows];
    RectF spatialMarks[kSoundSpatialRows];
    RectF spatialLabels[kSoundSpatialRows];

    RectF mixerLabel;          // "音量合成器"
    RectF mixerGear;           // the settings glyph at the section's right
    RectF mixerRows[kMaxSoundMixRows];
    RectF mixerIcons[kMaxSoundMixRows];
    RectF mixerTracks[kMaxSoundMixRows];   // the slider's own rect; the hit takes the fraction from it

    RectF footer;              // "更多音量设置"
    RectF footerRule;

    // The scrollbar, on the card's right edge inside the viewport.
    RectF scrollTrack;
    RectF scrollThumb;

    // The region the content scrolls in -- between the header and the footer. Everything above and below
    // it is the frame and stays put.
    RectF viewport;

    int deviceCount = 0;
    int sessionCount = 0;
    float contentHeight = 0.0f;   // how tall the whole list is, for clamping the scroll
    float scroll = 0.0f;          // the offset that was applied to the rects above
};

// The layout, declared here because the scroll limit below is derived from it: the limit is not a second
// piece of arithmetic, it is "the content's height minus the window it is shown in", read off the one
// layout function every other caller uses.
inline SoundPageLayout SoundPageFor(const RoundedBox& card, int deviceCount, int mixerRowCount,
                                    float scroll);

// How far the page can scroll for a given card and content.
inline float SoundPageScrollLimit(const RoundedBox& card, int deviceCount, int mixerRowCount) {
    const SoundPageLayout probe = SoundPageFor(card, deviceCount, mixerRowCount, 0.0f);
    const float limit = probe.contentHeight - (probe.viewport.bottom - probe.viewport.top);
    return limit > 0.0f ? limit : 0.0f;
}

inline SoundPageLayout SoundPageFor(const RoundedBox& card, int deviceCount, int mixerRowCount,
                                    float scroll) {
    SoundPageLayout layout;
    if (deviceCount < 0) deviceCount = 0;
    if (deviceCount > kMaxSoundDevices) deviceCount = kMaxSoundDevices;
    if (mixerRowCount < 0) mixerRowCount = 0;
    if (mixerRowCount > kMaxSoundMixRows) mixerRowCount = kMaxSoundMixRows;
    layout.deviceCount = deviceCount;
    layout.sessionCount = mixerRowCount;

    const float left = card.rect.left + kSoundIndentDip;
    const float right = card.rect.right - kSoundIndentDip;

    // --- the frame: header and footer, both fixed ---
    layout.headerRule = {card.rect.left, card.rect.top + kSoundHeaderHeightDip - 1.0f,
                         card.rect.right, card.rect.top + kSoundHeaderHeightDip};

    const float backSize = 26.0f;
    const float headerCentre = card.rect.top + (kSoundHeaderHeightDip - 1.0f) * 0.5f;
    layout.backButton = {left, headerCentre - backSize * 0.5f, left + backSize,
                         headerCentre + backSize * 0.5f};
    // The title starts clear of the arrow, at the same indent the sections use.
    layout.title = {left + backSize + 8.0f, card.rect.top + 2.0f, right,
                    card.rect.top + kSoundHeaderHeightDip - 3.0f};

    layout.footerRule = {card.rect.left, card.rect.bottom - kSoundFooterHeightDip,
                         card.rect.right, card.rect.bottom - kSoundFooterHeightDip + 1.0f};
    layout.footer = {card.rect.left, card.rect.bottom - kSoundFooterHeightDip, card.rect.right,
                     card.rect.bottom};

    layout.viewport = {card.rect.left, card.rect.top + kSoundHeaderHeightDip, card.rect.right,
                       card.rect.bottom - kSoundFooterHeightDip};

    // --- the content, laid out as a column from the viewport's top, then moved by the scroll ---
    const float contentTop = layout.viewport.top;
    const float contentRight = right - 6.0f;   // clear of the scrollbar
    float y = contentTop;

    layout.deviceLabel = {left, y, contentRight, y + kSoundSectionHeightDip};
    y += kSoundSectionHeightDip;
    for (int i = 0; i < deviceCount; ++i) {
        const RectF row = {card.rect.left, y, contentRight, y + kSoundDeviceRowHeightDip};
        layout.deviceRows[i] = row;
        // The accent bar: a short rounded rule against the card's left edge, which is how the shell marks
        // the device that is in use.
        layout.deviceMarks[i] = {card.rect.left + 3.0f, y + 9.0f, card.rect.left + 6.0f,
                                 y + kSoundDeviceRowHeightDip - 9.0f};
        const float iconSide = 20.0f;
        const float rowCentre = (row.top + row.bottom) * 0.5f;
        layout.deviceIcons[i] = {left + 8.0f, rowCentre - iconSide * 0.5f, left + 8.0f + iconSide,
                                 rowCentre + iconSide * 0.5f};
        layout.deviceLabels[i] = {left + 8.0f + iconSide + 10.0f, row.top, contentRight, row.bottom};
        y += kSoundDeviceRowHeightDip;
    }

    layout.spatialLabel = {left, y, contentRight, y + kSoundSectionHeightDip};
    y += kSoundSectionHeightDip;
    for (int i = 0; i < kSoundSpatialRows; ++i) {
        const RectF row = {card.rect.left, y, contentRight, y + kSoundSpatialRowHeightDip};
        layout.spatialRows[i] = row;
        layout.spatialMarks[i] = {card.rect.left + 3.0f, y + 7.0f, card.rect.left + 6.0f,
                                  y + kSoundSpatialRowHeightDip - 7.0f};
        layout.spatialLabels[i] = {left + 8.0f, row.top, contentRight, row.bottom};
        y += kSoundSpatialRowHeightDip;
    }

    layout.mixerLabel = {left, y, contentRight, y + kSoundSectionHeightDip};
    // The gear sits at the section's right, level with its label.
    const float gearSide = 20.0f;
    layout.mixerGear = {contentRight - gearSide, y + (kSoundSectionHeightDip - gearSide) * 0.5f,
                        contentRight, y + (kSoundSectionHeightDip + gearSide) * 0.5f};
    y += kSoundSectionHeightDip;
    for (int i = 0; i < mixerRowCount; ++i) {
        const RectF row = {card.rect.left, y, contentRight, y + kSoundMixerRowHeightDip};
        layout.mixerRows[i] = row;
        const float iconSide = 22.0f;
        const float rowCentre = (row.top + row.bottom) * 0.5f;
        layout.mixerIcons[i] = {left + 6.0f, rowCentre - iconSide * 0.5f, left + 6.0f + iconSide,
                                rowCentre + iconSide * 0.5f};
        // The track runs from clear of the icon to the card's right indent. The slider's own height is the
        // row's, as everywhere else in this file: a 4 DIP bar is not something to aim at.
        layout.mixerTracks[i] = {left + 6.0f + iconSide + 14.0f, y, contentRight,
                                 y + kSoundMixerRowHeightDip};
        y += kSoundMixerRowHeightDip;
    }
    layout.contentHeight = y - contentTop;

    // --- the scroll ---
    const float viewportHeight = layout.viewport.bottom - layout.viewport.top;
    float limit = layout.contentHeight - viewportHeight;
    if (limit < 0.0f) limit = 0.0f;
    if (scroll < 0.0f) scroll = 0.0f;
    if (scroll > limit) scroll = limit;
    layout.scroll = scroll;

    // Everything that scrolls moves here, in one place, so a row's rectangle can never be drawn at one
    // offset and hit-tested at another.
    if (scroll != 0.0f) {
        const auto shift = [scroll](RectF& r) {
            r.top -= scroll;
            r.bottom -= scroll;
        };
        shift(layout.deviceLabel);
        for (int i = 0; i < deviceCount; ++i) {
            shift(layout.deviceRows[i]);
            shift(layout.deviceIcons[i]);
            shift(layout.deviceLabels[i]);
            shift(layout.deviceMarks[i]);
        }
        shift(layout.spatialLabel);
        for (int i = 0; i < kSoundSpatialRows; ++i) {
            shift(layout.spatialRows[i]);
            shift(layout.spatialMarks[i]);
            shift(layout.spatialLabels[i]);
        }
        shift(layout.mixerLabel);
        shift(layout.mixerGear);
        for (int i = 0; i < mixerRowCount; ++i) {
            shift(layout.mixerRows[i]);
            shift(layout.mixerIcons[i]);
            shift(layout.mixerTracks[i]);
        }
    }

    // The scrollbar: a hairline track down the inside of the card's right edge, with a thumb as tall as
    // the visible fraction of the content. Drawn whether or not there is anything to scroll, which is
    // what the shell does -- its own thumb is a fixed short bar rather than a proportional one.
    const float barWidth = 3.0f;
    layout.scrollTrack = {card.rect.right - 6.0f, layout.viewport.top,
                          card.rect.right - 6.0f + barWidth, layout.viewport.bottom};
    const float trackHeight = layout.scrollTrack.bottom - layout.scrollTrack.top;
    const float fraction =
        layout.contentHeight > 0.0f ? viewportHeight / layout.contentHeight : 1.0f;
    float thumbHeight = trackHeight * (fraction < 1.0f ? fraction : 1.0f);
    const float minThumb = 28.0f;
    if (thumbHeight < minThumb) thumbHeight = minThumb;
    if (thumbHeight > trackHeight) thumbHeight = trackHeight;
    const float travel = trackHeight - thumbHeight;
    const float position = limit > 0.0f ? (layout.scroll / limit) * travel : 0.0f;
    layout.scrollThumb = {layout.scrollTrack.left, layout.scrollTrack.top + position,
                          layout.scrollTrack.right, layout.scrollTrack.top + position + thumbHeight};

    return layout;
}

// What a point on the sound page lands on.
enum class SoundPageTarget {
    None = 0,
    Back,
    Device,        // one row of the output-device list
    Spatial,       // one of the 空间音效 rows
    MixerSlider,   // a slider in 音量合成器
    MixerGear,     // the gear beside 音量合成器's label
    Footer,        // 更多音量设置
    Scrollbar,
};

struct SoundPageHit {
    SoundPageTarget target = SoundPageTarget::None;
    int index = -1;
    float fraction = 0.0f;   // where along the slider, for MixerSlider
};

inline SoundPageHit SoundPageHitFor(const RoundedBox& card, int deviceCount, int mixerRowCount,
                                    float scroll, float x, float y) {
    SoundPageHit hit;
    const SoundPageLayout layout = SoundPageFor(card, deviceCount, mixerRowCount, scroll);
    const auto inside = [x, y](const RectF& r) {
        return r.right > r.left && r.bottom > r.top && x >= r.left && x <= r.right && y >= r.top &&
               y <= r.bottom;
    };

    // The frame first: it is outside the viewport, so it cannot be scrolled past.
    if (inside(layout.backButton)) {
        hit.target = SoundPageTarget::Back;
        return hit;
    }
    if (inside(layout.footer)) {
        hit.target = SoundPageTarget::Footer;
        return hit;
    }
    if (inside(layout.mixerGear)) {
        hit.target = SoundPageTarget::MixerGear;
        return hit;
    }
    if (inside(layout.scrollTrack)) {
        hit.target = SoundPageTarget::Scrollbar;
        return hit;
    }

    // ... and then only inside the viewport, because a row scrolled out of it is not on screen and must
    // not be clickable through the header.
    const RectF& view = layout.viewport;
    if (x < view.left || x > view.right || y < view.top || y > view.bottom) return hit;

    for (int i = 0; i < layout.deviceCount; ++i) {
        if (inside(layout.deviceRows[i])) {
            hit.target = SoundPageTarget::Device;
            hit.index = i;
            return hit;
        }
    }
    for (int i = 0; i < kSoundSpatialRows; ++i) {
        if (inside(layout.spatialRows[i])) {
            hit.target = SoundPageTarget::Spatial;
            hit.index = i;
            return hit;
        }
    }
    for (int i = 0; i < layout.sessionCount; ++i) {
        const RectF& track = layout.mixerTracks[i];
        if (inside(track)) {
            const float width = track.right - track.left;
            float fraction = width > 0.0f ? (x - track.left) / width : 0.0f;
            if (fraction < 0.0f) fraction = 0.0f;
            if (fraction > 1.0f) fraction = 1.0f;
            hit.target = SoundPageTarget::MixerSlider;
            hit.index = i;
            hit.fraction = fraction;
            return hit;
        }
    }
    return hit;
}

inline MusicCardLayout MusicCardFor(const RoundedBox& box) {
    MusicCardLayout layout;
    const float pad = kMusicCardPadDip;

    layout.artBackground = box.rect;

    // Laid out from the bottom up, so the controls and the bar always sit on the
    // card's lower edge regardless of how tall it is, and the cover takes the space
    // that is left.
    const float bottom = box.rect.bottom - pad;

    layout.controls.left = box.rect.left + pad;
    layout.controls.right = box.rect.right - pad;
    layout.controls.bottom = bottom;
    layout.controls.top = bottom - kMusicPlaySizeDip;

    layout.progress.left = box.rect.left + pad;
    layout.progress.right = box.rect.right - pad;
    layout.progress.bottom = layout.controls.top - 18.0f;
    layout.progress.top = layout.progress.bottom - kMusicProgressBarDip;

    // The times sit AT THE BAR'S LEVEL, one at each end -- the row reads as "03:30 --- 03:49" rather
    // than as two numbers floating above a line, which is what the reference flyout does. They are
    // centred on the bar's own vertical centre, so the row is one line however tall the text is.
    const float timeRowTop =
        layout.progress.top - (kMusicTimeRowDip - kMusicProgressBarDip) * 0.5f;
    layout.elapsed.left = layout.progress.left;
    layout.elapsed.right = layout.elapsed.left + 56.0f;
    layout.elapsed.top = timeRowTop;
    layout.elapsed.bottom = timeRowTop + kMusicTimeRowDip;

    layout.remaining.right = layout.progress.right;
    layout.remaining.left = layout.remaining.right - 56.0f;
    layout.remaining.top = timeRowTop;
    layout.remaining.bottom = timeRowTop + kMusicTimeRowDip;

    // The app row is the top strip, inside the padding: the source application's own icon and
    // name, left aligned, which is the reference image's arrangement and leaves the row's rest
    // free for the cover that starts just below it.
    //
    // The volume control that used to sit at this row's right-hand end was removed on request
    // (see the note above MusicCardLayout): the row is the source's again, and the name is given
    // the width the readout and bar used to hold.
    layout.appRow.left = box.rect.left + pad;
    layout.appRow.right = box.rect.right - pad;
    layout.appRow.top = box.rect.top + pad;
    layout.appRow.bottom = layout.appRow.top + kMusicAppRowHeightDip;

    const float rowCentreY = (layout.appRow.top + layout.appRow.bottom) * 0.5f;

    // Source, at the left: the application's own icon, then the name.
    layout.appIconSize = kMusicAppIconSizeDip;
    layout.appIcon.left = layout.appRow.left;
    layout.appIcon.top = rowCentreY - layout.appIconSize * 0.5f;
    layout.appIcon.right = layout.appIcon.left + layout.appIconSize;
    layout.appIcon.bottom = layout.appIcon.top + layout.appIconSize;

    layout.appText.left = layout.appIcon.right + 10.0f;
    layout.appText.right = layout.appRow.right;
    layout.appText.top = layout.appRow.top;
    layout.appText.bottom = layout.appRow.bottom;

    // Artwork takes the space between the app row and the time row, square.
    layout.artwork.left = box.rect.left + pad;
    layout.artwork.top = layout.appRow.bottom + 8.0f;
    layout.artwork.right = layout.artwork.left + kMusicArtworkSizeDip;
    layout.artwork.bottom = layout.artwork.top + kMusicArtworkSizeDip;

    // Title and subtitle to the right of the artwork, vertically centred against it.
    layout.title.left = layout.artwork.right + kMusicGapAfterArtDip;
    layout.title.right = box.rect.right - pad;
    layout.title.top = layout.artwork.top + 16.0f;
    layout.title.bottom = layout.title.top + 28.0f;

    layout.subtitle.left = layout.title.left;
    layout.subtitle.right = layout.title.right;
    layout.subtitle.top = layout.title.bottom + 2.0f;
    layout.subtitle.bottom = layout.subtitle.top + 20.0f;

    // Three buttons, centred as a group. The play disc is 46 and the skips 38, which is what makes the
    // transport read as play-with-two-skips rather than as three equal buttons.
    const float groupWidth = kMusicControlSizeDip * 2.0f + kMusicPlaySizeDip +
                             kMusicControlGapDip * 2.0f;
    const float groupLeft = (box.rect.left + box.rect.right) * 0.5f - groupWidth * 0.5f;

    layout.previous.left = groupLeft;
    layout.previous.right = groupLeft + kMusicControlSizeDip;
    layout.playPause.left = layout.previous.right + kMusicControlGapDip;
    layout.playPause.right = layout.playPause.left + kMusicPlaySizeDip;
    layout.next.left = layout.playPause.right + kMusicControlGapDip;
    layout.next.right = layout.next.left + kMusicControlSizeDip;

    // The disc's own height is the row's; the skips are centred inside it, so their click targets are
    // as tall as the row they live in.
    const auto setVertical = [&](RectF& r) {
        r.top = layout.controls.top;
        r.bottom = layout.controls.bottom;
    };
    setVertical(layout.previous);
    setVertical(layout.playPause);
    setVertical(layout.next);

    return layout;
}

// Which music-card control a window-local DIP point lands on.
inline CardControl MusicControlAt(const RoundedBox& box, float x, float y) {
    const MusicCardLayout layout = MusicCardFor(box);
    const auto inside = [x, y](const RectF& r) {
        return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
    };
    if (inside(layout.previous)) return CardControl::Previous;
    if (inside(layout.playPause)) return CardControl::PlayPause;
    if (inside(layout.next)) return CardControl::Next;
    return CardControl::None;
}

} // namespace island
