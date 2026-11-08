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
#include "domain/StartSearch.h"
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

struct CardFaceSize {
    float widthDip = kExpandedWidthDip;
    float heightDip = kExpandedHeightDip;
};

// --- the tray page: the applications' tray icons, inside the panel's own card ------------------
//
// A PAGE of the panel rather than a fourth card, for the same reason the 声音输出 page is one: it is the
// same card in the same place with the same open/close animation, and only its contents change -- which is
// what the shell's own flyouts do too, and it means no new card geometry, no new policy rule and no new
// transition. See Renderer::RenderFrame's panel branch.
//
// WHAT THE PAGE PRINTS, top to bottom: a header row with a green status dot, the page's title and a count
// chip; a section row; the grid of tray icons; one "now playing" row standing on an inset well; and a footer
// with a single real "task manager" action at the right. It keeps the full 520 x 500 face while the system
// tiles page uses its own compact face, so the
// bands add up to this page's height -- the arithmetic is written down here so a moved number cannot
// leave a band hanging off the edge (the rule SystemPanelFor follows):
//
//     padding top                                                            18
//     header (status dot, title, count chip)                                 40
//     gap to the section row                                                  8
//     section row (one dim word)                                             18
//     gap to the grid                                                        10
//     grid band (flexible, centred)                                         268
//     gap to the now-playing well                                            12
//     now-playing well (26 DIP mark, two lines, inset surface)               52
//     gap to the footer                                                      12
//     footer (one 168 x 36 task-manager pill at the right)                   48
//     padding bottom                                                         18
//                                                                          ----
//                                                                          500
//
// THE GRID IS FIVE COLUMNS WIDE, as the reference's is, and the CELL is the design's own 56 DIP -- which
// is also exactly what a four-row page of sixteen icons comes out at. A page with fewer rows grows toward
// -- but not past -- 76: the extra air is what keeps a two-row page from reading as a strip of stamps in a
// 520 wide card. The columns stay five however many icons there are, so the column count never has to be
// explained to a reader; the cell is the only number that moves with the icon count, and it is derived
// from the card, so `TrayPageCellAt` (which calls this function) cannot disagree with the drawing.
inline constexpr int   kTrayPageColumns        = 5;
inline constexpr float kTrayPagePadDip         = 20.0f;
inline constexpr float kTrayPageHeaderDip      = 40.0f;
inline constexpr float kTrayPageHeaderGapDip   = 8.0f;
inline constexpr float kTrayPageSectionDip     = 18.0f;
inline constexpr float kTrayPageSectionGapDip  = 10.0f;
// The cell the design calls for. It is also exactly what a four-row page of sixteen icons comes out at,
// which makes "the design size" and "the size this machine gets" the same number rather than two that
// happen to be close.
inline constexpr float kTrayPageCellDip        = 56.0f;
inline constexpr float kTrayPageCellGapDip     = 12.0f;
// The floor a cell must clear to still be a target a finger can find, and the ceiling it may grow to when
// a page needs fewer rows than the band has room for. The floor is applied only when the band can hold it
// (see TrayPageFor): the page is composed in a fixed 520 x 500 card, where the worst case -- 24 icons, five
// rows -- still clears the pressable floor.
inline constexpr float kTrayPageMinCellDip     = 38.0f;
inline constexpr float kTrayPageMaxCellDip     = 76.0f;
inline constexpr float kTrayPageNowDip         = 52.0f;
inline constexpr float kTrayPageGapDip         = 12.0f;
inline constexpr float kTrayPageFooterDip      = 48.0f;
inline constexpr float kTrayPageButtonDip      = 34.0f;
inline constexpr float kTrayPageChipHeightDip  = 22.0f;
inline constexpr float kTrayPageChipWidthDip   = 96.0f;
// The header's status dot.
inline constexpr float kTrayPageDotDip         = 8.0f;
inline constexpr float kTrayPageNowIconDip     = 26.0f;
// The width reserved for the page's title, between the status dot and the count chip. The caption is the
// caller's ("系统后台托盘": six 13 DIP characters, so 78 DIP of ink), and the chip stands 12 DIP after the
// reserved width -- the 6 DIP of air is what keeps the LAST glyph out of the chip if the text face's
// fallback rounds the advance up. Written as a number rather than measured because this function is pure:
// it must give the hit test and the drawing the same arithmetic without a DWrite factory.
inline constexpr float kTrayPageTitleDip       = 84.0f;

struct TrayPageLayout {
    // The header row: a green dot, the page's title and the count chip.
    RectF header;
    RectF headerDot;
    RectF title;
    RectF countChip;
    RectF countText;
    // The section row above the grid.
    RectF section;
    RectF sectionLabel;   // 驻留托盘应用
    // The grid.
    RectF cells[kMaxTrayIcons];
    // The now-playing row's inset well, and the parts of the row: its mark, the state word and the track.
    RectF nowPlaying;
    RectF nowIcon;
    RectF nowLabel;
    RectF nowTitle;
    // The footer: the hairline at its top edge and the one real Task Manager button.
    RectF footer;
    RectF footerTaskManager;
    RectF footerTaskManagerLabel;
    RectF footerGear;
    int   count = 0;
    int   columns = 0;
};

