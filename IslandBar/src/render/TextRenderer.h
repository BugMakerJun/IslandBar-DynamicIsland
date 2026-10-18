// =====================================================================
//  TextRenderer.h
//  DirectWrite text for the island.
//
//  Deliberately small: it formats a time and a date and draws them into
//  whatever D2D context it is handed. It does not own the context or the target
//  bitmap, so the same object serves the pill and the expanded card.
//
//  Formatting is cached. GetLocalTime costs microseconds, but the formats and
//  the date string are rebuilt only when they actually change -- the time
//  changes once a second, the date once a day.
// =====================================================================
#pragma once

#include <windows.h>

#include <d2d1_3.h>
#include <dwrite_3.h>

#include <cstddef>
#include <unordered_map>

#include "domain/Geometry.h"      // RoundedBox: the text is laid out inside the pill's box
#include "media/MusicSession.h"  // the now-playing field capacities
#include "domain/CardLayout.h"    // where the card's controls go
#include "domain/TaskStripLayout.h"  // where the tray sits, and how many icons fit
#include "shell/TrayIcons.h"     // the icons themselves
#include "shell/StartApps.h"     // the Start card's applications

namespace island {

enum class ClockStyle {
    Compact,   // hh:mm for the pill
    Card,      // hh:mm plus the date, for the expanded card
};

// One window button on the taskbar.
//
// At namespace scope rather than nested inside TextRenderer, so the frame state can hold an array
// of them without a circular-looking dependency and the renderer can be handed one plain block.
struct TaskButton {
    const wchar_t* title = nullptr;   // already UTF-16; the window's own text
    ID2D1Bitmap1* icon = nullptr;     // the owning process's icon, or null
    bool active = false;              // this is the window in front
    bool minimised = false;

    // The button's background COLOUR, taken from the island's own fill.
    //
    // The COLOUR IS PASSED IN rather than chosen here, and that is the point: it is the same value
    // the island's pill is filled with. A button that picked its own shade would drift away from the
    // island the first time either changed, and the two read as one object.
    //
    // The RECTANGLE is not passed in. It is derived inside the draw from the same layout call that
    // positions the button, so the background and the button cannot disagree -- passing it in from
    // the caller meant computing the layout twice, and the two would part company the moment one of
    // them used a different hovered index.
    //
    // Before this existed the icons sat straight on the desktop. An icon on a bright photograph is
    // hard to read and a title on one is worse, which is exactly what was reported.
    D2D1_COLOR_F fill = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);

    // How far this button's opening has run, 0 to 1. Animated by the caller so the button OPENS
    // instead of snapping -- a width that jumps in one frame reads as a stutter, which is what was
    // reported.
    //
    // One value decides the button's width AND how far the buttons after it are pushed. A second
    // animated value for the push used to exist, disagreed with this one mid-transition, and let the
    // open button paint over its neighbour.
    float hoverAmount = 0.0f;
};


class TextRenderer {
public:
    // The call timer, as mm:ss (or h:mm:ss once it runs past an hour).
    //
    // A free function rather than something the drawing code does inline, because it is the part that is
    // WRONG in a way nobody notices for ten minutes: 61 seconds printed as "1:1" or 3661 as "61:01" both look
    // like a timer until you check. Asserted in the self-test at the boundaries.
    static void CallTimerText(int seconds, wchar_t* out, size_t capacity);

    // How tall bar `index` of a `count`-bar meter is, for a level of 0..1, as a fraction of the tallest bar.
    //
    // The bars are a fixed pattern scaled by the level, not a per-bar random number: a level meter whose bars
    // jump independently reads as noise, while one that keeps its shape and breathes reads as sound. Pure so
    // the shape can be asserted (it must never be flat, and it must never exceed 1).
    static float CallBarFraction(int index, int count, float level);
    TextRenderer() = default;
    ~TextRenderer();

    TextRenderer(const TextRenderer&) = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;

    // Creates the DirectWrite factory and the text formats. Safe to call twice;
    // the second call is a no-op.
    bool Initialize();

    void Shutdown();

    // Empties the brush cache. Called by Shutdown (the brushes belong to the device context) and by the
    // theme switch, which changes every colour the cache is keyed by -- see the note on BrushFor.
    void ClearBrushCache();

    bool IsReady() const { return factory_ != nullptr; }

    // Draws the current local time centred in `box`, using window-local DIPs.
    //
    // `opacity` scales the text colour so it can follow the pill's own fade
    // instead of popping in at full strength.
    void DrawClock(ID2D1DeviceContext2* context, const RoundedBox& box,
                   ClockStyle style, float opacity);

    // THE SYSTEM TRAY: one button at the right-hand end of the bar, and a card behind it.
    //
    // Asked for in these words: "在红框位置创建一个按钮 点击按钮弹出卡片 卡片里是这些托盘" -- a button and a
    // card rather than the row of icons the first version drew along the bar.
    //
    // The button is deliberately NOT a chevron: the deck's own system button already carries one, and two
    // identical marks at the two ends of the bar would read as the same control. Three dots say "there is
    // more than fits", which is what this is.
    // `wash` as for the Start button; `open` still forces a full one, because the card being open is a
    // state rather than something the pointer is doing.
    void DrawTrayButton(ID2D1DeviceContext2* context, const RectF& box, float wash, bool open,
                        const D2D1_COLOR_F& fill, float opacity);

    // --- the Start card -------------------------------------------------------------------------------
    //
    // Everything that card shows, assembled by the frame loop: the user's own pins with their icons, the
    // shell's full list for the picker, and the two things in its footer. Pointers rather than copies,
    // because the shell's list is 121 applications and a frame is rebuilt every frame -- the same
    // arrangement the tray page uses.
    struct StartCardText {
        const StartApp* pins = nullptr;
        int pinCount = 0;
        // One PNG per pin, resolved when the pin was added. A null entry is an icon that has not arrived
        // yet, which happens for a few frames and must not stop the tile being drawn.
        const uint8_t* const* pinIcons = nullptr;
        const size_t* pinIconSizes = nullptr;