// Where the page's contents go, inside the card the renderer was given. Pure, so the hit test and the
// drawing cannot disagree about which cell an icon is in, and so every band can be asserted from numbers.
inline TrayPageLayout TrayPageFor(const RoundedBox& card, int iconCount) {
    TrayPageLayout layout;
    if (iconCount < 0) iconCount = 0;
    if (iconCount > kMaxTrayIcons) iconCount = kMaxTrayIcons;
    layout.count = iconCount;

    const float cardWidth = card.rect.right - card.rect.left;
    const float pad = kTrayPagePadDip;
    const float left = card.rect.left + pad;
    const float right = card.rect.right - pad;

    // --- the header row -------------------------------------------------------------------------
    layout.header.left = left;
    layout.header.right = right;
    layout.header.top = card.rect.top + pad;
    layout.header.bottom = layout.header.top + kTrayPageHeaderDip;
    const float headerCentre = (layout.header.top + layout.header.bottom) * 0.5f;

    layout.headerDot.left = left;
    layout.headerDot.right = left + kTrayPageDotDip;
    layout.headerDot.top = headerCentre - kTrayPageDotDip * 0.5f;
    layout.headerDot.bottom = layout.headerDot.top + kTrayPageDotDip;

    layout.title.left = layout.headerDot.right + 10.0f;
    layout.title.right = layout.title.left + kTrayPageTitleDip;
    layout.title.top = layout.header.top;
    layout.title.bottom = layout.header.bottom;

    layout.countChip.left = layout.title.right + 12.0f;
    layout.countChip.right = layout.countChip.left + kTrayPageChipWidthDip;
    layout.countChip.top = headerCentre - kTrayPageChipHeightDip * 0.5f;
    layout.countChip.bottom = layout.countChip.top + kTrayPageChipHeightDip;
    layout.countText.left = layout.countChip.left + 10.0f;
    layout.countText.right = layout.countChip.right - 10.0f;
    layout.countText.top = layout.countChip.top;
    layout.countText.bottom = layout.countChip.bottom;

    // --- the section row ------------------------------------------------------------------------
    layout.section.left = left;
    layout.section.right = right;
    layout.section.top = layout.header.bottom + kTrayPageHeaderGapDip;
    layout.section.bottom = layout.section.top + kTrayPageSectionDip;
    layout.sectionLabel = layout.section;

    // --- the footer, pinned to the card's bottom edge like the panel's --------------------------
    layout.footer.left = left;
    layout.footer.right = right;
    layout.footer.bottom = card.rect.bottom - pad;
    layout.footer.top = layout.footer.bottom - kTrayPageFooterDip;
    const float footerCentre = (layout.footer.top + layout.footer.bottom) * 0.5f;
    layout.footerTaskManager.right = right;
    layout.footerTaskManager.left = right - 168.0f;
    layout.footerTaskManager.top = footerCentre - 18.0f;
    layout.footerTaskManager.bottom = footerCentre + 18.0f;
    layout.footerGear.left = right - kTrayPageButtonDip;
    layout.footerGear.right = right;
    layout.footerGear.top = footerCentre - kTrayPageButtonDip * 0.5f;
    layout.footerGear.bottom = layout.footerGear.top + kTrayPageButtonDip;
    layout.footerTaskManagerLabel.left = layout.footerTaskManager.left + 16.0f;
    layout.footerTaskManagerLabel.right = layout.footerGear.left - 6.0f;
    layout.footerTaskManagerLabel.top = layout.footerTaskManager.top;
    layout.footerTaskManagerLabel.bottom = layout.footerTaskManager.bottom;

    // --- the now-playing well, directly above the footer ---------------------------------------
    layout.nowPlaying.left = left;
    layout.nowPlaying.right = right;
    layout.nowPlaying.bottom = layout.footer.top - kTrayPageGapDip;
    layout.nowPlaying.top = layout.nowPlaying.bottom - kTrayPageNowDip;
    const float nowCentre = (layout.nowPlaying.top + layout.nowPlaying.bottom) * 0.5f;
    layout.nowIcon.left = left + 14.0f;
    layout.nowIcon.right = layout.nowIcon.left + kTrayPageNowIconDip;
    layout.nowIcon.top = nowCentre - kTrayPageNowIconDip * 0.5f;
    layout.nowIcon.bottom = layout.nowIcon.top + kTrayPageNowIconDip;
    layout.nowLabel.left = layout.nowIcon.right + 12.0f;
    layout.nowLabel.right = layout.nowPlaying.right - 16.0f;
    layout.nowLabel.top = layout.nowPlaying.top + 9.0f;
    layout.nowLabel.bottom = layout.nowLabel.top + 18.0f;
    layout.nowTitle.left = layout.nowLabel.left;
    layout.nowTitle.top = layout.nowLabel.bottom;
    layout.nowTitle.bottom = layout.nowPlaying.bottom - 9.0f;
    layout.nowTitle.right = layout.nowPlaying.right - 16.0f;

    if (iconCount == 0) return layout;

    // --- the grid, centred in the band between the section row and the well ---------------------
    const int columns = iconCount < kTrayPageColumns ? iconCount : kTrayPageColumns;
    layout.columns = columns;
    const int rows = (iconCount + columns - 1) / columns;
    const float bandTop = layout.section.bottom + kTrayPageSectionGapDip;
    const float bandBottom = layout.nowPlaying.top - kTrayPageGapDip;
    float band = bandBottom - bandTop;
    if (band < 0.0f) band = 0.0f;
    const float availableWidth = right - left;
    const float byWidth =
        (availableWidth - static_cast<float>(columns - 1) * kTrayPageCellGapDip) / columns;
    const float byHeight = (band - static_cast<float>(rows - 1) * kTrayPageCellGapDip) / rows;
    float cell = byWidth < byHeight ? byWidth : byHeight;
    if (cell > kTrayPageMaxCellDip) cell = kTrayPageMaxCellDip;
    // The pressable floor, applied only when this card can hold it: a box smaller than the design's must
    // not have its cells pushed out of it to satisfy a floor meant for the 520 x 500 card.
    if (cell < kTrayPageMinCellDip &&
        static_cast<float>(rows) * kTrayPageMinCellDip +
                static_cast<float>(rows - 1) * kTrayPageCellGapDip <=
            band + 0.01f &&
        static_cast<float>(columns) * kTrayPageMinCellDip +
                static_cast<float>(columns - 1) * kTrayPageCellGapDip <=
            availableWidth + 0.01f) {
        cell = kTrayPageMinCellDip;
    }
    if (cell < 1.0f) cell = 1.0f;

    const float gridWidth =
        static_cast<float>(columns) * cell + static_cast<float>(columns - 1) * kTrayPageCellGapDip;
    const float gridHeight =
        static_cast<float>(rows) * cell + static_cast<float>(rows - 1) * kTrayPageCellGapDip;
    // Centred BOTH ways: the grid is a group of tiles in the reference, not a page-wide sheet, and a
    // two-row page (this machine's eight icons) sits in the middle of the room rather than under the
    // section row.
    const float gridLeft = card.rect.left + (cardWidth - gridWidth) * 0.5f;
    const float gridTop = bandTop + (band - gridHeight) * 0.5f;

    for (int i = 0; i < iconCount; ++i) {
        const int column = i % columns;
        const int row = i / columns;
        // THE INCOMPLETE LAST ROW IS CENTRED under the full rows. Left-aligning it made a 5+2 page read as
        // a row that had lost three icons -- the empty columns all on the right, where the eye expects the
        // grid to continue -- and on this machine the tray page is 5+2 more often than not.
        const int firstInRow = row * columns;
        const int remaining = iconCount - firstInRow;
        const int rowCount = remaining < columns ? remaining : columns;
        const float rowWidth =
            static_cast<float>(rowCount) * cell +
            static_cast<float>(rowCount - 1) * kTrayPageCellGapDip;
        const float rowLeft = gridLeft + (gridWidth - rowWidth) * 0.5f;
        RectF placed;
        placed.left = rowLeft + static_cast<float>(column) * (cell + kTrayPageCellGapDip);
        placed.top = gridTop + static_cast<float>(row) * (cell + kTrayPageCellGapDip);
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

inline bool TrayPageTaskManagerAt(const RoundedBox& card, int iconCount, float x, float y) {
    const TrayPageLayout layout = TrayPageFor(card, iconCount);
    const RectF& button = layout.footerTaskManager;
    return x >= button.left && x <= button.right && y >= button.top && y <= button.bottom;
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
// words ("再点一次"), the same deliberate two-step arrangement the Start card uses for restart and
// shutdown.
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
inline constexpr float kMusicArtworkSizeDip   = 124.0f;
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
    RectF lyricCurrent;    // the current lyric line, beside the cover
    RectF lyricNext;       // the line after it
    RectF levelBar;        // the thin output-level bar at the card's right edge
    RectF lyricProgress;   // the thin vertical reading position beside it
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
// `Start` is the fourth level: the user's own Start card (see StartPageFor). The bar's own settings
// moved to a separate activating window on 2026-11-08.
enum class PanelLevel { Tiles, Sound, Tray, Start };

// A card's visible face can differ by owner AND level while the state and animation remain shared.
// Start and tray are levels of the SystemPanel owner, so owner alone cannot distinguish the compact
// system-tiles page from the wider pages it shares its lifecycle with.
inline CardFaceSize CardFaceSizeFor(CardOwner owner, PanelLevel level) {
    CardFaceSize size;
    if (owner == CardOwner::Music) {
        size.heightDip = kMusicCardHeightDip;
    } else if (owner == CardOwner::SystemPanel && level == PanelLevel::Tiles) {
        size.widthDip = kSystemPanelWidthDip;
        size.heightDip = kSystemPanelHeightDip;
    }
    return size;
}

inline float CardFaceWidthFor(CardOwner owner, PanelLevel level) {
    return CardFaceSizeFor(owner, level).widthDip;
}

inline float CardFaceHeightFor(CardOwner owner, PanelLevel level = PanelLevel::Tiles) {
    return CardFaceSizeFor(owner, level).heightDip;
}

inline PanelLevel NextPanelLevel(PanelLevel held, bool cardUp, bool panelWanted, bool trayWanted,
                                 bool soundWanted, bool startWanted) {
    if (!cardUp) return PanelLevel::Tiles;
    if (!panelWanted) return held;   // put away: the card keeps its level while it leaves the screen
    if (trayWanted) return PanelLevel::Tray;
    if (soundWanted) return PanelLevel::Sound;
    if (startWanted) return PanelLevel::Start;
    return PanelLevel::Tiles;
}

// WHAT PRESSING THE SYSTEM BUTTON ASKS FOR.
//
// One card, four levels (see NextPanelLevel above), and three controls that open one of them -- so this
// button toggles ITS OWN level, not "a card". While the Start card is up the press SWITCHES the card to
// the tiles and the card stays up; every other case is the shell's own toggle.
//
// The distinction is the whole bug it was written for (2026-11-05): the Start card holds the very same
// request -- `systemPanelOpen` is "a card is wanted", and the Start level sets it -- so a plain toggle
// put the card away instead of switching it, and left the level pointing at Start. Reported as
// "点击开始菜单按钮弹出卡片后 再点击系统按钮 不会弹窗 而且是关闭开始菜单卡片".
inline bool SystemButtonWantsTiles(bool panelWanted, bool startWanted) {
    return startWanted || !panelWanted;
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

// Whether the SYSTEM button should show its open state. `panelCardUp` is "the card on screen is the
// SYSTEM PANEL's own", NOT "some card is up".
//
// The music card and a notification are cards too, and neither was opened by that button -- with the old
// answer ("any card") the chevron flipped every time a song's card appeared, which read as the control
// itself moving (reported 2026-11-06 as "当音乐卡片弹出来的时候 系统按钮图标也在动").
inline bool SystemButtonLitFor(PanelLevel level, bool panelCardUp) {
    return panelCardUp && SystemButtonShowsOpen(level);
}

inline ControlLit ControlLitFor(PanelLevel level, bool panelCardUp) {
    ControlLit lit;
    if (!panelCardUp) return lit;
    lit.system = SystemButtonLitFor(level, panelCardUp);
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
// The 2026-11 reference put a frame around that grid: a search field at the top, a page row under it,
// and the account and power controls in a footer band. The card is still the SAME card as the panel's --
// 520 x 500, hanging from the same button, with the same open/close animation. The search field now owns a
// temporary native editor for real keyboard/IME input, and the grid button opens the all-applications grid.
//
//     padding top                                                          18
//     search field (full inner width, stadium radius)                      52
//     gap to the tab row                                                   12
//     page row: 固定常用 + all-applications grid button                    28
//     gap to the grid                                                      16
//     three rows of 72 DIP cells (a 50 icon with its name beneath)        232
//     ... room below the grid, which the picker uses as it scrolls
//     footer: picture + name, and the four-button power group               48
//     padding bottom                                                       18
//                                                                        ----
//                                                                        500 (with room for the picker)
inline constexpr int   kStartColumns    = 6;
inline constexpr int   kStartRows       = 3;
// The card's own padding: the shared card's 18, so this page's content sits on the same margins as the
// system panel's tiles and the tray page's grid.
inline constexpr float kStartPadDip     = 20.0f;
// The search field is the command-palette silhouette: darker than the card, a fine neutral edge, a
// cyan optical mark at the left, and enough height for the placeholder to breathe.
inline constexpr float kStartSearchDip       = 52.0f;
inline constexpr float kStartSearchRadiusDip = 26.0f;
inline constexpr float kStartSearchIconDip   = 20.0f;
// The tab row: 28 DIP is the reference's px-3.5 py-1 around a 12 DIP caption. The two widths are the
// captions' own ink plus that padding, written down because this function is pure and cannot measure.
// The middle two are reference-only categories in this round: they are drawn and deliberately answer no
// target.
inline constexpr float kStartTabHeightDip        = 28.0f;
inline constexpr float kStartTabsGapDip          = 12.0f;   // search field -> page row
inline constexpr float kStartGridGapDip          = 16.0f;   // page row -> the grid's first icon
inline constexpr float kStartPinnedTabWidthDip   = 88.0f;   // 固定常用
inline constexpr float kStartAllAppsButtonDip    = 34.0f;   // the tab row's only right-hand button
// The grid: six columns of a 44 DIP icon with its name under it, compacted for the 520 DIP card.
//
// The CELL is the whole thing -- icon and caption -- because a tile is one target: the icon and the name
// below it are pressed together, and the name has to be INSIDE the cell or the rows would be hit-tested
// against the gaps as well. The two gaps are separate because the reference's are: the columns are
// separated by 8 and the rows by 8, which keeps a 72 DIP cell with a 44 DIP icon from reading as
// a stamp sheet.
inline constexpr float kStartCellWidth         = 72.0f;
inline constexpr float kStartCellHeight        = 72.0f;
inline constexpr float kStartColumnGap         = 8.0f;
inline constexpr float kStartRowGap            = 8.0f;
inline constexpr float kStartAppIconDip        = 44.0f;
inline constexpr float kStartAppIconRadiusDip  = 10.0f;
inline constexpr float kStartLabelGapDip       = 3.0f;
// The footer. Its band is compact at 48 DIP, and the power group is the reference's pill of four 36 DIP
// round buttons -- the size the panel's footer buttons use, so the two cards' bottoms read as the same
// furniture. Only the fourth button answers (see StartPageHitFor); the lock, the moon and the restart are
// drawn for the reference and are inert in this round.
inline constexpr float kStartFooterDip          = 48.0f;
inline constexpr float kStartAvatarDip          = 30.0f;
inline constexpr float kStartPowerDip           = 36.0f;
inline constexpr float kStartPowerGroupPadDip   = 4.0f;
inline constexpr float kStartPowerGroupGapDip   = 4.0f;
inline constexpr int   kStartPowerButtons       = 4;

struct StartPageLayout {
    // The chrome, drawn on every page because it is the card's frame rather than one page's content: the
    // search field with its magnifier and text box, plus the page row's title and grid button.
    RectF search;
    RectF searchIcon;
    RectF searchHint;
    RectF searchClear;
    RectF tabRow;
    RectF pinnedTab;
    RectF allAppsButton;
    // Where the pins, the add tile and the all-applications grid live.
    RectF grid;
    // The footer: the account picture, the name's box, and the four-button power group.
    RectF footer;
    RectF avatar;
    RectF accountName;
    RectF powerGroup;
    RectF powerButtons[kStartPowerButtons];
    bool  valid = false;
};

// `card` is the card's own rectangle, the same one every other level is laid out in.
inline StartPageLayout StartPageFor(const RoundedBox& card) {
    StartPageLayout layout;
    const float pad = kStartPadDip;
    const float cardWidth = card.rect.right - card.rect.left;
    const float cardHeight = card.rect.bottom - card.rect.top;
    // The grid's own width, and the height the three rows need. A box that cannot hold the chrome, one
    // full grid and the footer is not a card this page can be composed in; every caller skips the page
    // rather than drawing a rectangle with a negative side.
    const float gridWidth =
        kStartColumns * kStartCellWidth + (kStartColumns - 1) * kStartColumnGap;
    const float gridHeight =
        kStartRows * kStartCellHeight + (kStartRows - 1) * kStartRowGap;
    const float fixedHeight = pad * 2.0f + kStartSearchDip + kStartTabsGapDip + kStartTabHeightDip +
                              kStartGridGapDip + gridHeight + 16.0f + kStartFooterDip;
    if (cardWidth < pad * 2.0f + gridWidth || cardHeight < fixedHeight) {
        return layout;
    }

    const float left = card.rect.left + pad;
    const float right = card.rect.right - pad;

    // --- the search field -------------------------------------------------------------------------
    layout.search.left = left;
    layout.search.right = right;
    layout.search.top = card.rect.top + pad;
    layout.search.bottom = layout.search.top + kStartSearchDip;
    const float searchCentreY = (layout.search.top + layout.search.bottom) * 0.5f;
    layout.searchIcon.left = layout.search.left + 18.0f;
    layout.searchIcon.top = searchCentreY - kStartSearchIconDip * 0.5f;
    layout.searchIcon.right = layout.searchIcon.left + kStartSearchIconDip;
    layout.searchIcon.bottom = layout.searchIcon.top + kStartSearchIconDip;
    layout.searchClear.right = layout.search.right - 10.0f;
    layout.searchClear.left = layout.searchClear.right - 30.0f;
    layout.searchClear.top = searchCentreY - 15.0f;
    layout.searchClear.bottom = layout.searchClear.top + 30.0f;
    layout.searchHint.left = layout.searchIcon.right + 14.0f;
    layout.searchHint.right = layout.searchClear.left - 10.0f;
    layout.searchHint.top = layout.search.top;
    layout.searchHint.bottom = layout.search.bottom;

    // --- the page row, under it --------------------------------------------------------------------
    //
    // The card has one page title and one way to the full application grid. The old category tabs were
    // labels without behaviour, and the all-applications text tab duplicated the grid button.
    layout.tabRow.left = left;
    layout.tabRow.right = right;
    layout.tabRow.top = layout.search.bottom + kStartTabsGapDip;
    layout.tabRow.bottom = layout.tabRow.top + kStartTabHeightDip;

    layout.pinnedTab.left = layout.tabRow.left;
    layout.pinnedTab.right = layout.pinnedTab.left + kStartPinnedTabWidthDip;
    layout.pinnedTab.top = layout.tabRow.top;
    layout.pinnedTab.bottom = layout.tabRow.bottom;
    layout.allAppsButton.right = layout.tabRow.right;
    layout.allAppsButton.left = layout.allAppsButton.right - kStartAllAppsButtonDip;
    layout.allAppsButton.top = layout.tabRow.top - 3.0f;
    layout.allAppsButton.bottom = layout.tabRow.bottom + 3.0f;

    // --- the footer, pinned to the card's lower edge ----------------------------------------------
    layout.footer.left = left;
    layout.footer.right = right;
    layout.footer.bottom = card.rect.bottom - pad;
    layout.footer.top = layout.footer.bottom - kStartFooterDip;
    const float footerCentreY = (layout.footer.top + layout.footer.bottom) * 0.5f;

    layout.avatar.left = layout.footer.left;
    layout.avatar.top = footerCentreY - kStartAvatarDip * 0.5f;
    layout.avatar.right = layout.avatar.left + kStartAvatarDip;
    layout.avatar.bottom = layout.avatar.top + kStartAvatarDip;

    // The power group: four round buttons in a pill, against the card's right edge. The pad and the gap
    // are the reference's p-1 / gap-1, so the buttons stand 4 DIP apart inside a 4 DIP rim.
    const float groupWidth = kStartPowerButtons * kStartPowerDip +
                             (kStartPowerButtons - 1) * kStartPowerGroupGapDip +
                             kStartPowerGroupPadDip * 2.0f;
    layout.powerGroup.right = layout.footer.right;
    layout.powerGroup.left = layout.powerGroup.right - groupWidth;
    layout.powerGroup.top =
        footerCentreY - (kStartPowerDip + kStartPowerGroupPadDip * 2.0f) * 0.5f;
    layout.powerGroup.bottom =
        layout.powerGroup.top + kStartPowerDip + kStartPowerGroupPadDip * 2.0f;
    for (int i = 0; i < kStartPowerButtons; ++i) {
        RectF& button = layout.powerButtons[i];
        button.left = layout.powerGroup.left + kStartPowerGroupPadDip +
                      static_cast<float>(i) * (kStartPowerDip + kStartPowerGroupGapDip);
        button.top = footerCentreY - kStartPowerDip * 0.5f;
        button.right = button.left + kStartPowerDip;
        button.bottom = button.top + kStartPowerDip;
    }
    // The account name uses everything between the picture and the power group.
    layout.accountName.left = layout.avatar.right + 10.0f;
    layout.accountName.right = layout.powerGroup.left - 16.0f;
    layout.accountName.top = layout.footer.top;
    layout.accountName.bottom = layout.footer.bottom;

    // --- the grid, between the tab row and the footer ---------------------------------------------
    // The columns are spread across the card's full inner width, centred: the cell size is fixed and the
    // leftover is split, the same rule the tray page's cells follow.
    layout.grid.left = card.rect.left + (cardWidth - gridWidth) * 0.5f;
    layout.grid.right = layout.grid.left + gridWidth;
    layout.grid.top = layout.tabRow.bottom + kStartGridGapDip;
    // The band the picker scrolls in ends a gap above the footer. The pins themselves are only three rows
    // tall and stand at the band's top; the room under them is what the picker uses.
    layout.grid.bottom = layout.footer.top - 16.0f;
    if (layout.grid.bottom < layout.grid.top + gridHeight) {
        return StartPageLayout{};
    }

    layout.valid = true;
    return layout;
}

// --- the grouped search list ---------------------------------------------------------------------
//
// Search uses the same grid band as the pins, but not the six-column grid: applications, Windows Settings
// and files are three lists a reader scans vertically. Eight rows is the largest list that fits without
// scrolling, and the three section headers are part of that arithmetic:
//
//     3 section headers x 16 + 8 rows x 29 + 2 gaps x 4 = 288 DIP
//
// which is exactly the height StartPageFor leaves between the tab row and the footer on the 520 x 500 card.
inline constexpr int   kStartSearchMaxResults = 8;
inline constexpr int   kStartSearchGroupCount = 3;
inline constexpr float kStartSearchHeaderDip   = 16.0f;
inline constexpr float kStartSearchRowDip      = 30.0f;
inline constexpr float kStartSearchGroupGapDip = 4.0f;

struct StartSearchListLayout {
    RectF headers[kStartSearchGroupCount];
    RectF rows[kStartSearchMaxResults];
    int   rowGroup[kStartSearchMaxResults] = {};
    int   groupCounts[kStartSearchGroupCount] = {};
    int   count = 0;
    bool  valid = false;
};

inline StartSearchListLayout StartSearchListFor(const StartPageLayout& page,
                                                const SearchResultKind* kinds, int count) {
    StartSearchListLayout layout;
    if (!page.valid || !kinds || count <= 0) return layout;
    if (count > kStartSearchMaxResults) count = kStartSearchMaxResults;
    layout.count = count;
    for (int i = 0; i < count; ++i) {
        const int group = static_cast<int>(kinds[i]);
        if (group < 0 || group >= kStartSearchGroupCount) return StartSearchListLayout{};
        layout.rowGroup[i] = group;
        ++layout.groupCounts[group];
    }

    int presentGroups = 0;
    for (int groupCount : layout.groupCounts) {
        if (groupCount > 0) ++presentGroups;
    }
    const float fixedHeight =
        static_cast<float>(presentGroups) * kStartSearchHeaderDip +
        static_cast<float>(presentGroups > 0 ? presentGroups - 1 : 0) *
            kStartSearchGroupGapDip;
    float rowHeight =
        (page.grid.bottom - page.grid.top - fixedHeight) / static_cast<float>(count);
    if (rowHeight > kStartSearchRowDip) rowHeight = kStartSearchRowDip;
    if (rowHeight < 1.0f) return StartSearchListLayout{};

    float top = page.grid.top;
    bool haveGroup = false;
    int row = 0;
    for (int group = 0; group < kStartSearchGroupCount; ++group) {
        if (layout.groupCounts[group] == 0) continue;
        if (haveGroup) top += kStartSearchGroupGapDip;
        layout.headers[group] = {page.grid.left, top, page.grid.right,
                                 top + kStartSearchHeaderDip};
        top += kStartSearchHeaderDip;
        haveGroup = true;

        for (int i = 0; i < count; ++i) {
            if (layout.rowGroup[i] != group) continue;
            layout.rows[i] = {page.grid.left, top, page.grid.right, top + rowHeight};
            top += rowHeight;
            ++row;
        }
    }
    layout.valid = row == count && top <= page.grid.bottom + 0.01f;
    return layout;
}

inline int StartSearchResultAt(const StartSearchListLayout& layout, float x, float y) {
    if (!layout.valid) return -1;
    for (int i = 0; i < layout.count; ++i) {
        const RectF& row = layout.rows[i];
        if (x >= row.left && x <= row.right && y >= row.top && y <= row.bottom) return i;
    }
    return -1;
}

// One pinned application's cell, by index. Rows are read left to right, top to bottom, which is also the
// order they were pinned in.
inline RectF StartPinCellFor(const StartPageLayout& layout, int index) {
    RectF cell;
    if (!layout.valid || index < 0) return cell;
    const int column = index % kStartColumns;
    const int row = index / kStartColumns;
    cell.left = layout.grid.left + static_cast<float>(column) * (kStartCellWidth + kStartColumnGap);
    cell.top = layout.grid.top + static_cast<float>(row) * (kStartCellHeight + kStartRowGap);
    cell.right = cell.left + kStartCellWidth;
    cell.bottom = cell.top + kStartCellHeight;
    return cell;
}

// The 44 DIP icon inside a cell: centred horizontally, against the cell's top. The name is under it, so
// the two are separate rectangles rather than one tile: the drawing rounds the icon (radius 10) and centres
// the caption in the room that is left, and neither has to guess where the other is.
inline RectF StartPinIconFor(const StartPageLayout& layout, int index) {
    RectF icon;
    const RectF cell = StartPinCellFor(layout, index);
    if (cell.right <= cell.left) return icon;
    icon.left = cell.left + (kStartCellWidth - kStartAppIconDip) * 0.5f;
    icon.top = cell.top;
    icon.right = icon.left + kStartAppIconDip;
    icon.bottom = icon.top + kStartAppIconDip;
    return icon;
}

// The application's name under its icon, INSIDE the cell: the caption is part of the target, so a press on
// the word is a press on the application (see StartPinCellAt, which divides the whole cell area).
inline RectF StartPinLabelFor(const StartPageLayout& layout, int index) {
    RectF label;
    const RectF cell = StartPinCellFor(layout, index);
    const RectF icon = StartPinIconFor(layout, index);
    if (cell.right <= cell.left) return label;
    label.left = cell.left + 2.0f;
    label.right = cell.right - 2.0f;
    label.top = icon.bottom + kStartLabelGapDip;
    label.bottom = cell.bottom;
    return label;
}

// How many pins the grid can hold, which is what says whether a pin has to wait for a second page.
inline int StartPinCapacity() { return kStartColumns * kStartRows; }

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
    // By ARITHMETIC rather than by testing each cell: the cells are separated by a small gap, and a click
    // two pixels off a tile landing in that gap and doing nothing is a dead zone in a grid the user is
    // meant to fill by hand. Dividing the offset by the pitch makes the whole area belong to some tile.
    const float pitchX = kStartCellWidth + kStartColumnGap;
    const float pitchY = kStartCellHeight + kStartRowGap;
    const int column = static_cast<int>((x - layout.grid.left) / pitchX);
    const int row = static_cast<int>((y - layout.grid.top) / pitchY);
    if (column < 0 || column >= kStartColumns || row < 0 || row >= kStartRows) return -1;
    const int index = row * kStartColumns + column;
    const int capacity = StartPinCapacity();
    const int limit = count < capacity ? count : capacity;
    return index < limit ? index : -1;
}

// One cell of the ALL-APPLICATIONS grid: the same 6-column cells the pins use, scrolled by whole rows.
// Returns the LIST index (scrollRow * kStartColumns + the cell), not the cell index, because that is what
// the drawing and the click need; -1 off the grid or past the end of the list.
inline int StartPickerCellAt(const RoundedBox& card, int scrollRow, int count, float x, float y) {
    const StartPageLayout layout = StartPageFor(card);
    if (!layout.valid || !StartRectHasPoint(layout.grid, x, y)) return -1;
    const float pitchX = kStartCellWidth + kStartColumnGap;
    const float pitchY = kStartCellHeight + kStartRowGap;
    const int column = static_cast<int>((x - layout.grid.left) / pitchX);
    const int row = static_cast<int>((y - layout.grid.top) / pitchY);
    if (column < 0 || column >= kStartColumns || row < 0 || row >= kStartRows) return -1;
    const int index = (scrollRow + row) * kStartColumns + column;
    return index < count ? index : -1;
}

// What a point on the Start card lands on.
//
// ONE function for the pointer's hover, the press that follows it and the right-click that unpins: the
// three used to ask slightly different questions, and the all-applications grid is what paid for it -- it
// was DRAWN as 6-column cells and hit-tested as 34 DIP list rows on one of the paths, so a click on the
// second half of an icon opened whatever the list thought was there.
//
// The card passed in must already be the PLACED card (the one the drawing was given), and the page, the
// two counts and the scroll must be the same numbers the drawing was given -- they are the frame's, told
// to the window for exactly this reason.
enum class StartPageTarget {
    None = 0,
    Search,       // the text field; starts the native search editor
    Pin,          // one pinned application
    PickerRow,    // one application in the all-applications grid
    PowerButton,  // one of the four direct power controls
    EmptyGrid,    // the pins area away from every tile: pressing it adds a shortcut
    AllApps,      // the grid button at the right of the page row
    PinnedTab,    // 固定常用: return to the pins
    AddTile,      // the dashed "+ 添加快捷键" tile at the end of the pins
    // THE CARD ITSELF: its chrome and its gaps. It is
    // NOT a control -- nothing happens -- but a press here must still be CONSUMED, because the panel's own
    // tiles are laid out in the same rectangle one level down and `bar.systemPanelOpen` is set for the Start
    // level too. Without this, pressing the card's background fell through to the panel and closed it.
    Chrome,
    ClearSearch,  // the x inside the search field
};

struct StartPageHit {
    StartPageTarget target = StartPageTarget::None;
    int index = -1;
};

inline StartPageHit StartPageHitFor(const RoundedBox& card, int page, int pinCount, int listCount,
                                    int pickerScrollRow, float x, float y,
                                    bool searchActive = false,
                                    const SearchResultKind* resultKinds = nullptr,
                                    int resultCount = 0, bool searchHasText = false) {
    StartPageHit hit;
    const StartPageLayout layout = StartPageFor(card);
    if (!layout.valid || !StartRectHasPoint(card.rect, x, y)) return hit;

    // Text entry first, because the search field spans a wide top band.
    if (searchHasText && StartRectHasPoint(layout.searchClear, x, y)) {
        hit.target = StartPageTarget::ClearSearch;
        return hit;
    }
    if (StartRectHasPoint(layout.search, x, y)) {
        hit.target = StartPageTarget::Search;
        return hit;
    }
    // The four footer controls are direct actions.
    for (int i = 0; i < kStartPowerButtons; ++i) {
        if (StartRectHasPoint(layout.powerButtons[i], x, y)) {
            hit.target = StartPageTarget::PowerButton;
            hit.index = i;
            return hit;
        }
    }
    // Then the page-row controls, which are drawn on every page.
    if (StartRectHasPoint(layout.pinnedTab, x, y)) {
        hit.target = StartPageTarget::PinnedTab;
        return hit;
    }
    if (StartRectHasPoint(layout.allAppsButton, x, y)) {
        hit.target = StartPageTarget::AllApps;
        return hit;
    }

    if (page == 1) {
        int index = -1;
        if (searchActive) {
            const StartSearchListLayout results =
                StartSearchListFor(layout, resultKinds, resultCount);
            index = StartSearchResultAt(results, x, y);
        } else {
            index = StartPickerCellAt(card, pickerScrollRow, listCount, x, y);
        }
        if (index >= 0) {
            hit.target = StartPageTarget::PickerRow;
            hit.index = index;
            return hit;
        }
    } else {
        const int pin = StartPinCellAt(layout, pinCount, x, y);
        if (pin >= 0) {
            hit.target = StartPageTarget::Pin;
            hit.index = pin;
            return hit;
        }
        // The add tile is the cell AFTER the last pin, and it is a real one: pressing it opens the picker,
        // the same as pressing the empty part of the grid. It only exists while there is room for it.
        if (pinCount < StartPinCapacity()) {
            const RectF addCell = StartPinCellFor(layout, pinCount);
            if (StartRectHasPoint(addCell, x, y)) {
                hit.target = StartPageTarget::AddTile;
                return hit;
            }
        }
        if (StartRectHasPoint(layout.grid, x, y)) {
            hit.target = StartPageTarget::EmptyGrid;
            return hit;
        }
    }

    // Everything left inside the card is its own body: drawn chrome and the room between the controls.
    hit.target = StartPageTarget::Chrome;
    return hit;
}

// --- the system panel --------------------------------------------------------------
//
// What the row of system indicators summarises, opened out, in the arrangement of the reference control
// centre (2026-11-06):
//
//     [ tile ][ tile ][ tile ]          2 rows of 3, each 82 DIP tall and radius 14
//     [ tile ][ tile ][ tile ]
//     +--------------------------------+
//     | [speaker] 音量         78% [dev] |
//     | ==============o--------------  |   the volume row
//     | [sun] 显示亮度              85% |   (the reading is orange)
//     | =================o-----------  |   the brightness row
//     +--------------------------------+
//     (battery) 98% 充电中    (moon)(speaker)(gear)
//
// The tiles have an ON and an OFF appearance rather than being a list of readings. A row that says
// "Wi-Fi: not connected" tells the user something; a tile that is dark tells them the same thing AND
// what pressing it will do. The tile carries its own caption now: the name in the reference's bold
// white and the state under it in the dim face, both INSIDE the rounded box -- the reference's tile is
// one surface, not a box with a caption hanging under it.
//
// Like the music card, this is one CARD -- same state, same shape, same radius -- because it is the
// same kind of thing: something that opens downward from the bar to show more than the bar has room
// for. The compact card is 520 x 500 (Motion.h), and the bands below add up to 500.
inline constexpr float kSystemTileHeightDip = 74.0f;
inline constexpr float kSystemTileGapDip = 8.0f;

inline constexpr int kSystemTileColumns = 3;
inline constexpr int kSystemTileRows = 2;
inline constexpr int kMaxSystemTiles = kSystemTileColumns * kSystemTileRows;

// How many tiles the panel can HOLD, as against how many it shows at once: the shell's flyout pages through
// its tiles, and this is the ceiling on the whole set.
inline constexpr int kMaxSystemPanelTiles = 3 * kMaxSystemTiles;

struct SystemPanelLayout {
    // The tile itself: one rounded surface carrying the glyph, the chevron, the name and the state.
    // The two caption rectangles are INSIDE it, which is the reference's arrangement.
    RectF tiles[kMaxSystemTiles];
    RectF tileIcons[kMaxSystemTiles];
    RectF tileLabels[kMaxSystemTiles];      // the bold name at the tile's lower left
    RectF tileSubLabels[kMaxSystemTiles];   // the dim state line under the name
    // The inset surface the two control rows stand on, and the parts of each row: its label at the
    // left, its reading at the right, and the row the track is drawn and pressed in.
    RectF sliderPanel;
    RectF sliderLabels[2];
    RectF sliderValues[2];
    RectF sliders[2];
    // The visible row is 32 DIP high, but the mouse gets a 36 DIP target centred on it. Keeping the two
    // separate preserves the larger hit target without making the compressed panel look taller.
    RectF sliderHitRows[2];
    // The complete tile band, including its gaps. The wheel uses this rectangle rather than each tile,
    // so scrolling over the space between two tiles still feels like scrolling the grid.
    RectF tileArea;
    RectF speakerIcon;        // at the left of the volume row's header
    // At the left of the brightness row's header. Its own rectangle rather than a shared one, because
    // the two rows carry different marks -- a speaker and a sun.
    RectF brightnessIcon;
    // The output-device button at the RIGHT of the volume row: the device glyph and the chevron that
    // opens the "声音输出" page, which is what the shell's own flyout puts there.
    RectF volumeButton;
    // The footer: the battery plate at the left and the three round buttons at the right.
    RectF footer;
    RectF footerText;
    RectF footerSubText;
    RectF footerBattery;
    RectF footerButtons[3];
    // The visible circles are 32 DIP while their hit regions stay 40 DIP.
    RectF footerButtonHits[3];
    RectF gearButton;
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

// The panel's geometry for a card box and a number of tiles, in the reference's three bands.
//
// TOP-DOWN, which is a change. The old panel stacked its sliders up from the settings button because
// they were pinned to the card's lower edge. The reference pins the FOOTER there instead, and the tile
// grid and the inset slider surface are one group above it -- so the grid starts at the card's top,
// the panel sits directly under it, and only the footer answers to the card's bottom edge.
//
// The arithmetic is the whole of the compact card's 408 DIP, and it is written down here so a moved number
// cannot leave a band hanging off the edge:
//
//     padding top                                    18
//     2 rows of 74 tiles + one 8 gap                156
//     gap to the inset panel                          12
//     inset: 8 pad + 54 row + 8 gap + 54 row + 8    132
//     gap to the footer                               12
//     footer (three 32 DIP buttons, inset plate)       60
//     padding bottom                                  18
//                                                   ----
//                                                   408
//
// A row inside the inset panel is an 18 DIP header (glyph, name, reading) and a 32 DIP track row. The
// visible 6 DIP track is centred in it; the hit rectangle grows to 36 DIP by extending two DIP each way.
inline SystemPanelLayout SystemPanelFor(const RoundedBox& box, int tileCount, int sliderCount,
                                        bool paging = false, int pageIndex = 0, int pageCount = 1) {
    SystemPanelLayout layout;

    if (tileCount < 0) tileCount = 0;
    if (tileCount > kMaxSystemTiles) tileCount = kMaxSystemTiles;
    if (sliderCount < 0) sliderCount = 0;
    if (sliderCount > 2) sliderCount = 2;
    layout.tileCount = tileCount;
    layout.sliderCount = sliderCount;

    const float pad = 18.0f;
    const float left = box.rect.left + pad;
    const float right = box.rect.right - pad;

    // The paging controls live in the card's right PADDING, not in the grid. Subtracting a pager column from
    // the grid made the three tiles visibly left-heavy; overlaying the arrows in the margin keeps the grid
    // symmetric while leaving every real control where the reference has it.
    const float pagingWidth = paging ? 20.0f : 0.0f;
    const float gridRight = right;
    const float innerWidth = gridRight - left;

    // THE FOOTER, pinned to the card's bottom edge: the battery plate at the left and three 32 DIP
    // round buttons at the right. Their hit regions are wider than their visible circles.
    const float footerButton = 32.0f;
    const float footerButtonHit = 40.0f;
    const float footerGap = 8.0f;
    const float footerHeight = 60.0f;
    layout.footer.left = left;
    layout.footer.right = right;
    layout.footer.bottom = box.rect.bottom - pad;
    layout.footer.top = layout.footer.bottom - footerHeight;
    for (int i = 0; i < 3; ++i) {
        const float buttonRight =
            right - 14.0f - static_cast<float>(2 - i) * (footerButton + footerGap);
        const float buttonTop = (layout.footer.top + layout.footer.bottom - footerButton) * 0.5f;
        layout.footerButtons[i].left = buttonRight - footerButton;
        layout.footerButtons[i].right = buttonRight;
        layout.footerButtons[i].top = buttonTop;
        layout.footerButtons[i].bottom = buttonTop + footerButton;
        const float centreX = (layout.footerButtons[i].left + layout.footerButtons[i].right) * 0.5f;
        layout.footerButtonHits[i].left = centreX - footerButtonHit * 0.5f;
        layout.footerButtonHits[i].right = centreX + footerButtonHit * 0.5f;
        layout.footerButtonHits[i].top =
            (layout.footer.top + layout.footer.bottom - footerButtonHit) * 0.5f;
        layout.footerButtonHits[i].bottom = layout.footerButtonHits[i].top + footerButtonHit;
    }
    layout.gearButton = layout.footerButtons[2];
    layout.footerBattery.left = layout.footer.left;
    layout.footerBattery.right = layout.footerButtonHits[0].left - 4.0f;
    layout.footerBattery.top = layout.footer.top;
    layout.footerBattery.bottom = layout.footer.bottom;
    layout.footerText.left = left + 14.0f;
    layout.footerText.right = layout.footerBattery.right - 14.0f;
    const float footerCentreY = (layout.footer.top + layout.footer.bottom) * 0.5f;
    layout.footerText.top = footerCentreY - 20.0f;
    layout.footerText.bottom = footerCentreY - 2.0f;
    layout.footerSubText.left = layout.footerText.left + 30.0f;
    layout.footerSubText.right = layout.footerText.right;
    layout.footerSubText.top = footerCentreY + 2.0f;
    layout.footerSubText.bottom = footerCentreY + 20.0f;

    // THE TILE GRID, from the card's top: two rows of 74 DIP tiles, with the name and the state INSIDE
    // each tile. The grid stops short of the paging column when there is one.
    const int rows = (tileCount + kSystemTileColumns - 1) / kSystemTileColumns;
    const float gridTop = box.rect.top + pad;
    float gridBottom = gridTop;
    layout.tileArea.left = left;
    layout.tileArea.right = right;
    layout.tileArea.top = gridTop;
    if (rows > 0) {
        const float tileWidth =
            (innerWidth - static_cast<float>(kSystemTileColumns - 1) * kSystemTileGapDip) /
            static_cast<float>(kSystemTileColumns);
        for (int i = 0; i < tileCount; ++i) {
            const int row = i / kSystemTileColumns;
            const int column = i % kSystemTileColumns;
            const float tileLeft =
                left + static_cast<float>(column) * (tileWidth + kSystemTileGapDip);
            const float tileTop =
                gridTop + static_cast<float>(row) * (kSystemTileHeightDip + kSystemTileGapDip);
            RectF& tile = layout.tiles[i];
            tile.left = tileLeft;
            tile.right = tileLeft + tileWidth;
            tile.top = tileTop;
            tile.bottom = tileTop + kSystemTileHeightDip;

            // The glyph at the tile's TOP LEFT, at the size the reference draws it.
            const float glyphSize = 20.0f;
            layout.tileIcons[i].left = tileLeft + 12.0f;
            layout.tileIcons[i].right = layout.tileIcons[i].left + glyphSize;
            layout.tileIcons[i].top = tileTop + 12.0f;
            layout.tileIcons[i].bottom = layout.tileIcons[i].top + glyphSize;

            // The name and the state, stacked at the tile's BOTTOM LEFT: 18 DIP for the bold name and
            // 14 for the dim line under it, with the same 16 DIP inset the glyph uses.
            layout.tileSubLabels[i].left = tileLeft + 12.0f;
            layout.tileSubLabels[i].right = tile.right - 12.0f;
            layout.tileSubLabels[i].bottom = tile.bottom - 8.0f;
            layout.tileSubLabels[i].top = layout.tileSubLabels[i].bottom - 12.0f;
            layout.tileLabels[i].left = layout.tileSubLabels[i].left;
            layout.tileLabels[i].right = layout.tileSubLabels[i].right;
            layout.tileLabels[i].bottom = layout.tileSubLabels[i].top;
            layout.tileLabels[i].top = layout.tileLabels[i].bottom - 16.0f;
        }
        gridBottom = gridTop + static_cast<float>(rows) * kSystemTileHeightDip +
                     static_cast<float>(rows - 1) * kSystemTileGapDip;
    }
    layout.tileArea.bottom = gridBottom;

    // THE INSET PANEL, directly under the grid: its own rounded surface, holding the volume row and the
    // brightness row when the display answers for one.
    const float headerHeight = 18.0f;
    const float headerGap = 4.0f;
    const float trackRowHeight = 32.0f;
    const float rowGap = 8.0f;
    const float panelInset = 20.0f;
    const float rowHeight = headerHeight + headerGap + trackRowHeight;
    constexpr float kSliderPanelHeight = 132.0f;
    if (sliderCount > 0) {
        layout.sliderPanel.left = left;
        layout.sliderPanel.right = right;
        layout.sliderPanel.bottom = layout.footer.top - 12.0f;
        layout.sliderPanel.top = layout.sliderPanel.bottom - kSliderPanelHeight;

        const float rowsHeight = static_cast<float>(sliderCount) * rowHeight +
                                 static_cast<float>(sliderCount - 1) * rowGap;
        const float rowsTop =
            layout.sliderPanel.top + (kSliderPanelHeight - rowsHeight) * 0.5f;

        for (int i = 0; i < sliderCount; ++i) {
            const float rowTop = rowsTop + static_cast<float>(i) * (rowHeight + rowGap);
            const float headerTop = rowTop;
            const float headerBottom = headerTop + headerHeight;
            const float centreY = (headerTop + headerBottom) * 0.5f;

            RectF& icon = i == 0 ? layout.speakerIcon : layout.brightnessIcon;
            icon.left = left + panelInset;
            icon.right = icon.left + 20.0f;
            icon.top = centreY - 10.0f;
            icon.bottom = centreY + 10.0f;

            layout.sliderLabels[i].left = icon.right + 10.0f;
            layout.sliderLabels[i].right = right - panelInset - 70.0f;
            layout.sliderLabels[i].top = headerTop;
            layout.sliderLabels[i].bottom = headerBottom;

            layout.sliderValues[i].right = right - panelInset;
            layout.sliderValues[i].left = layout.sliderValues[i].right - 60.0f;
            layout.sliderValues[i].top = headerTop;
            layout.sliderValues[i].bottom = headerBottom;

            layout.sliders[i].left = left + panelInset;
            layout.sliders[i].right = right - panelInset;
            layout.sliders[i].top = headerBottom + headerGap;
            layout.sliders[i].bottom = layout.sliders[i].top + trackRowHeight;
            layout.sliderHitRows[i].left = layout.sliders[i].left;
            layout.sliderHitRows[i].right = layout.sliders[i].right;
            layout.sliderHitRows[i].top = layout.sliders[i].top - 2.0f;
            layout.sliderHitRows[i].bottom = layout.sliders[i].bottom + 2.0f;
        }

        // The output-device button takes the RIGHT END of the volume header, and the volume reading stops
        // short of it. The brightness row has no such button, so its reading uses the full width.
        layout.volumeButton.right = right - panelInset;
        layout.volumeButton.left = layout.volumeButton.right - 40.0f;
        layout.volumeButton.top = layout.sliderLabels[0].top + 3.0f;
        layout.volumeButton.bottom = layout.sliderLabels[0].bottom - 3.0f;
        layout.sliderValues[0].right = layout.volumeButton.left - 8.0f;
        layout.sliderValues[0].left = layout.sliderValues[0].right - 60.0f;
        layout.sliderLabels[0].right = layout.sliderValues[0].left - 10.0f;
    }

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
    // The battery/status plate at the left of the footer.
    BatterySettings,
    // The moon button: Night Light.
    NightLight,
    // The speaker button in the footer. The speaker at the left of the volume row remains the same
    // mute action; both report and read the one AudioVolume state.
    FooterMute,
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
        // The track row is 32 DIP high; the hit row extends two DIP beyond it without reaching the header
        // or the neighbouring row. Fraction still comes from the visible track.
        const RectF& track = layout.sliders[i];
        const RectF& hitRow = layout.sliderHitRows[i];
        if (x >= hitRow.left && x <= hitRow.right && y >= hitRow.top && y <= hitRow.bottom) {
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
    if (inside(layout.footerBattery)) {
        hit.target = SystemPanelTarget::BatterySettings;
        return hit;
    }
    if (inside(layout.footerButtonHits[0])) {
        hit.target = SystemPanelTarget::NightLight;
        return hit;
    }
    if (inside(layout.footerButtonHits[1])) {
        hit.target = SystemPanelTarget::FooterMute;
        return hit;
    }
    if (inside(layout.footerButtonHits[2])) {
        hit.target = SystemPanelTarget::Gear;
    }
    return hit;
}

// A wheel notch moves one tile page: positive is up, negative is down. Boundaries clamp rather than
// wrap, so scrolling past the last page cannot unexpectedly return to the first.
inline int SystemPanelPageAfterWheel(int pageIndex, int pageCount, int notches) {
    if (pageCount <= 1) return 0;
    int page = pageIndex - notches;
    if (page < 0) page = 0;
    if (page >= pageCount) page = pageCount - 1;
    return page;
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

    // Title, artist and the lyric region, top to bottom in the right-hand column. The cover is 108 tall
    // and the column beside it has about 100 DIP: the title (22) and the artist (13) take the top, and
    // the current lyric and the next one take the rest. The lyrics used to be an empty area -- "这个区域
    // 显示歌词" is what fills it.
    layout.title.left = layout.artwork.right + kMusicGapAfterArtDip;
    layout.title.right = box.rect.right - pad;
    layout.title.top = layout.artwork.top;
    layout.title.bottom = layout.title.top + 32.0f;

    layout.subtitle.left = layout.title.left;
    layout.subtitle.right = layout.title.right;
    layout.subtitle.top = layout.title.bottom;
    layout.subtitle.bottom = layout.subtitle.top + 20.0f;

    layout.lyricCurrent.left = layout.title.left;
    layout.lyricCurrent.right = layout.title.right;
    layout.lyricCurrent.top = layout.subtitle.bottom + 6.0f;
    layout.lyricCurrent.bottom = layout.lyricCurrent.top + 28.0f;

    layout.lyricNext.left = layout.title.left;
    layout.lyricNext.right = layout.title.right;
    layout.lyricNext.top = layout.lyricCurrent.bottom + 2.0f;
    layout.lyricNext.bottom = layout.lyricNext.top + 22.0f;

    // The thin level bar at the card's right edge, spanning the cover's height. A single bar rather than
    // the pill's ribbon: the card has no room for the wave, and the number it would show is this one.
    // The text stops short of it, so no word ever runs under it.
    layout.levelBar.right = box.rect.right - pad;
    layout.levelBar.left = layout.levelBar.right - 6.0f;
    layout.levelBar.top = layout.artwork.top;
    layout.levelBar.bottom = layout.artwork.bottom;

    // A second, thinner vertical bar beside it: the reading position in the SHEET. Two thin bars read as
    // one instrument panel at the card's right edge, and the text now stops before both of them.
    layout.lyricProgress.right = layout.levelBar.left - 8.0f;
    layout.lyricProgress.left = layout.lyricProgress.right - 3.0f;
    layout.lyricProgress.top = layout.levelBar.top;
    layout.lyricProgress.bottom = layout.levelBar.bottom;
    layout.title.right = layout.lyricProgress.left - 10.0f;
    layout.subtitle.right = layout.title.right;
    layout.lyricCurrent.right = layout.title.right;
    layout.lyricNext.right = layout.title.right;

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