        const StartApp* list = nullptr;
        int listCount = 0;
        int pickerScrollRow = 0;
        // One PNG per entry of `list`, for the all-applications grid (the text picker ignores them).
        const uint8_t* const* listIcons = nullptr;
        const size_t* listIconSizes = nullptr;

        // 0 = the pins, 1 = the picker, 2 = the power list. A page rather than three flags, because only
        // one of them can be showing and the hit tests have to ask the same question the drawing did.
        int page = 0;
        int hoveredPin = -1;
        int hoveredPickerRow = -1;
        int hoveredPowerRow = -1;
        bool powerButtonHovered = false;
        // The 全部 button's highlight, and whether the picker was opened by it (its rows then launch
        // instead of pinning -- the footer says which).
        bool allAppsHovered = false;
        bool allAppsMode = false;
        // How far each item's highlight has come up, 0 to 1, one entry per item in the same order as
        // `pins`, `list` and the power rows. Spring-animated by the frame loop, for the reason the deck's
        // washes are (see ControlWashStep): a highlight that switches on in one frame reads as a hard edge,
        // and moving the pointer across a grid of them then reads as flicker rather than as movement.
        //
        // Per ITEM rather than "the one being pointed at", because the highlight that is LEAVING has to
        // fade out while the next one fades in -- with a single amount, the tile just left would blink off.
        const float* pinWashes = nullptr;
        const float* rowWashes = nullptr;
        const float* powerWashes = nullptr;
        // The power row waiting for a second press (see the click handler): the three that lose unsaved
        // work do not act on the first click, and this is which one is armed.
        int powerConfirmRow = -1;

        const wchar_t* accountName = nullptr;
        const uint8_t* avatarPng = nullptr;
        size_t avatarPngSize = 0;
    };

    void DrawStartPage(ID2D1DeviceContext2* context, const RoundedBox& card, const StartCardText& text,
                       float opacity);

    // The card's tray page: the icons in a wrapping grid, each with the wash the other buttons get.
    //
    // `revision` is the list's own, so a decoded bitmap is kept until the icons themselves change: the bar
    // draws at the compositor's rate and a PNG decode per frame per icon would be the most expensive thing
    // on it. `hoveredIndex` is a cell of the grid (see TrayPageFor).
    void DrawTrayPage(ID2D1DeviceContext2* context, const RoundedBox& card, const wchar_t* title,
                      const TrayIcon* icons, int count, unsigned long long revision, float opacity,
                      int hoveredIndex, bool moving = false);

    // The tray's own context menu: a small raised surface with one row per entry, the row under the
    // pointer washed white. Geometry comes from `TrayMenuFor` (CardLayout.h) -- the box and the rows are
    // handed in, so the drawing and the hit test cannot disagree about which row a point is on.
    void DrawTrayMenu(ID2D1DeviceContext2* context, const island::RectF* rows, int count,
                      const wchar_t* const* labels, int hoveredRow, float opacity);

    // --- the date, the weekday, the time and the weather: one line, two homes ----
    //
    // Inside the pill while the island is idle, and in the band left of the deck while the pill's
    // own line is taken (the lyric, a message, a call). See StatusPlacementFor for the rule and
    // StatusStripFor for where the band is.
    //
    // THE LOOK IS THE REFERENCE'S, and it is not a sentence: it is a run of pieces read left to
    // right, each set in its own weight and colour, with hairlines between the groups and a drawn
    // glyph in it --
    //
    //     weekday day | time | [ cloud ] sky temperature
    //
    // asked for with a picture of exactly that ("星期 日期 时间 天气改成图片这样显示"). Two rounds of
    // the user's eye shaped it: the calendar glyph that stood at the head of the run was removed on
    // request ("去掉前面的日历图标"), and the run is now the SAME in both homes, glyph included
    // ("胶囊里也带图标") -- which the removal is what made possible: 235.6 DIP with the calendar did
    // not fit the 220 DIP pill, about 200 without it does.
    //
    // It fits, and that is measured rather than assumed: MeasureStatusLine is called by the
    // self-test for the widest run that can ever occur, against both boxes.

    // The width the whole run is drawn at, in the formats it is drawn with. Needs Initialize().
    //
    // A function of the STRINGS rather than of the clock, so the self-test can ask about the widest
    // run that can ever occur without waiting for that day to come round.
    float MeasureStatusLine(const wchar_t* weekday, const wchar_t* day, const wchar_t* time,
                            const wchar_t* sky, const wchar_t* temperature,
                            int weatherCode) const;

    // Draws the run centred in `box`, on whatever `fill` says it is standing on.
    //
    // ONE function for both homes, because it is one line and the two places must not be able to
    // disagree about how it is set. What differs between them is only what it is drawn ON, and
    // that is what `fill` says: the band passes the island's colour, so its capsule stands on the
    // wallpaper, and the island's own copy passes a transparent fill, because the pill underneath
    // has already been filled with exactly that colour. (The colour is passed in rather than
    // chosen here for the reason TaskButton gives: a surface that picks its own shade drifts away
    // from the bar it belongs to.)
    //
    // The weekday, the day and the time come from this object's own clock; `sky` and `temperature`
    // are the reading, and either may be null (no reading yet, or no city and no position).
    void DrawStatusLine(ID2D1DeviceContext2* context, const RoundedBox& box, float opacity,
                        const wchar_t* sky, const wchar_t* temperature,
                        int weatherCode, const D2D1_COLOR_F& fill);

    // The date as its two pieces, because the run sets them apart with a gap and a weight change:
    // "周一" and "10月12日". Filled by Refresh; empty before the first call.
    void CurrentDateParts(wchar_t* weekday, size_t weekdayCapacity, wchar_t* day,
                          size_t dayCapacity) const;

    // Draws a notification's sender, title and body in the lower part of `box`.
    //
    // Empty fields are skipped rather than drawn blank, so a notification with only
    // a title does not leave a hole where the body would be.
    void DrawNotification(ID2D1DeviceContext2* context, const RoundedBox& box,
                          const char* app, const char* title, const char* body,
                          const uint8_t* iconPixels, int iconWidth, int iconHeight,
                          unsigned long long iconRevision, float opacity);

    // Everything the music card says, in one struct so the call site cannot pass the
    // fields in the wrong order.
    struct MusicCardText {
        const char* title = nullptr;
        const char* artist = nullptr;
        const char* album = nullptr;
        const char* app = nullptr;
        bool  playing = false;
        long long positionMs = 0;
        long long durationMs = 0;
        // 0..1, or <= 0 when the duration is unknown (a live stream), in which case the
        // bar is drawn but never filled.
        float progressFraction = 0.0f;
        // Average luminance of the blurred album background, 0..1; 0 means "no background". The card's
        // words switch between black and white on this, because the album -- not the island -- is what
        // is behind them.
        float backgroundLuminance = 0.0f;
        // The accent taken from the cover (island::AccentColourFromArtwork), so the progress bar and the
        // play button follow the album rather than a fixed blue. A zero alpha means "use the default".
        D2D1_COLOR_F accent = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);
        // THE CARD'S LYRICS, in the space beside the cover ("这个区域显示歌词"): the current line, the one
        // after it, and how far through the current line the song is (0..1) for the karaoke fill. All
        // three come from the same sheet the pill's line is read from.
        const char* lyricLine = nullptr;
        const char* lyricNext = nullptr;
        float lyricProgress = 0.0f;
        // The line the current one replaced, and how far its arrival has rolled (0..1). The card draws the
        // two as a cross-fade, the way the pill's line rolls -- without it the words switch in one frame.
        const char* lyricPrev = nullptr;
        float lyricRoll = 1.0f;
        // The output level for the card's thin level bar, 0..1. `levelKnown` false means the reader has
        // no peak yet, and the card draws only the bar's track.
        float level = 0.0f;
        bool levelKnown = false;
        // The whole-sheet reading position for the thin vertical lyric progress bar: the current line's
        // index and the sheet's line count. Count 0 draws only the track.
        int lyricIndex = -1;
        int lyricCount = 0;
    };

    // Draws the now-playing card, laid out like the Windows 11 media flyout: blurred
    // album art as the background, the app's name and state along the top, the cover,
    // title and artist, the elapsed and total times with a progress bar between them,
    // and previous / play-pause / next along the bottom.
    //
    // `backgroundPixels` is the blurred artwork (see WicImage's
    // MakeBlurredBackgroundPng); passing null simply omits the wash.
    // `artwork` and `background` are GPU bitmaps the CALLER owns and caches; passing raw
    // pixels here instead would mean this function rebuilt them on every frame, which
    // copies the pixels to the GPU each time -- see Renderer::CachedBitmap.
    void DrawMusicCard(ID2D1DeviceContext2* context, const RoundedBox& box,
                       const MusicCardText& text, ID2D1Bitmap1* artwork, int artworkWidth,
                       int artworkHeight, ID2D1Bitmap1* background, ID2D1Bitmap1* icon,
                       float opacity);

    // Draws a cached bitmap into a rectangle. Shared by the card's background and its
    // album cover so both go through one draw path.
    // The filter is a parameter because the two users differ: the card's background is a
    // smooth blur that linear resampling cannot visibly change, while the album cover is
    // small and detailed and is worth the cubic filter.
    void DrawBitmapRect(ID2D1DeviceContext2* context, ID2D1Bitmap1* bitmap,
                        const RectF& target, float opacity,
                        D2D1_INTERPOLATION_MODE mode = D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    // Raw-pixel variant, used once per assertion by the diagnostic artwork drawing.
    void DrawBitmapRect(ID2D1DeviceContext2* context, const uint8_t* pixels, int width,
                        int height, const RectF& target, float opacity);

    // How long the last music-card draw spent on each expensive layer, in microseconds.
    //
    // Reported because "the card is laggy" has to be attributed to a layer before it can
    // be fixed: the first guess was the per-frame bitmap upload, and measuring showed that
    // was worth nothing, so the cost is one of these blits.
    double LastCardBackgroundMicroseconds() const { return lastCardBackgroundUs_; }
    double LastCardArtworkMicroseconds() const { return lastCardArtworkUs_; }
    double LastCardMicroseconds() const { return lastCardUs_; }

    // Draws decoded album art into the layout's artwork square. `pixels` is BGRA
    // premultiplied, as WicImage produces.
    //
    // The bitmap is created from the pixel data on every call and released again;
    // artwork changes rarely and the image is at most 128x128, so caching it would
    // save very little while adding a lifetime to get wrong.
    void DrawArtwork(ID2D1DeviceContext2* context, const RoundedBox& box,
                     const uint8_t* pixels, int width, int height);

    // Draws one line of lyrics in the resting pill, marquee-style.
    //
    // `scrollDip` shifts the text left as it plays, so a line longer than the pill
    // scrolls instead of being cut off. The text is clipped to the pill's rounded
    // shape, so it can never appear outside the island.
    // How one line replaces another: the roll. `progress` runs 0 -> 1 (see LyricLineRollFor) and `previous`
    // is the line that is leaving, drawn above the arriving one by one line height. For the length of the
    // roll the pill therefore shows two half rows -- one climbing out, one rising in -- and that is the
    // shape the user asked for, twice.
    struct LineRoll {
        const char* previous = nullptr;
        float       progress = 1.0f;
        float       previousScrollDip = 0.0f;
    };

    //
    // `roll` is how the line arrived: at progress 1 it is settled and nothing is leaving, which is
    // what every caller that is not changing lines passes (the default).
    void DrawLyricLine(ID2D1DeviceContext2* context, const RoundedBox& box,
                       const char* utf8Line, float scrollDip, float opacity,
                       const LineRoll& roll = {}, const D2D1_COLOR_F* over = nullptr);

    // Draws one line of the track's title in the resting pill. Used when there are
    // no lyrics to show. `over` is the colour the words are painted ON -- the music wave -- and the line
    // then takes its contrast rather than the card's ink, which is what makes a lyric readable over a
    // bright accent ("歌词 要根据后面的音波图 变反差色 要不看不清").
    void DrawTrackLine(ID2D1DeviceContext2* context, const RoundedBox& box,
                       const char* utf8Line, float scrollDip, float opacity,
                       const LineRoll& roll = {}, const D2D1_COLOR_F* over = nullptr);

    // The pill's BACKGROUND audio wave.
    //
    // A full-width, full-height wave rather than a row of vertical bars: "电平条有没有别的什么方式 现在着用
    // 竖条看着有点丑 然后能不能占胶囊全部高度 宽度也一样 颜色跟随专辑获取歌曲变". The ribbon's
    // half-height at each x is a smooth envelope times the peak reading (AudioVolume::Peak, sampled by the
    // worker about thirty times a second), so it fills the pill when the music is loud and thins to a line
    // when it is quiet; `phase` drifts with time so the shape moves even while the level sits still.
    // `accent` is the colour taken from the cover (see AccentColourFromArtwork).
    void DrawAudioWave(ID2D1DeviceContext2* context, const RoundedBox& box, float phase, float level,
                       const D2D1_COLOR_F& accent, float opacity);

    // The wave's half-height at `step` of `steps`, 0..1, before the level scales it.
    //
    // A pure function for the same reason CallBarFraction is one: "the wave does not move" and "the wave
    // is not drawn" look identical on screen, and this separates the two.
    static float AudioWaveEnvelope(int step, int steps, float phase);

    // One window button on the taskbar.
    //
    // The struct itself is at namespace scope (see above) so the frame state can hold an array of
    // them; this alias keeps the name available inside the class where it reads better.
    using TaskButton = island::TaskButton;

    // Draws the row of application icons beside the island.
    //
    // `hovered` widens each button so it can show its window's title; at rest they are plain
    // squares. The island's own box is passed because the row is positioned FROM it -- the island is
    // the fixed point and the row grows to its right.
    //
    // `buttons` is read up to `count`, and the count is trusted rather than silently adjusted: a bar
    // that draws a different number of buttons than the click handler hit-tests is a bar whose
    // clicks land on the wrong window.
    // `rowAmounts` and `rowCount` describe the row, and are the same count as the buttons.
    //
    // `cardLeftDip` and `cardWidthDip` locate the system panel's card when one is open, so the row can
    // start past it instead of underneath it: the card hangs from the system button and spans rightwards
    // over where the row would otherwise begin. Zero means "no card", which is every other state.
    void DrawTaskIcons(ID2D1DeviceContext2* context, const RoundedBox& island,
                       const TaskButton* buttons, int count, int hoveredIndex, float opacity,
                       const float* rowAmounts = nullptr, int rowCount = 0,
                       float cardLeftDip = 0.0f, float cardWidthDip = 0.0f,
                       // Each icon's arrival, 0..1 (see TaskIconAppearStep); null means all settled.
                       const float* appearAmounts = nullptr);

    // The Start button, on the left of the island.
    //
    // Drawn from the SAME island box the icon row is drawn from, so the two ends of the bar cannot
    // disagree about where the middle is. `hovered` is whether the pointer is on it, and `fill` is the
    // island's own colour, for the reason the icon buttons take it: a button that chose its own shade
    // would drift away from the island the first time either changed.
    // `wash` is how far the pointer's wash has come up, 0 to 1, spring-animated by the frame loop -- a
    // wash that switches on in one frame reads as hard rather than as responsive. See FrameState::startWash.
    //
    // `open` is the other thing a control has to be able to say: that it is the one whose card is up
    // (FrameState::startButtonOpen). It is a separate flag from the wash because being pressed and being
    // the reason a card is on screen are different states -- and because the bar spent a version with
    // every card lighting up the SYSTEM button instead.
    void DrawStartButton(ID2D1DeviceContext2* context, const RoundedBox& island, float wash, bool open,
                         const D2D1_COLOR_F& fill, float opacity);

    // Which system indicator a row slot — or a panel tile — holds.
    //
    // ONE enum for both, because they show the same things: the tile that explains Wi-Fi must draw the
    // same mark as the indicator that summarises it, and two enums would let them drift apart.
    enum class IndicatorIcon : int {
        InputMethod = 0,
        Network = 1,
        Bluetooth = 2,
        Volume = 3,
        Battery = 4,
        Airplane = 5,
        Settings = 6,
        Accessibility = 7,
        BatterySaver = 8,
        Captions = 9,
        // The second page's tiles. Codes verified by drawing them, like the rest of this table -- a glyph that
        // does not exist comes out as a box, which is why they are looked at rather than trusted.
        Moon = 10,
        Hotspot = 11,
        Share = 12,
        // The output-device button at the right of the volume row, and the icon of a sound device in the
        // "声音输出" page. It is the speaker glyph with the arrow that says "this is where the sound goes",
        // which is the mark the shell's own flyout puts at the end of the volume row.
        OutputDevice = 13,
        // The two tiles on the shell's second page that open a LIST of display modes, and the two device
        // marks the "声音输出" page needs for a headset and for audio that comes out of a screen.
        WiredDisplay = 14,
        Projection = 15,
        Headphones = 16,
        DisplayAudio = 17,
        // The brightness row's own glyph, at the left of the second slider -- the shell's quick-settings
        // flyout puts a sun there. Verified by drawing it, like the rest of this table.
        Brightness = 18,
    };

    using SystemPanelIcon = IndicatorIcon;

    // The Segoe Fluent Icons codepoint for a mark, at a given reading.
    //
    // A function rather than a switch buried in the drawing code, because the codepoints are the part
    // that went wrong: U+E0CE looked like the Bluetooth codepoint and drew an empty box, and A MISSING
    // GLYPH DOES NOT REPORT AN ERROR. Keeping the mapping in one place, with the values written down,
    // means the next one can be checked against this table instead of guessed at again.
    //
    // `value` is the reading the mark depends on -- signal strength for the network, 0-100 for the
    // battery, 0 for muted on the volume -- and is ignored by the marks that do not vary.
    static wchar_t IndicatorGlyph(IndicatorIcon icon, int value);

    // DrawIndicator used to live here: one system indicator, at a rectangle the caller had laid out.
    //
    // It is gone with the indicator row itself. The glyph table above stays, because the system panel's
    // TILES are drawn from it -- a tile that says "Bluetooth" and an indicator that said the same thing
    // had to show the same mark, and that is still true of the tiles.

    // The system panel's button: a chevron on the left deck, beside the Start button.
    //
    // `open` points it up when the panel is showing, which is the one piece of state the button has to
    // communicate by itself: whether pressing it again will put away what is already there.
    void DrawSystemButton(ID2D1DeviceContext2* context, const RectF& box, float wash, bool open,
                          const D2D1_COLOR_F& fill, float opacity);

    // What pressing a panel tile does.
    //
    // An enum rather than a callback, so a tile is plain data: the frame is built in one place and
    // consumed in another, and the click handler has to be able to ask "what does this tile want"
    // without the tile carrying a function pointer into a lifetime it does not own.
    enum class PanelAction : int {
        None = 0,        // nothing yet -- the tile reports state and does not change it
        OpenSettings,    // launch the `uri` page
        ToggleMute,      // the speaker tile's own action
        // Open the "声音输出" page inside the card -- the shell's own second level off the volume row --
        // rather than launching a settings page outside it.
        OpenSoundPage,
        // 投影: the shell's own tile sends Win+P, which is what brings up the projection-mode list. The
        // page it opens is the shell's, not a settings URI, so it cannot be expressed as OpenSettings.
        ProjectionModes,
        // A tile that switches something here and now, rather than taking the user to a page. 飞行模式 is
        // the one that switches BOTH radios, through island::RequestRadios -- the documented WLAN write that
        // used to serve it reported success and changed nothing on this machine (see SetWifiRadioOn).
        ToggleRadio,
        // Bluetooth, through Windows.Devices.Radios -- which, measured, allows an ordinary unpackaged
        // process: see island::RequestRadios. Runs on a worker, so the press returns at once.
        ToggleBluetooth,
        // Wi-Fi, through the same API and the same worker. It used to open a settings page, which is what
        // the tile had the arrow for; a radio switch is what every tile beside it does now, and the shell's
        // own tile switches in place as well.
        ToggleWifi,
        // 夜间模式, by pressing the Settings page's own switch (island::RequestNightLight): there is no API,
        // and measured -- writing the state the OS keeps in the registry does not switch it.
        ToggleNightLight,
        // 实时字幕, through the shortcut the feature itself is driven by: Win+Ctrl+L (island::ToggleLiveCaptions).
        ToggleLiveCaptions,
        // 外观: walks the theme list (Theme.h) and re-colours the whole bar on the spot. A CYCLE rather
        // than a list page, for the panel's own reason: every other tile applies its change here and
        // says what it is now, and the caption under this one names the theme that is in force.
        CycleTheme,
    };

    // One tile of the system panel, as the caller has decided it.
    //
    // The caller supplies the finished words rather than the readings, because turning a reading into
    // a label ("Wi-Fi", "Airplane mode") and deciding whether it is ON is a judgement about the
    // machine, and that belongs with the code that has the reading.
    struct SystemPanelTile {
        const wchar_t* label = nullptr;   // "Wi-Fi", "Bluetooth", ...
        // What the CAPTION under the box says, when that is not the name: the state ("开"/"关"), which is what
        // the shell's own flyout puts there (its Wi-Fi tile is captioned 可用 rather than "Wi-Fi" -- the glyph
        // above it already says which control it is). Null means "caption the label".
        const wchar_t* stateText = nullptr;
        SystemPanelIcon icon = SystemPanelIcon::Network;
        int iconValue = 0;                // the icon's own reading, e.g. signal strength
        bool on = false;                  // the tile's ON appearance
        // Whether the tile shows a chevron, i.e. whether it leads somewhere rather than toggling in place.
        //
        // The shell's own flyout draws this arrow on the tiles that open a LIST -- Wi-Fi and Bluetooth open
        // their flyouts -- and not on the plain ones (飞行模式, 辅助功能, 节能模式, 实时字幕 have none). The
        // first version of this drew the arrow on every tile that had any action, which is not the same
        // question and does not look like the flyout it is meant to sit beside.
        bool chevron = false;

        // What pressing it does.
        //
        // Carried on the tile rather than looked up from the label, because the label is user-facing
        // text and the action is not: renaming a tile must not silently change what it does, and a
        // lookup by label would also tie the two together in the one direction that cannot be tested.
        PanelAction action = PanelAction::None;
        const wchar_t* uri = nullptr;     // the settings page, for PanelAction::OpenSettings
    };

    struct SystemPanelText {
        SystemPanelTile tiles[kMaxSystemTiles] = {};
        int tileCount = 0;
        // Which page of tiles this is, and how many there are. The tiles above are always the CURRENT page's
        // (the caller assembles the slice), so the layout only needs these two for the paging dots.
        int pageIndex = 0;
        int pageCount = 1;
        float volume = 0.0f;      // 0..1, for the slider
        bool muted = false;
        // Whether to print the volume as a percentage beside the slider. The shell shows the number while
        // the slider is being used and hides it again, and the number is the only exact reading on the
        // panel -- a handle at 40% of its travel says nothing about whether that is 40 or 45.
        bool showVolumePercent = false;

        // The brightness row, under the volume row, and only when the display answers for it.
        //
        // DDC/CI is what makes it possible at all, and it is not universal: a monitor with DDC/CI switched
        // off in its own menu, or one that does not support it, reports nothing -- and then this row is not
        // drawn rather than drawn at a made-up level. `hasBrightness` is that answer, decided by the caller
        // from an actual read (DisplayBrightness.h).
        bool hasBrightness = false;
        float brightness = 0.0f;        // 0..1, the same scale as the volume
        bool showBrightnessPercent = false;

        // WHETHER A SLIDER IS THE CONTROL IN HAND, per row (0 the volume, 1 the brightness): hovered or
        // being dragged. The handle grows a little when it is, which is what every slider of this kind
        // does -- it says which row the pointer is on without moving anything, and it gives the finger
        // more to hold during a drag.
        float sliderEmphasis[2] = {0.0f, 0.0f};
    };

    // The system panel, drawn into the card.
    //
    // One call for the whole panel, like the music card: the tile washes, glyphs, labels, the slider
    // and the settings button are drawn in one place so their layers cannot end up in the wrong order.
    void DrawSystemPanel(ID2D1DeviceContext2* context, const RoundedBox& box,
                         const SystemPanelText& text, float opacity);

    // What the "声音输出" page says: the device list, the spatial-sound rows and the mixer.
    //
    // The strings and the readings are supplied by the caller -- they come from the audio stack, which this
    // class knows nothing about -- and the geometry comes from SoundPageFor, which the hit test also calls.
    struct SoundPageText {
        // Rows the mixer can hold, including the output device's own row at the top: the shell's mixer lists
        // the device first and the applications under it.
        static constexpr int kMaxMixRows = kMaxSoundSessions + 1;

        // The page's own words. Chinese, because this is the shell's own page and it is what the reference
        // flyout says; the caller supplies them so this class holds no user-facing strings of its own.
        const wchar_t* title = nullptr;         // 声音输出
        const wchar_t* deviceLabel = nullptr;   // 输出设备
        const wchar_t* spatialLabel = nullptr;  // 空间音效
        const wchar_t* mixerLabel = nullptr;    // 音量合成器
        const wchar_t* footer = nullptr;        // 更多音量设置

        struct DeviceRow {
            const wchar_t* name = nullptr;
            int formFactor = 0;      // EndpointFormFactor; the glyph is chosen from it
            bool isDefault = false;
        };
        DeviceRow devices[kMaxSoundDevices] = {};
        int deviceCount = 0;

        struct SpatialRow {
            const wchar_t* label = nullptr;
            bool selected = false;
        };
        SpatialRow spatial[kSoundSpatialRows] = {};

        struct MixRow {
            const wchar_t* label = nullptr;     // for the log, and for a row with no icon
            float volume = 1.0f;
            bool muted = false;
            // The application's icon exactly as the sound worker encoded it: a small PNG, decoded once per
            // revision and kept as a bitmap here. Null falls back to a speaker glyph.
            const unsigned char* iconPng = nullptr;
            size_t iconSize = 0;
            unsigned long long iconRevision = 0;
        };
        MixRow mix[kMaxMixRows] = {};
        int mixCount = 0;

        float scroll = 0.0f;
    };

    // The 声音输出 page, drawn into the card. One call, like the panel, so its layers cannot be drawn out of
    // order -- and clipped to its own viewport, so a scrolled row cannot paint over the header.
    void DrawSoundPage(ID2D1DeviceContext2* context, const RoundedBox& box,
                       const SoundPageText& text, float opacity);

    // The CALL strip: the island, widened, showing a call in progress.
    //
    // The reference the user handed over is the iPhone's Dynamic Island during a call -- a round avatar, a
    // green running timer, and a row of level bars -- and every part of it here is a real reading rather than
    // a decoration: the timer is the call's elapsed time, and the bars are the OUTPUT's peak level, so they
    // move when somebody speaks and sit still when nobody does.
    struct CallStripText {
        int seconds = 0;                 // elapsed, for the green timer
        float level = 0.0f;              // 0..1 peak, for the bars
        bool  levelKnown = false;        // false draws an idle meter rather than one stuck at zero
        // The caller's picture is not readable from outside WeChat, so the strip shows the APPLICATION's icon
        // in that place -- the honest stand-in, and the same icon the notification rows use.
        const uint8_t* iconPixels = nullptr;
        int iconWidth = 0;
        int iconHeight = 0;
        unsigned long long iconRevision = 0;
    };
    void DrawCallStrip(ID2D1DeviceContext2* context, const RoundedBox& box,
                       const CallStripText& text, float opacity, bool showTimer = true);

    // Height, in DIPs, that a taskbar button's title needs at the current face size. Reported so
    // the layout can be checked against the strip it has to fit in.
    float TaskTitleLineHeight() const;

    // Width of a text run in DIPs, for deciding whether it needs to scroll. Returns
    // 0 when it cannot be measured.
    float TextWidth(const wchar_t* text, bool titleStyle) const;

    // Width of one line of the PILL's own text, measured with the format that DRAWS it.
    //
    // `TextWidth` measures with appFormat_ (11 DIP, NORMAL), which is right for most callers but not for the
    // pill: the strip's line is drawn with pillLineFormat_ (11 DIP, BOLD), and a bold advance is wider for
    // Latin text. Sizing the marquee's rectangle from the narrower number is what let a long English line be
    // cut instead of scrolled -- and, with word wrap still on, what let it wrap into two rows. Both the fit
    // decision and the drawn rectangle go through here now, so the two cannot disagree.
    float TrackLineWidth(const wchar_t* text) const;

    // Reports the line height each format actually needs, in DIPs.
    //
    // The card lays text out in hand-sized rectangles, and a rectangle shorter than
    // the font's natural line height clips the glyphs. That is exactly what happened:
    // the title drew as a horizontal sliver because its 20 DIP rect was shorter than
    // the face's line height. A CJK face's line height is well above its nominal
    // point size, so the rectangle has to be sized from the real metrics.
    struct TextMetrics {
        float titleLineHeight = 0.0f;
        float secondaryLineHeight = 0.0f;
        // The pill's line (the lyric / track line), which is drawn in its own heavier format.
        float pillLineHeight = 0.0f;
    };
    TextMetrics Metrics() const;

    // The second the clock currently shows. The frame loop uses this to decide
    // whether a redraw is needed while the island is sitting still: everything
    // else about a resting pill is constant.
    void CurrentSecond(int* second);

    // The text this would currently draw, for assertions and window titles.
    // Writes at most `capacity` wide characters.
    void CurrentTimeText(wchar_t* out, size_t capacity) const;

private:
    // Refreshes the cached formatted strings if the second or the day rolled
    // over. Caching by day-key rather than by value avoids both a strcmp per
    // frame and a stale date at midnight.
    void Refresh();

    IDWriteFactory3*     factory_     = nullptr;
    IDWriteTextFormat*   timeFormat_  = nullptr;

    // The last title-box width written to the log, so the trace reports changes rather than frames.
    // Owned by the text renderer because that is what computes and draws the box.
    float lastTitleWidth_ = -1.0f;

    // What the last icon draw was asked for, so a change is reported once.
    int   lastDrawnCount_ = -1;
    int   lastIslandLeft_ = -9999;
    // The smallest arrival reported so far, so an icon on its way in logs one line per step rather than
    // one per frame while it is settling.
    float lastLeastAppear_ = 1.0f;
    bool  startTraced_ = false;
    int   startTraceLeft_ = -9999;
    float startTraceAlpha_ = -1.0f;
    float startTraceOpacity_ = -1.0f;

    // The mixer's application icons, one per row, kept until the sound worker's revision for that row
    // changes. Decoding a PNG on every frame would be the most expensive thing on the page by a wide
    // margin, and the icon only changes when a different application takes the row.
    struct SoundIconSlot {
        ID2D1Bitmap1* bitmap = nullptr;
        unsigned long long revision = 0;
    };
    SoundIconSlot soundIcons_[SoundPageText::kMaxMixRows];

    // The tray icons, one decoded bitmap each, kept until the list's revision changes.
    //
    // The REVISION is what makes the index a safe key: the list bumps it when its contents change, and
    // its contents include which icon sits at which index (an application starting shifts the order), so
    // a bitmap cached under index 3 cannot survive into a list where index 3 is a different icon.
    struct TrayIconSlot {
        ID2D1Bitmap1* bitmap = nullptr;
        unsigned long long revision = 0;
        int index = -1;
    };
    TrayIconSlot trayIconSlots_[kMaxTrayIcons];

    // The bitmap for one mixer row's icon. The PNG is decoded and uploaded only when that row's revision
    // changes, so a steady page costs nothing per frame.
    ID2D1Bitmap1* SoundIconAt(ID2D1DeviceContext2* context, int slot, const unsigned char* png,
                              size_t size, unsigned long long revision);

    // The notification sender's icon, uploaded once per revision -- same arrangement as the mixer rows.
    struct CachedBitmapSlot {
        ID2D1Bitmap1* bitmap = nullptr;
        unsigned long long revision = 0;
    };
    CachedBitmapSlot notificationIcon_;
    ID2D1Bitmap1* NotificationIconBitmap(ID2D1DeviceContext2* context, const uint8_t* pixels,
                                        int width, int height, unsigned long long revision);
    // The call strip's icon, cached for the same reason (one decode per call, not one per frame).
    CachedBitmapSlot callIcon_;
    ID2D1Bitmap1* CallIconBitmap(ID2D1DeviceContext2* context, const uint8_t* pixels, int width,
                                 int height, unsigned long long revision);

    // The decoded bitmap for one tray icon, cached until the list's revision changes. One place knows how
    // to turn a registry PNG into something drawable, and it is called by the tray page.
    ID2D1Bitmap1* TrayIconBitmap(ID2D1DeviceContext2* context, int index, const TrayIcon& icon,
                                 unsigned long long revision);

    // The Start card's own bitmaps: one per pinned tile and one for the account picture.
    //
    // Keyed by the PNG's ADDRESS rather than by a revision, because the pins' icons arrive one at a time
    // from a worker (see StartApps::ResolveIconAsync): a revision would be bumped once per icon and every
    // bump would throw away every decoded bitmap. The address of a resolved icon is stable for as long as
    // it is that icon, which is exactly the lifetime wanted here.
    struct StartBitmap {
        ID2D1Bitmap1* bitmap = nullptr;
        const uint8_t* png = nullptr;
        size_t size = 0;
    };
    ID2D1Bitmap1* StartBitmapFor(ID2D1DeviceContext2* context, StartBitmap* slot, const uint8_t* png,
                                 size_t size);

    // A single-colour brush, borrowed from a cache rather than created.
    //
    // PUBLIC because the theme switch has to empty it (see CycleTheme in main.cpp): the cache is keyed
    // by the colour it was asked for, so a theme change would leave every colour of the old one in
    // memory for the rest of the session. Nothing else outside this class has a reason to call it.
    //
    // WHY THIS EXISTS. The draw path used to create a brush wherever it needed one -- ninety-odd places
    // in this file -- and a creation is a device call with an allocation behind it. Where the count is
    // constant (a title, a clock) that is a handful per frame and nobody notices; where it scales with
    // what is on screen it is a brush per tray icon, per window in the task row, per tile, per mixer row
    // -- and the bar repaints for as long as it is on screen, which is all day.
    //
    // The cache follows the same rule as the bitmaps above it: keyed by something that identifies the
    // resource (there, a revision; here, the colour), and cleared when the device context is replaced.
    //
    // THE COLOUR IS QUANTISED TO 8 BITS PER CHANNEL, which is what D2D stores anyway -- so the key can
    // never be finer than the brush, and a colour that also depends on a fading `opacity` produces a
    // small number of distinct brushes per animation instead of one per frame. Callers must NOT release
    // what this returns: the cache owns it, and the brush is shared with every other caller that asked
    // for the same colour.
    ID2D1SolidColorBrush* BrushFor(ID2D1DeviceContext2* context, const D2D1_COLOR_F& colour);
    std::unordered_map<unsigned, ID2D1SolidColorBrush*> brushCache_;
    StartBitmap startPinBitmaps_[24];   // as many as can be on screen at once, and then some
    StartBitmap startListBitmaps_[24];  // the same, for the all-applications grid
    StartBitmap startAvatarBitmap_;

    IDWriteTextFormat*   dateFormat_  = nullptr;
    IDWriteTextFormat*   appFormat_   = nullptr;
    // --- the status line's four faces ------------------------------------------------------
    //
    // The run is a hierarchy, not a sentence, and each part gets the face the reference gives it:
    // the weekday and the time are the bright, heavy ends of it and the date is the quiet middle.
    // Sizes are between the card's and the Windows clock's, because this line stands beside the
    // deck's 30 DIP buttons rather than inside a card.
    IDWriteTextFormat*   statusWeekdayFormat_ = nullptr;  // 周一 -- 13, semibold, brightest
    IDWriteTextFormat*   statusDayFormat_     = nullptr;  // 10月12日 -- 12, regular, dim
    IDWriteTextFormat*   statusTimeFormat_    = nullptr;  // 17:49 -- 14.5, semibold, brightest
    IDWriteTextFormat*   statusWeatherFormat_ = nullptr;  // 小雨 / 17° -- 12.5, regular
    // THE PILL'S OWN LINE, one weight heavier than appFormat_.
    //
    // A separate format rather than a heavier appFormat_, because appFormat_ is also what the task
    // buttons, the card's app line and the track times are drawn with -- bolding it would bold all of
    // them, which is not what "字体加粗" was about. Asked for as "现在可以了 美化美化就行了 比如字体加粗".
    IDWriteTextFormat*   pillLineFormat_ = nullptr;
    // The Start card's two faces: the label under a pinned tile (centred, small) and one line of a list.
    IDWriteTextFormat*   startLabelFormat_ = nullptr;
    IDWriteTextFormat*   startRowFormat_ = nullptr;
    // The music card's top row, which needs a different size and alignment from the notification
    // header that appFormat_ serves: the source name is left aligned beside the icon, and the
    // volume readout is right aligned at the far end of the same row.
    IDWriteTextFormat*   appRowFormat_  = nullptr;
    IDWriteTextFormat*   volumeFormat_  = nullptr;
    IDWriteTextFormat*   titleFormat_ = nullptr;
    IDWriteTextFormat*   bodyFormat_  = nullptr;

    // The music card's own formats. They are separate from the notification ones
    // because the card is left aligned while every notification line is centred, and
    // because its title is larger. See the comment in Initialize().
    IDWriteTextFormat*   musicTitleFormat_    = nullptr;
    IDWriteTextFormat*   musicSubtitleFormat_ = nullptr;
    IDWriteTextFormat*   musicLyricFormat_    = nullptr;   // the current lyric line, with the karaoke
    IDWriteTextFormat*   musicLyricNextFormat_ = nullptr;  // the line after it, dimmed
    // The ♪ that marks a cover-less track's empty cover slot. Its own format rather than the title's: the
    // glyph has to read as a placeholder at a glance, and 20 DIP left it a speck in a 92 DIP square.
    IDWriteTextFormat*   coverNoteFormat_     = nullptr;
    IDWriteTextFormat*   glyphFormat_         = nullptr;
    IDWriteTextFormat*   inputMethodFormat_   = nullptr;
    IDWriteTextFormat*   gearGlyphFormat_ = nullptr;

    SYSTEMTIME time_{};
    wchar_t timeText_[16]  = {};   // L"hh:mm"
    wchar_t dateText_[32]  = {};   // L"<weekday>, <month> <day>"
    // The same date, in the two pieces the status line sets apart: "\u5468\u4e00" and
    // "10\u670812\u65e5". Kept beside `dateText_` rather than derived from it at every draw, because
    // the run needs them separately for their weight and colour, and splitting a string on a
    // separator character at draw time is the kind of thing that works until the separator changes.
    wchar_t weekdayText_[8] = {};
    wchar_t dayText_[24]   = {};
    // The island's compact date: "10.18". The card's date line and the clock keep the long form.
    wchar_t shortDateText_[16] = {};
    wchar_t dateKey_[16]   = {};   // L"yyyy-mm-dd", to detect a day rollover
    bool haveTime_ = false;

    // Last music-card layer timings, for the preview report.
    double lastCardUs_ = 0.0;
    double lastCardBackgroundUs_ = 0.0;
    double lastCardArtworkUs_ = 0.0;
};

} // namespace island

