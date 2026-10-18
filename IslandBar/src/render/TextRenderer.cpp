// =====================================================================
//  TextRenderer.cpp
// =====================================================================
#include "render/TextRenderer.h"
#include "render/Theme.h"   // the material: the card's surface, the edge, the accent and the ink
#include "render/GlassMaterial.h"   // the shadow under a card and the grain over it
#include "shell/Weather.h"   // SkyMark: the weather mark the status line draws, per WMO code

#include "media/WicImage.h"   // DecodeImage, for the mixer's application icons
#include "domain/TaskStripLayout.h"

// For the notification field capacities. TextRenderer itself stays free of the
// notification model; it only needs to know how large the buffers it is handed can
// be, and taking those from the model keeps the two from drifting apart.
#include "services/Notification.h"
#include "core/Log.h"

#include <cmath>
#include <cstdio>
#include <cwchar>

#pragma comment(lib, "dwrite.lib")

namespace island {
namespace {
// Formats a millisecond position as the mm:ss the media flyout shows.
//
// Hours are intentionally dropped: a track longer than an hour would read "75:30",
// which is what the system flyout does too, and the card is not the place to invent
// a different convention.
void FormatPosition(long long milliseconds, wchar_t* out, size_t capacity) {
    if (!out || capacity == 0) return;
    if (milliseconds < 0) milliseconds = 0;
    const long long totalSeconds = milliseconds / 1000;
    swprintf(out, capacity, L"%02lld:%02lld", totalSeconds / 60, totalSeconds % 60);
}


template <typename T>
void SafeRelease(T** pp) {
    if (pp && *pp) {
        (*pp)->Release();
        *pp = nullptr;
    }
}

// The pill is near-black, so the text is a light grey rather than pure white:
// pure white on a dark translucent pill glares against a bright desktop.
constexpr float kTextLuminance = 0.93f;

D2D1_COLOR_F TextColour(float opacity) {
    // The theme's ink rather than a fixed grey, which is what makes a theme change the WORDS as well as
    // the surfaces. The luminance above is the built-in default's own value, so 默认 draws exactly what
    // the bar drew before themes existed.
    const float a = (opacity < 0.0f) ? 0.0f : (opacity > 1.0f ? 1.0f : opacity);
    return island::PillInk(a);
}

// Chinese weekday and month names for the card's date line.
//
// Written as \u escapes, NOT as literal UTF-8, and that is deliberate. The
// project compiles with /utf-8 (top-level CMakeLists.txt), so literal UTF-8
// would work -- but only if the file stays UTF-8 on disk. Every edit path that
// touched this file through a non-UTF-8-aware tool has silently re-encoded it
// and turned these strings into mojibake that either failed to compile or, worse,
// would have shipped as garbage. \u escapes are ASCII, so they survive any
// encoding, any editor and any codepage.
//
//   weekdays: \u5468\u65e5 .. \u5468\u516d
//   months:   1\u6708 .. 12\u6708
//   separator between them: \u3001
const wchar_t* const kWeekdays[] = {
    L"\u5468\u65e5", L"\u5468\u4e00", L"\u5468\u4e8c", L"\u5468\u4e09",
    L"\u5468\u56db", L"\u5468\u4e94", L"\u5468\u516d"
};

const wchar_t* const kMonths[] = {
    L"1\u6708",  L"2\u6708",  L"3\u6708",  L"4\u6708",
    L"5\u6708",  L"6\u6708",  L"7\u6708",  L"8\u6708",
    L"9\u6708",  L"10\u6708", L"11\u6708", L"12\u6708"
};

// The separator between the weekday and the month in the date line.
constexpr wchar_t kDateSeparator = L'\u3001';

} // namespace

TextRenderer::~TextRenderer() {
    Shutdown();
}

bool TextRenderer::Initialize() {
    if (factory_) return true;

    HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3),
                                     reinterpret_cast<IUnknown**>(&factory_));
    if (FAILED(hr) || !factory_) {
        SafeRelease(&factory_);
        return false;
    }

    // "Segoe UI Variable Display" is the Windows 11 system face; DirectWrite's
    // own fallback chain covers machines that lack it.
    hr = factory_->CreateTextFormat(
        L"Segoe UI Variable Display", nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        15.0f, L"zh-CN", &timeFormat_);
    if (FAILED(hr)) {
        Shutdown();
        return false;
    }
    timeFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    timeFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    hr = factory_->CreateTextFormat(
        L"Segoe UI Variable Text", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        12.0f, L"zh-CN", &dateFormat_);
    if (FAILED(hr)) {
        Shutdown();
        return false;
    }
    dateFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    dateFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    // The status line's four faces: a hierarchy rather than one sentence (see the header).
    //
    // NO_WRAP on all of them, for the reason the pill's line has it: a part that wrapped inside a
    // 26 DIP capsule would draw its second row over the top of its first, and every part is a
    // fragment -- a weekday, a date, a clock -- that has nothing to wrap at.
    const auto makeStatusFormat = [this](float size, DWRITE_FONT_WEIGHT weight,
                                         IDWriteTextFormat** out) -> bool {
        const HRESULT created = factory_->CreateTextFormat(
            L"Segoe UI Variable Text", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", out);
        if (FAILED(created) || !*out) return false;
        (*out)->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        (*out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return true;
    };
    // The sizes are set by the tighter of the two boxes rather than by taste: the same run is drawn
    // inside a 220 DIP pill and in the 268 DIP band, and the pill -- less its padding -- is what
    // these numbers have to live within. Measured by the self-test, not chosen by eye.
    //
    // THE TIME IS THE ISLAND. The date and the reading are small in the TOP CORNERS; the time is 18 DIP
    // semibold in the CENTRE. They used to be one 11.5/13 run on one line, which made the time a word in a
    // sentence rather than the thing the island is read for.
    makeStatusFormat(9.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &statusWeekdayFormat_);
    makeStatusFormat(9.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &statusDayFormat_);
    makeStatusFormat(20.0f, DWRITE_FONT_WEIGHT_EXTRA_BOLD, &statusTimeFormat_);
    makeStatusFormat(9.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &statusWeatherFormat_);

    // The music card's top row: the source name and the volume readout.
    //
    // Its own format rather than reusing appFormat_, which the notification header uses and which
    // is centred at 11 DIP. The reference draws the source name at 13 DIP, left aligned beside the
    // icon, and the volume readout at 11 -- so the row is neither the notification's size nor its
    // alignment.
    hr = factory_->CreateTextFormat(
        L"Segoe UI Variable Text", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        13.0f, L"zh-CN", &appRowFormat_);
    if (SUCCEEDED(hr)) {
        appRowFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        appRowFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        appRowFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    hr = factory_->CreateTextFormat(
        L"Segoe UI Variable Text", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        11.0f, L"zh-CN", &volumeFormat_);
    if (SUCCEEDED(hr)) {
        volumeFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        volumeFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // The Start card's two faces.
    //
    // The label under a pinned tile is centred and one size smaller than a list row: it is read at a
    // glance under a 32 DIP picture, and it is the NAME of the thing above it rather than a line of text
    // to be read. `startRowFormat_` is left-aligned because every list in this bar is.
    hr = factory_->CreateTextFormat(
        L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 10.5f, L"zh-CN", &startLabelFormat_);
    if (SUCCEEDED(hr)) {
        startLabelFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        startLabelFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        startLabelFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    hr = factory_->CreateTextFormat(
        L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 12.5f, L"zh-CN", &startRowFormat_);
    if (SUCCEEDED(hr)) {
        startRowFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        startRowFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        startRowFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    // Notification text: a small sender line, a bold title, and a body that is
    // allowed to wrap onto several lines.
    hr = factory_->CreateTextFormat(
        L"Segoe UI Variable Text", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        11.0f, L"zh-CN", &appFormat_);
    if (SUCCEEDED(hr)) {
        appFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        appFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    // The pill's line: the same size, one weight heavier, so the words on the island read a little
    // more solidly than the same words do in a card. See the member's note about why this is not a
    // heavier appFormat_.
    hr = factory_->CreateTextFormat(
        L"Segoe UI Variable Text", nullptr,
        DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        11.0f, L"zh-CN", &pillLineFormat_);
    if (FAILED(hr)) {
        Shutdown();
        return false;
    }
    pillLineFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    pillLineFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // ONE ROW, ALWAYS. The default is DWRITE_WORD_WRAPPING_WRAP, and with it a line whose draw rectangle is
    // even slightly too narrow is broken into two rows inside a box one row tall -- so the strip showed the
    // head of the message and, under it, the tail. Reported as "是歌词不要换行" and then photographed again on
    // the live bar as `WeChat · hello there my friend` / `again` (captures/call-2-message.png). The line is
    // meant to SCROLL when it does not fit; wrapping was never the intent and is now impossible.
    pillLineFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    hr = factory_->CreateTextFormat(
        L"Segoe UI Variable Display", nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        14.0f, L"zh-CN", &titleFormat_);
    if (SUCCEEDED(hr)) {
        titleFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        titleFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        // Long titles are clipped rather than allowed to push the body off the card.
        titleFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        IDWriteTextFormat1* format1 = nullptr;
        if (SUCCEEDED(titleFormat_->QueryInterface(__uuidof(IDWriteTextFormat1),
                                                   reinterpret_cast<void**>(&format1))) &&
            format1) {
            format1->SetLastLineWrapping(false);
            format1->Release();
        }
    }

    hr = factory_->CreateTextFormat(
        L"Segoe UI Variable Text", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        12.0f, L"zh-CN", &bodyFormat_);
    if (SUCCEEDED(hr)) {
        bodyFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        bodyFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        bodyFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }

    // --- music card ------------------------------------------------------
    //
    // The card is laid out like the Windows 11 media flyout, which means its text is
    // LEFT aligned in the space beside the cover. appFormat_ cannot be reused for that:
    // it is centred, because the notification card centres everything. Sharing it made
    // the track title sit in the middle of its own column, which is visibly wrong.
    hr = factory_->CreateTextFormat(
        L"Segoe UI Variable Display", nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        20.0f, L"zh-CN", &musicTitleFormat_);
    if (SUCCEEDED(hr)) {
        musicTitleFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        musicTitleFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        musicTitleFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    hr = factory_->CreateTextFormat(
        L"Segoe UI Variable Text", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        13.0f, L"zh-CN", &musicSubtitleFormat_);
    if (SUCCEEDED(hr)) {
        musicSubtitleFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        musicSubtitleFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        musicSubtitleFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    // The transport glyphs. Bigger than any body text, because a control has to look
    // like a control; "previous" and "next" are text glyphs, and at 11 DIP they read as
    // punctuation rather than buttons.
    //
    // ... and the same face draws the ♪ that marks a cover-less track: at 20 DIP (the card's own title size)
    // it was a speck in a 92 DIP square, which is the opposite of what a placeholder is for.
    hr = factory_->CreateTextFormat(
        L"Segoe UI Symbol", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        38.0f, L"zh-CN", &coverNoteFormat_);
    if (SUCCEEDED(hr)) {
        coverNoteFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        coverNoteFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        coverNoteFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    hr = factory_->CreateTextFormat(
        L"Segoe UI Symbol", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        19.0f, L"zh-CN", &glyphFormat_);
    if (SUCCEEDED(hr)) {
        glyphFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        glyphFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // The system indicator marks: the icon face, which has designed glyphs for exactly these
    // symbols. Sized to fill a 30 DIP button.
    //
    // This face is used for the drawn marks and NOT for the input-method label, which is a real
    // language character and needs a text face with those glyphs.
    hr = factory_->CreateTextFormat(
        L"Segoe Fluent Icons", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-US", &gearGlyphFormat_);
    if (SUCCEEDED(hr) && gearGlyphFormat_) {
        gearGlyphFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        gearGlyphFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // The input-method label: a character inside a 30 DIP button, so it is sized as a LABEL rather
    // than as text. "Segoe UI" is used rather than the symbol face because the label is a real
    // language character -- Chinese, Japanese or Korean -- and the symbol face has no glyph for
    // those, which would draw an empty box.
    hr = factory_->CreateTextFormat(
        L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"zh-CN", &inputMethodFormat_);
    if (SUCCEEDED(hr) && inputMethodFormat_) {
        inputMethodFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        inputMethodFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    Refresh();
    return true;
}

void TextRenderer::Shutdown() {
    // The brush cache first: those brushes belong to this device context, and a cache that outlived its
    // context would hand out dangling pointers on the next frame.
    ClearBrushCache();
    // The mixer's icon bitmaps go with everything else: they are GPU resources created on this device
    // context's device, and leaving them behind would keep the device alive past Shutdown.
    for (SoundIconSlot& slot : soundIcons_) {
        SafeRelease(&slot.bitmap);
        slot.revision = 0;
    }
    SafeRelease(&notificationIcon_.bitmap);
    notificationIcon_.revision = 0;
    SafeRelease(&callIcon_.bitmap);
    callIcon_.revision = 0;
    SafeRelease(&bodyFormat_);
    SafeRelease(&titleFormat_);
    SafeRelease(&musicTitleFormat_);
    SafeRelease(&coverNoteFormat_);
    SafeRelease(&musicSubtitleFormat_);
    SafeRelease(&glyphFormat_);
    SafeRelease(&inputMethodFormat_);
    SafeRelease(&gearGlyphFormat_);
    SafeRelease(&appRowFormat_);
    SafeRelease(&volumeFormat_);
    SafeRelease(&appFormat_);
    SafeRelease(&startLabelFormat_);
    SafeRelease(&startRowFormat_);
    SafeRelease(&statusWeatherFormat_);
    SafeRelease(&statusTimeFormat_);
    SafeRelease(&statusDayFormat_);
    SafeRelease(&statusWeekdayFormat_);
    SafeRelease(&pillLineFormat_);
    SafeRelease(&dateFormat_);
    SafeRelease(&timeFormat_);
    SafeRelease(&factory_);
    for (TrayIconSlot& slot : trayIconSlots_) {
        SafeRelease(&slot.bitmap);
        slot.revision = 0;
        slot.index = -1;
    }
    haveTime_ = false;
    timeText_[0] = L'\0';
    dateText_[0] = L'\0';
    dateKey_[0] = L'\0';
}

// See the declaration in the header for why this exists at all.
ID2D1SolidColorBrush* TextRenderer::BrushFor(ID2D1DeviceContext2* context,
                                            const D2D1_COLOR_F& colour) {
    if (!context) return nullptr;

    // 8 bits per channel, which is the precision D2D keeps anyway: a key finer than the brush would miss
    // for changes nobody can see, and `opacity` is multiplied into most of these colours, so it would miss
    // on every frame of every animation.
    const auto quantise = [](float value) -> unsigned {
        if (value <= 0.0f) return 0u;
        if (value >= 1.0f) return 255u;
        return static_cast<unsigned>(value * 255.0f + 0.5f);
    };
    const unsigned key = (quantise(colour.a) << 24) | (quantise(colour.r) << 16) |
                         (quantise(colour.g) << 8) | quantise(colour.b);

    const auto found = brushCache_.find(key);
    if (found != brushCache_.end()) return found->second;

    // Built from the QUANTISED value, so what is drawn is exactly what the key describes.
    const D2D1_COLOR_F wanted =
        D2D1::ColorF(static_cast<float>((key >> 16) & 0xFF) / 255.0f,
                     static_cast<float>((key >> 8) & 0xFF) / 255.0f,
                     static_cast<float>(key & 0xFF) / 255.0f,
                     static_cast<float>((key >> 24) & 0xFF) / 255.0f);
    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(context->CreateSolidColorBrush(wanted, &brush)) || !brush) return nullptr;
    brushCache_.emplace(key, brush);
    return brush;
}

void TextRenderer::ClearBrushCache() {
    for (auto& entry : brushCache_) SafeRelease(&entry.second);
    brushCache_.clear();
}

void TextRenderer::Refresh() {
    SYSTEMTIME now = {};
    GetLocalTime(&now);

    const bool sameSecond = haveTime_ &&
                            now.wMinute == time_.wMinute &&
                            now.wHour   == time_.wHour &&
                            now.wSecond == time_.wSecond;
    if (sameSecond) return;

    time_ = now;
    haveTime_ = true;

    // A 24-hour clock, matching what the taskbar this replaces shows on this
    // machine. Which format the user wants is a setting, not a constant, so the
    // choice lives in exactly this one place.
    std::swprintf(timeText_, 16, L"%02u:%02u",
                  static_cast<unsigned>(now.wHour), static_cast<unsigned>(now.wMinute));

    // The date only needs rebuilding when the day rolls over. The freshly built
    // key is compared against the cached one, NOT against time_: time_ was just
    // assigned from `now`, so comparing those two would always match and the date
    // would go stale at midnight until the next day's first tick.
    wchar_t freshKey[16] = {};
    std::swprintf(freshKey, 16, L"%04u-%02u-%02u",
                  static_cast<unsigned>(now.wYear), static_cast<unsigned>(now.wMonth),
                  static_cast<unsigned>(now.wDay));

    if (std::wcscmp(freshKey, dateKey_) != 0) {
        wcsncpy_s(dateKey_, 16, freshKey, _TRUNCATE);

        const unsigned weekday = (now.wDayOfWeek < 7) ? now.wDayOfWeek : 0;
        const unsigned month = (now.wMonth >= 1 && now.wMonth <= 12) ? now.wMonth : 1;

        // The two pieces the status line sets apart, and the one string the card's own date line
        // has always used. Built together so a change of format cannot reach one and miss another.
        wcsncpy_s(weekdayText_, 8, kWeekdays[weekday], _TRUNCATE);
        std::swprintf(dayText_, 24, L"%s%u\u65e5", kMonths[month - 1],
                      static_cast<unsigned>(now.wDay));
        // The compact form the island itself draws: "10.18", no weekday and no month name.
        std::swprintf(shortDateText_, 16, L"%u.%u", static_cast<unsigned>(now.wMonth),
                      static_cast<unsigned>(now.wDay));

        std::swprintf(dateText_, 32, L"%s%c%s%u\u65e5",
                      kWeekdays[weekday], kDateSeparator, kMonths[month - 1],
                      static_cast<unsigned>(now.wDay));
    }
}

void TextRenderer::DrawClock(ID2D1DeviceContext2* context, const RoundedBox& box,
                             ClockStyle style, float opacity) {
    if (!context || !factory_ || !timeFormat_) return;

    Refresh();

    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(context->CreateSolidColorBrush(TextColour(opacity), &brush)) || !brush) {
        return;
    }

    const auto text = [&](const wchar_t* value, IDWriteTextFormat* format,
                          const D2D1_RECT_F& rect) {
        if (!format || !value || value[0] == L'\0') return;
        context->DrawTextW(value, static_cast<UINT32>(std::wcslen(value)), format, rect, brush);
    };

    // Layout is derived from the box so the same call works at any card size.
    //
    // The card reserves its BOTTOM for the notification, and an earlier version
    // ignored that: the date line was placed around the vertical middle, which is
    // exactly where the notification's title lands, so the two printed on top of
    // each other. Clock and date now share the top third.
    const float half = (box.rect.bottom - box.rect.top) * 0.5f;

    if (style == ClockStyle::Compact) {
        text(timeText_, timeFormat_,
             D2D1::RectF(box.rect.left, box.rect.top, box.rect.right, box.rect.bottom));
    } else {
        // Clock on the upper of the two lines, date below it, both inside the top
        // 44% of the card. The notification starts at 52%, leaving a gap.
        text(timeText_, timeFormat_,
             D2D1::RectF(box.rect.left, box.rect.top + half * 0.06f,
                         box.rect.right, box.rect.top + half * 0.56f));
        text(dateText_, dateFormat_,
             D2D1::RectF(box.rect.left, box.rect.top + half * 0.50f,
                         box.rect.right, box.rect.top + half * 0.88f));
    }

    SafeRelease(&brush);
}

// =====================================================================
//  the status line's own layout
//
//  In an anonymous namespace beside the drawing that uses it rather than at the top of the file,
//  because nothing else in this translation unit has any business with it: the run is built,
//  measured and drawn by these functions alone, and that is the property the self-test leans on.
// =====================================================================
namespace {

// Every number the run is laid out from, in one place, so the measuring and the drawing cannot
// disagree about the sizes they are adding up.
struct StatusRunStyle {
    float glyphDip      = 16.0f;   // the weather glyph
    float groupGapDip   = 4.0f;    // inside a group: 周三 / 10月14日, 晴 / 6°
    // BETWEEN the groups, where two hairlines used to be. The brief that asked for this bar's look says
    // "fewer borders and dividers, more white space", and a 1x10 DIP grey bar between "10月14日" and
    // "03:30" is exactly the cheap-looking separator it means: space separates them just as well, and the
    // sizes and weights already say which is which.
    float groupSpaceDip = 10.0f;
    float glyphGapDip   = 4.0f;    // between the weather glyph and the word beside it
};

constexpr StatusRunStyle kStatusRun;

// One piece of the run. `widthDip` is the room it takes and `afterDip` the gap it insists on
// leaving behind it, so the run's total is the sum of the two over all of them.
//
// There is no calendar here: one was drawn at the head of the run for an hour, and the user asked
// for it to go -- "去掉前面的日历图标". The weather glyph stays, and it is now the ONE glyph, in both
// homes (see the note on the forms below).
struct StatusPiece {
    enum class Kind { Text, SkyMark };
    Kind kind = Kind::Text;
    const wchar_t* text = nullptr;
    IDWriteTextFormat* format = nullptr;
    float widthDip = 0.0f;
    float afterDip = 0.0f;
    // WHAT THE WEATHER MARK IS, carried on the piece because the drawing walks the pieces. There was no
    // field here at all before -- the mark was drawn the same way for every sky, which is why a clear
    // night came out as 晴 beside a cloud with rain under it ("天气这个图标对应错了 现在显示晴但是下雨的").
    island::SkyMark mark = island::SkyMark::None;
    // The piece's own brightness, which is the hierarchy the reference is built on: the weekday and
    // the time are the bright ends of the run and the date is the quiet middle.
    float luminance = 0.93f;
};

// The date and the weather are back as of the user's word. MeasureStatusLine measures the full run, so
// the box is the same width either way; this switch only decides what is DRAWN.
constexpr bool kStatusExtrasVisible = true;

// The four faces the run sets its words in. Passed in rather than reached for, because this file's
// helpers are free functions and the formats belong to the TextRenderer that created them.
struct StatusRunFormats {
    IDWriteTextFormat* weekday = nullptr;
    IDWriteTextFormat* day = nullptr;
    IDWriteTextFormat* time = nullptr;
    IDWriteTextFormat* weather = nullptr;
};

float TextWidthIn(IDWriteFactory3* factory, IDWriteTextFormat* format, const wchar_t* text) {
    if (!factory || !format || !text || text[0] == L'\0') return 0.0f;

    // The same wide, tall area TextWidth uses: the width of a piece is a property of the text and
    // the face, not of the box it will end up in.
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(factory->CreateTextLayout(text, static_cast<UINT32>(std::wcslen(text)), format,
                                         8192.0f, 512.0f, &layout)) ||
        !layout) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics = {};
    const HRESULT hr = layout->GetMetrics(&metrics);
    layout->Release();
    return SUCCEEDED(hr) ? metrics.width : 0.0f;
}

// The run, built in one place so the measuring and the drawing walk the SAME list.
//
// ONE RUN FOR BOTH HOMES, which is a simplification the calendar bought: with the calendar at its
// head the run was 235.6 DIP and the 220 DIP pill could not hold it, so the island drew a second,
// glyph-less form. Remove the calendar and what is left -- the weekday, the date, the time, the
// weather mark and two words -- fits the pill with its padding and the band with room to spare, so
// there is no form to choose between any more and the two places cannot even in principle disagree.
//
// THE HAIRLINES ARE GONE (see StatusRunStyle::groupSpaceDip): they were the last thing on the bar that
// looked like a text separator rather than like a surface, and the brief this bar is being drawn to asks
// for fewer of them, not for better ones.
int BuildStatusRun(IDWriteFactory3* factory, const StatusRunFormats& formats, const wchar_t* weekday,
                   const wchar_t* day, const wchar_t* time, const wchar_t* sky,
                   const wchar_t* temperature, int weatherCode, StatusPiece* pieces, int capacity) {
    // "周日 10.18   16:03   ☀ 14°": the weekday and the compact date on the left, the temperature and
    // its sun on the right, the clock alone in the middle. The sun is back -- the icon, not the "晴"
    // word: the word is what made the old right-hand group long.
    (void)sky;

    int count = 0;
    const auto push = [&](const StatusPiece& piece) {
        if (count < capacity) pieces[count++] = piece;
    };
    const auto pushText = [&](const wchar_t* text, IDWriteTextFormat* format, float after,
                              float luminance) {
        if (!text || text[0] == L'\0' || !format) return;
        StatusPiece piece;
        piece.kind = StatusPiece::Kind::Text;
        piece.text = text;
        piece.format = format;
        piece.widthDip = TextWidthIn(factory, format, text);
        piece.afterDip = after;
        piece.luminance = luminance;
        push(piece);
    };
    pushText(weekday, formats.weekday, kStatusRun.groupGapDip, 0.88f);
    pushText(day, formats.day, 0.0f, 0.92f);
    pushText(time, formats.time, 0.0f, 1.0f);
    StatusPiece sun;
    sun.kind = StatusPiece::Kind::SkyMark;
    sun.mark = island::SkyMarkForCode(weatherCode);
    sun.widthDip = kStatusRun.glyphDip;
    sun.afterDip = kStatusRun.glyphGapDip;
    if (sun.mark != island::SkyMark::None) push(sun);
    pushText(temperature, formats.weather, 0.0f, 0.88f);
    return count;
}

float StatusRunWidth(const StatusPiece* pieces, int count) {
    float total = 0.0f;
    for (int i = 0; i < count; ++i) total += pieces[i].widthDip + pieces[i].afterDip;
    return total;
}

// The weather mark: ONE SHAPE PER SKY, drawn rather than set in a font. Flat rather than glossy, because
// the reference's rendering is beyond what geometry can do at 16 DIP and a flat shape reads as the same
// thing without an image to ship.
//
// This was a single cloud with rain under it, drawn for every code -- so 晴 was shown beside a rain cloud.
// Reported as "天气这个图标对应错了 现在显示晴但是下雨的". The sun is the one shape not in the line's blue:
// a warm disc is what makes "sunny" read at this size.
void DrawSkyMark(ID2D1DeviceContext2* context, const D2D1_RECT_F& box, island::SkyMark mark,
                 float opacity) {
    if (!context || mark == island::SkyMark::None) return;

    const float cx = (box.left + box.right) * 0.5f;
    const float cy = (box.top + box.bottom) * 0.5f;

    // The cloud, as three circles and a base in one colour -- they read as a single cloud rather than as
    // three circles, which is why they are drawn in one pass and never outlined. Offset and scaled so a
    // rain or snow cloud can leave room under it for the drops.
    const auto cloud = [&](float offsetX, float offsetY, float scale) {
        ID2D1SolidColorBrush* brush = nullptr;
        if (SUCCEEDED(context->CreateSolidColorBrush(D2D1::ColorF(0.46f, 0.71f, 1.00f, 0.96f * opacity),
                                                     &brush)) &&
            brush) {
            const float x = cx + offsetX;
            const float y = cy + offsetY;
            context->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(x - 3.2f * scale, y - 0.2f * scale), 3.2f * scale,
                              3.0f * scale),
                brush);
            context->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(x + 0.6f * scale, y - 1.6f * scale), 3.8f * scale,
                              3.6f * scale),
                brush);
            context->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(x + 4.0f * scale, y - 0.4f * scale), 2.6f * scale,
                              2.4f * scale),
                brush);
            const D2D1_ROUNDED_RECT base = {
                D2D1::RectF(x - 4.6f * scale, y - 0.6f * scale, x + 5.6f * scale, y + 2.2f * scale),
                1.4f, 1.4f};
            context->FillRoundedRectangle(&base, brush);
            brush->Release();
        }
    };

    const auto sun = [&](float offsetX, float offsetY, float radius) {
        ID2D1SolidColorBrush* brush = nullptr;
        if (SUCCEEDED(context->CreateSolidColorBrush(D2D1::ColorF(0.99f, 0.78f, 0.35f, 0.98f * opacity),
                                                     &brush)) &&
            brush) {
            context->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(cx + offsetX, cy + offsetY), radius, radius), brush);
            for (int i = 0; i < 8; ++i) {
                const float angle = static_cast<float>(i) * 3.14159265f / 4.0f;
                const float inner = radius + 1.1f;
                const float outer = radius + 2.6f;
                context->DrawLine(
                    D2D1::Point2F(cx + offsetX + std::cos(angle) * inner,
                                  cy + offsetY + std::sin(angle) * inner),
                    D2D1::Point2F(cx + offsetX + std::cos(angle) * outer,
                                  cy + offsetY + std::sin(angle) * outer),
                    brush, 1.2f);
            }
            brush->Release();
        }
    };

    // Returns null when the brush cannot be created, so each caller has one `if` rather than a nested one.
    const auto lightInk = [&](float alpha) -> ID2D1SolidColorBrush* {
        ID2D1SolidColorBrush* brush = nullptr;
        if (SUCCEEDED(context->CreateSolidColorBrush(
                D2D1::ColorF(0.66f, 0.86f, 1.00f, alpha * opacity), &brush)) &&
            brush) {
            return brush;
        }
        return nullptr;
    };

    switch (mark) {
        case island::SkyMark::Sun:
            sun(0.0f, 0.0f, 4.6f);
            break;
        case island::SkyMark::SunCloud:
            // The sun peeks out from the upper left and the cloud covers its lower right.
            sun(-3.4f, -3.0f, 3.6f);
            cloud(1.0f, 1.4f, 0.92f);
            break;
        case island::SkyMark::Cloud:
            cloud(0.0f, 0.4f, 1.0f);
            break;
        case island::SkyMark::Rain:
            cloud(0.0f, -1.4f, 0.95f);
            if (ID2D1SolidColorBrush* brush = lightInk(0.92f)) {
                for (int i = 0; i < 3; ++i) {
                    const float x = cx - 2.6f + static_cast<float>(i) * 2.8f;
                    context->DrawLine(D2D1::Point2F(x, cy + 2.0f), D2D1::Point2F(x - 1.4f, cy + 5.6f),
                                      brush, 1.5f);
                }
                brush->Release();
            }
            break;
        case island::SkyMark::Snow:
            cloud(0.0f, -1.4f, 0.95f);
            if (ID2D1SolidColorBrush* brush = lightInk(0.95f)) {
                for (int i = 0; i < 3; ++i) {
                    const float x = cx - 2.6f + static_cast<float>(i) * 2.8f;
                    context->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x - 0.7f, cy + 3.8f), 1.1f, 1.1f),
                                         brush);
                }
                brush->Release();
            }
            break;
        case island::SkyMark::Thunder:
            cloud(0.0f, -1.4f, 0.95f);
            if (ID2D1SolidColorBrush* brush = lightInk(0.98f)) {
                context->DrawLine(D2D1::Point2F(cx - 0.4f, cy + 2.0f),
                                  D2D1::Point2F(cx + 1.4f, cy + 3.4f), brush, 1.6f);
                context->DrawLine(D2D1::Point2F(cx + 1.4f, cy + 3.4f),
                                  D2D1::Point2F(cx - 0.6f, cy + 3.8f), brush, 1.6f);
                context->DrawLine(D2D1::Point2F(cx - 0.6f, cy + 3.8f),
                                  D2D1::Point2F(cx + 0.6f, cy + 6.0f), brush, 1.6f);
                brush->Release();
            }
            break;
        case island::SkyMark::Fog:
            // Three bars, the widest in the middle: fog has no shape of its own, and a cloud would claim a
            // different weather.
            if (ID2D1SolidColorBrush* brush = lightInk(0.90f)) {
                const D2D1_ROUNDED_RECT bars[3] = {
                    {D2D1::RectF(cx - 5.0f, cy - 3.4f, cx + 4.0f, cy - 1.6f), 0.9f, 0.9f},
                    {D2D1::RectF(cx - 6.0f, cy - 0.9f, cx + 6.0f, cy + 0.9f), 0.9f, 0.9f},
                    {D2D1::RectF(cx - 4.0f, cy + 1.6f, cx + 5.0f, cy + 3.4f), 0.9f, 0.9f},
                };
                for (const D2D1_ROUNDED_RECT& bar : bars) context->FillRoundedRectangle(&bar, brush);
                brush->Release();
            }
            break;
        case island::SkyMark::None:
            break;
    }
}

} // namespace

float TextRenderer::MeasureStatusLine(const wchar_t* weekday, const wchar_t* day,
                                      const wchar_t* time, const wchar_t* sky,
                                      const wchar_t* temperature, int weatherCode) const {
    if (!factory_) return 0.0f;
    const StatusRunFormats formats{statusWeekdayFormat_, statusDayFormat_, statusTimeFormat_,
                                   statusWeatherFormat_};
    StatusPiece pieces[12] = {};
    (void)day;
    const int count = BuildStatusRun(factory_, formats, weekday, shortDateText_, time, sky,
                                     temperature, weatherCode, pieces, 12);
    // "10.18   16:03   14°": one row, three zones -- the compact date on the left, the time in the
    // CENTRE, the temperature on the right, all vertically centred. The box is the three widths plus the
    // two 12 DIP gaps and the 4 DIP corner padding on each side.
    int timeIndex = -1;
    for (int i = 0; i < count; ++i) {
        if (pieces[i].format == formats.time) {
            timeIndex = i;
            break;
        }
    }
    if (timeIndex < 0) return StatusRunWidth(pieces, count);
    float dateWidth = 0.0f;
    for (int i = 0; i < timeIndex; ++i) dateWidth += pieces[i].widthDip + pieces[i].afterDip;
    float temperatureWidth = 0.0f;
    for (int i = timeIndex + 1; i < count; ++i) temperatureWidth += pieces[i].widthDip + pieces[i].afterDip;
    return dateWidth + 12.0f + pieces[timeIndex].widthDip + 12.0f + temperatureWidth + 8.0f;
}

// The decoded bitmap for one tray icon, cached until the list's revision changes.
//
// One helper for the button's card and the old inline row alike: the decode is the expensive part (a PNG
// per icon), and there is exactly one place that knows how to do it.
ID2D1Bitmap1* TextRenderer::TrayIconBitmap(ID2D1DeviceContext2* context, int index, const TrayIcon& icon,
                                           unsigned long long revision) {
    if (!context || index < 0 || index >= kMaxTrayIcons) return nullptr;
    if (!icon.png || icon.pngSize == 0) return nullptr;

    TrayIconSlot& slot = trayIconSlots_[index];
    if (slot.bitmap && slot.revision == revision && slot.index == index) return slot.bitmap;

    ID2D1Bitmap1* bitmap = nullptr;
    DecodedImage decoded;
    if (DecodeImage(icon.png, icon.pngSize, &decoded) && decoded.pixels) {
        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f,
            96.0f);
        const UINT32 stride = static_cast<UINT32>(decoded.width) * 4;
        context->CreateBitmap(
            D2D1::SizeU(static_cast<UINT32>(decoded.width), static_cast<UINT32>(decoded.height)),
            decoded.pixels, stride, &properties, &bitmap);
    }
    SafeRelease(&slot.bitmap);
    slot.bitmap = bitmap;
    slot.revision = revision;
    slot.index = index;

    if (!bitmap) {
        // Reported once: a picture that will not decode and a picture drawn where the region clips it look
        // identical on screen, and only one of them is this branch.
        static bool reported = false;
        if (!reported) {
            reported = true;
            LogLine(L"  tray: icon %d has a %zu byte PNG that produced no bitmap", index,
                    icon.pngSize);
        }
    }
    return bitmap;
}

void TextRenderer::DrawTrayButton(ID2D1DeviceContext2* context, const RectF& box, float wash,
                                  bool open, const D2D1_COLOR_F& fill, float opacity) {
    if (!context || box.right <= box.left || opacity <= 0.01f) return;

    // The same background the deck's buttons carry, in the island's own colour, so the bar's two ends read
    // as one object -- and the same wash on hover, so "under the pointer" looks the same everywhere.
    const float radius = (box.bottom - box.top) * 0.5f;
    const D2D1_ROUNDED_RECT rounded = {
        D2D1::RectF(box.left, box.top, box.right, box.bottom), radius, radius};
    if (fill.a > 0.001f) {
        ID2D1SolidColorBrush* background = nullptr;
        if (SUCCEEDED(context->CreateSolidColorBrush(fill, &background)) && background) {
            context->FillRoundedRectangle(&rounded, background);
            context->DrawRoundedRectangle(&rounded, background, 1.0f);
            background->Release();
        }
    }
    // The amount comes up over a tenth of a second (FrameState::trayButtonWash); `open` is a state and so
    // is worth a full wash at once, at a slightly stronger strength.
    const float strength = open ? 0.22f : 0.16f * wash;
    if (strength > 0.01f && opacity > 0.01f) {
        ID2D1SolidColorBrush* washBrush = nullptr;
        if (SUCCEEDED(context->CreateSolidColorBrush(
                PillInk(strength * opacity), &washBrush)) &&
            washBrush) {
            context->FillRoundedRectangle(&rounded, washBrush);
            washBrush->Release();
        }
    }

    // THREE DOTS, not the chevron the system button carries. Two identical marks at the two ends of one bar
    // would read as the same control, and this one means "there is more than fits", which is what the dots
    // say. Drawn as geometry, for the reason the Start button's mark is: no glyph to get wrong and nothing
    // to fail to load.
    ID2D1SolidColorBrush* ink = nullptr;
    const float brightness = open ? 1.0f : 0.90f;
    if (FAILED(context->CreateSolidColorBrush(
            PillInk(brightness * 0.94f * opacity), &ink)) ||
        !ink) {
        return;
    }
    const float cx = (box.left + box.right) * 0.5f;
    const float cy = (box.top + box.bottom) * 0.5f;
    // The dots grow with the button: it is a 30 DIP square now, the same as an application icon, and the
    // three marks were sized for a 22 DIP one -- on the bar they read as a control that had been scaled
    // down rather than as one of the row's beads.
    for (int i = -1; i <= 1; ++i) {
        const float x = cx + static_cast<float>(i) * 7.5f;
        context->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, cy), 2.6f, 2.6f), ink);
    }
    ink->Release();
}

namespace {

// Every card's material, in one place (defined with the system panel's drawing code further down).
// Declared here because the tray page -- which used to fill its own surface, and therefore looked flat
// beside a panel that had a lit one -- is drawn above the definition.
void FillCardSurface(ID2D1DeviceContext2* context, const RoundedBox& box, float opacity,
                     bool moving = false);

} // namespace

void TextRenderer::DrawTrayPage(ID2D1DeviceContext2* context, const RoundedBox& card,
                                const wchar_t* title, const TrayIcon* icons, int count,
                                unsigned long long revision, float opacity, int hoveredIndex,
                                bool moving) {
    if (!context || opacity <= 0.01f) return;

    // THE CARD'S OWN SURFACE, from the one function that draws it (FillCardSurface): the same fill, the same
    // edge, the same surface lighting, the same grain and the same shadow as the panel and the media card.
    //
    // This page used to fill its own rounded rectangle with the theme's card colour and a fringe stroke of
    // the same colour. It matched the OTHERS' colour and nothing else -- no edge, no lighting, no shadow --
    // which is exactly the "看着不协调" it was reported as: the panel beside it had a lit surface and this one
    // was flat. One function draws every card's material, and this was the card that did not call it.
    FillCardSurface(context, card, opacity, moving);

    const TrayPageLayout layout = TrayPageFor(card, count);

    if (title && title[0] != L'\0' && appRowFormat_) {
        // The title in the THEME's ink, like every other word on the card: it was a hard-coded near-white,
        // which is the one colour that shows up as a grey patch in a theme with a coloured ink.
        if (ID2D1SolidColorBrush* ink = BrushFor(context, island::PillInk(0.88f * opacity))) {
            context->DrawTextW(title, static_cast<UINT32>(std::wcslen(title)), appRowFormat_,
                               D2D1::RectF(layout.title.left, layout.title.top, layout.title.right,
                                           layout.title.bottom),
                               ink);
        }
    }

    if (count == 0) return;   // the heading alone, over an empty page

    const island::Theme& theme = island::CurrentTheme();
    for (int i = 0; i < layout.count; ++i) {
        const RectF& cell = layout.cells[i];

        // THE CELL IS DARKER THAN THE CARD for the PAINTED themes, not for the glass one.
        //
        // The shell snapshots these icons onto a near-black background, so a LIGHTER tile shows every one of
        // them as a black square sitting inside it -- which is what "看着不协调" was, past the empty card.
        // A dark tile makes the pictures' own backgrounds disappear into it, and the hairline plus the hover
        // wash are what still say "this is a control". (The panel's own tiles stay light: nothing is drawn
        // inside them but a glyph we choose.)
        //
        // The glass card is the exception: its surface IS the card's own material, and a 0.28 black fill on
        // top of it read as grey squares floating in the glass. The tile there is the same 2.5 DIP rim band
        // as the island's edge, with a white hover -- the card shows through, the icon sits on the glass.
        const float radius = 10.0f;
        const D2D1_ROUNDED_RECT tile = {
            D2D1::RectF(cell.left, cell.top, cell.right, cell.bottom), radius, radius};
        if (theme.lens) {
            island::DrawLensRimBand(context, tile, theme, opacity);
            if (i == hoveredIndex) {
                if (ID2D1SolidColorBrush* wash =
                        BrushFor(context, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f * opacity))) {
                    context->FillRoundedRectangle(&tile, wash);
                }
            }
        } else {
            if (ID2D1SolidColorBrush* tileFill =
                    BrushFor(context, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.28f * opacity))) {
                context->FillRoundedRectangle(&tile, tileFill);
            }
            if (ID2D1SolidColorBrush* tileEdge =
                    BrushFor(context, island::PillInk(0.13f * opacity))) {
                context->DrawRoundedRectangle(&tile, tileEdge, 1.0f);
            }
            if (i == hoveredIndex) {
                ID2D1SolidColorBrush* wash = nullptr;
                if ((wash = BrushFor(context, island::PillInk(0.16f * opacity))) != nullptr) {
                    context->FillRoundedRectangle(&tile, wash);
                }
            }
        }

        ID2D1Bitmap1* bitmap = TrayIconBitmap(context, i, icons[i], revision);
        if (!bitmap) continue;
        // THE PICTURE IS HALF THE CELL, so it grows with it (see TrayPageFor's growth step): a 24 DIP icon
        // centred in a 68 DIP cell reads as lost, and the same 24 in a 48 DIP cell is what the page was
        // designed around. Clamped at the top because the registry's snapshots are 32 DIP.
        const float cellDip = cell.right - cell.left;
        float side = cellDip * 0.5f;
        if (side < 22.0f) side = 22.0f;
        if (side > 34.0f) side = 34.0f;
        const float centreX = (cell.left + cell.right) * 0.5f;
        const float centreY = (cell.top + cell.bottom) * 0.5f;
        const D2D1_RECT_F target = D2D1::RectF(centreX - side * 0.5f, centreY - side * 0.5f,
                                               centreX + side * 0.5f, centreY + side * 0.5f);
        context->DrawBitmap(bitmap, &target, opacity, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    }
}

void TextRenderer::DrawTrayMenu(ID2D1DeviceContext2* context, const island::RectF* rows, int count,
                                const wchar_t* const* labels, int hoveredRow, float opacity) {
    if (!context || !rows || !labels || count <= 0 || opacity <= 0.01f) return;
    Refresh();

    // The surface: a raised rectangle that sits ON the card, so it reads as something in front rather than
    // as a hole in it. The PAINTED themes get the lighter outlined rectangle; the glass theme gets the
    // card's own surface, so the menu is a piece of the same glass rather than an opaque dark panel
    // dropped into it.
    const float pad = kTrayMenuPadDip;
    const D2D1_ROUNDED_RECT box = {
        D2D1::RectF(rows[0].left - pad, rows[0].top - pad, rows[0].right + pad,
                    rows[count - 1].bottom + pad),
        10.0f, 10.0f};
    const island::Theme& theme = island::CurrentTheme();
    if (theme.lens) {
        const island::RoundedBox menuBox = {
            island::RectF{box.rect.left, box.rect.top, box.rect.right, box.rect.bottom}, 10.0f, 10.0f};
        FillCardSurface(context, menuBox, opacity, false);
    } else {
        ID2D1SolidColorBrush* fill = nullptr;
        if (SUCCEEDED(context->CreateSolidColorBrush(
                D2D1::ColorF(0.13f, 0.13f, 0.16f, 0.99f * opacity), &fill)) &&
            fill) {
            context->FillRoundedRectangle(&box, fill);
            fill->Release();
        }
        ID2D1SolidColorBrush* edge = nullptr;
        if (SUCCEEDED(context->CreateSolidColorBrush(
                PillInk(0.12f * opacity), &edge)) &&
            edge) {
            context->DrawRoundedRectangle(&box, edge, 1.0f);
            edge->Release();
        }
    }

    for (int i = 0; i < count; ++i) {
        const RectF& row = rows[i];
        const D2D1_ROUNDED_RECT rowBox = {
            D2D1::RectF(row.left, row.top, row.right, row.bottom), 6.0f, 6.0f};
        if (i == hoveredRow) {
            // A white gloss on the glass; the ink tint on the painted themes. The ink as a wash turned a
            // hovered row into a black bar on a light theme, which is the same defect the task buttons had.
            const D2D1_COLOR_F hover = theme.lens
                                           ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f * opacity)
                                           : PillInk(0.16f * opacity);
            ID2D1SolidColorBrush* wash = nullptr;
            if (SUCCEEDED(context->CreateSolidColorBrush(hover, &wash)) && wash) {
                context->FillRoundedRectangle(&rowBox, wash);
                wash->Release();
            }
        }
        const wchar_t* label = labels[i] ? labels[i] : L"";
        if (label[0] == L'\0') continue;
        ID2D1SolidColorBrush* ink = nullptr;
        if (SUCCEEDED(context->CreateSolidColorBrush(PillInk(0.94f * opacity), &ink)) && ink) {
            if (appRowFormat_) {
                // Left-aligned inside the row, vertically centred by the format's own paragraph alignment.
                context->DrawTextW(label, static_cast<UINT32>(std::wcslen(label)), appRowFormat_,
                                   D2D1::RectF(row.left + 8.0f, row.top, row.right - 8.0f, row.bottom),
                                   ink);
            }
            ink->Release();
        }
    }
}

void TextRenderer::DrawStatusLine(ID2D1DeviceContext2* context, const RoundedBox& box,
                                  float opacity, const wchar_t* sky, const wchar_t* temperature,
                                  int weatherCode, const D2D1_COLOR_F& fill) {
    if (!context || !factory_) return;
    if (opacity <= 0.01f) return;
    if (box.rect.right <= box.rect.left) return;

    Refresh();

    // The surface first, when there is one. In the band it is the island's own colour -- the same
    // background the deck's buttons carry and for the same reason: without it the words sit on
    // whatever the wallpaper happens to be, which is unreadable against a bright photograph. Its
    // alpha follows the strip's own fade, so the strip arrives as a shape rather than as words
    // appearing. Inside the pill the caller passes nothing, because the pill has been filled with
    // exactly this colour already.
    if (fill.a > 0.001f) {
        const D2D1_ROUNDED_RECT rounded = {
            D2D1::RectF(box.rect.left, box.rect.top, box.rect.right, box.rect.bottom), box.radiusX,
            box.radiusY};
        D2D1_COLOR_F background = fill;
        background.a = fill.a * opacity;
        ID2D1SolidColorBrush* brush = nullptr;
        if (SUCCEEDED(context->CreateSolidColorBrush(background, &brush)) && brush) {
            context->FillRoundedRectangle(&rounded, brush);
            // The hairline stroke in the same colour covers the antialiased fringe, exactly as the
            // island's own fill does -- without it a pale seam shows along the capsule's curve.
            context->DrawRoundedRectangle(&rounded, brush, 1.0f);
            brush->Release();
        }
    }

    const StatusRunFormats formats{statusWeekdayFormat_, statusDayFormat_, statusTimeFormat_,
                                   statusWeatherFormat_};
    if (!formats.weekday || !formats.day || !formats.time || !formats.weather) return;

    StatusPiece pieces[12] = {};
    const int count = BuildStatusRun(factory_, formats, weekdayText_, shortDateText_, timeText_, sky,
                                     temperature, weatherCode, pieces, 12);
    if (count <= 0) return;

    int timeIndex = -1;
    for (int i = 0; i < count; ++i) {
        if (pieces[i].format == formats.time) {
            timeIndex = i;
            break;
        }
    }
    if (timeIndex < 0) return;

    // "10.18   16:03   14°": the compact date on the left, the temperature on the right, the time alone
    // in the middle -- one row, all vertically centred. The long Chinese date and the weather group are
    // gone; the three values are the whole line.
    const float centreX = (box.rect.left + box.rect.right) * 0.5f;
    const float centreY = (box.rect.top + box.rect.bottom) * 0.5f;

    const auto drawPiece = [&](const StatusPiece& piece, float x, float top, float height) {
        const float centre = top + height * 0.5f;
        if (piece.kind == StatusPiece::Kind::SkyMark) {
            DrawSkyMark(context, D2D1::RectF(x, centre - height * 0.5f, x + piece.widthDip,
                                             centre + height * 0.5f),
                        piece.mark, opacity);
            return;
        }
        if (!piece.text || piece.text[0] == L'\0' || !piece.format) return;
        ID2D1SolidColorBrush* brush = nullptr;
        const D2D1_COLOR_F colour = island::PillInk(opacity * piece.luminance);
        if (SUCCEEDED(context->CreateSolidColorBrush(colour, &brush)) && brush) {
            context->DrawTextW(piece.text, static_cast<UINT32>(std::wcslen(piece.text)),
                               piece.format,
                               D2D1::RectF(x, top, x + piece.widthDip, top + height), brush);
            brush->Release();
        }
    };

    if (kStatusExtrasVisible) {
        const float sideY = centreY - 6.0f;
        const float sideHeight = 12.0f;

        float temperatureWidth = 0.0f;
        for (int i = timeIndex + 1; i < count; ++i) {
            temperatureWidth += pieces[i].widthDip + pieces[i].afterDip;
        }

        float x = box.rect.left + 4.0f;
        for (int i = 0; i < timeIndex; ++i) {
            drawPiece(pieces[i], x, sideY, sideHeight);
            x += pieces[i].widthDip + pieces[i].afterDip;
        }
        x = box.rect.right - 4.0f - temperatureWidth;
        for (int i = timeIndex + 1; i < count; ++i) {
            drawPiece(pieces[i], x, sideY, sideHeight);
            x += pieces[i].widthDip + pieces[i].afterDip;
        }
    }

    const StatusPiece& timePiece = pieces[timeIndex];
    drawPiece(timePiece, centreX - timePiece.widthDip * 0.5f, centreY - 13.0f, 26.0f);
}

void TextRenderer::CurrentDateParts(wchar_t* weekday, size_t weekdayCapacity, wchar_t* day,
                                    size_t dayCapacity) const {
    if (weekday && weekdayCapacity) weekday[0] = L'\0';
    if (day && dayCapacity) day[0] = L'\0';
    if (weekday && weekdayCapacity) wcsncpy_s(weekday, weekdayCapacity, weekdayText_, _TRUNCATE);
    if (day && dayCapacity) wcsncpy_s(day, dayCapacity, dayText_, _TRUNCATE);
}

ID2D1Bitmap1* TextRenderer::NotificationIconBitmap(ID2D1DeviceContext2* context,
                                                   const uint8_t* pixels, int width, int height,
                                                   unsigned long long revision) {
    if (!context || !pixels || width <= 0 || height <= 0) {
        if (notificationIcon_.bitmap) {
            SafeRelease(&notificationIcon_.bitmap);
            notificationIcon_.revision = 0;
        }
        return nullptr;
    }
    if (notificationIcon_.bitmap && notificationIcon_.revision == revision) {
        return notificationIcon_.bitmap;
    }

    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    const UINT32 stride = static_cast<UINT32>(width) * 4;
    ID2D1Bitmap1* bitmap = nullptr;
    const HRESULT hr = context->CreateBitmap(
        D2D1::SizeU(static_cast<UINT32>(width), static_cast<UINT32>(height)), pixels, stride,
        &properties, &bitmap);
    if (FAILED(hr) || !bitmap) return notificationIcon_.bitmap;

    SafeRelease(&notificationIcon_.bitmap);
    notificationIcon_.bitmap = bitmap;
    notificationIcon_.revision = revision;
    return notificationIcon_.bitmap;
}

ID2D1Bitmap1* TextRenderer::CallIconBitmap(ID2D1DeviceContext2* context, const uint8_t* pixels,
                                           int width, int height, unsigned long long revision) {
    if (!context || !pixels || width <= 0 || height <= 0) {
        if (callIcon_.bitmap) {
            SafeRelease(&callIcon_.bitmap);
            callIcon_.revision = 0;
        }
        return nullptr;
    }
    if (callIcon_.bitmap && callIcon_.revision == revision) return callIcon_.bitmap;

    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    const UINT32 stride = static_cast<UINT32>(width) * 4;
    ID2D1Bitmap1* bitmap = nullptr;
    if (FAILED(context->CreateBitmap(
            D2D1::SizeU(static_cast<UINT32>(width), static_cast<UINT32>(height)), pixels, stride,
            &properties, &bitmap)) ||
        !bitmap) {
        return callIcon_.bitmap;
    }
    SafeRelease(&callIcon_.bitmap);
    callIcon_.bitmap = bitmap;
    callIcon_.revision = revision;
    return callIcon_.bitmap;
}

void TextRenderer::DrawCallStrip(ID2D1DeviceContext2* context, const RoundedBox& box,
                                 const CallStripText& text, float opacity) {
    if (!context) return;

    const float height = box.rect.bottom - box.rect.top;
    const float centreY = (box.rect.top + box.rect.bottom) * 0.5f;
    const float pad = 16.0f;
    float cursorX = box.rect.left + pad;

    // --- the avatar, clipped to a circle -------------------------------------------
    //
    // Round, because that is what the reference does and because a square icon beside a timer reads as two
    // unrelated things. The circle is a geometry pushed as a layer: D2D has no "draw this bitmap in a circle".
    const float avatarSide = height - 13.0f;   // 23 DIP in the 36 DIP pill
    ID2D1Bitmap1* icon = CallIconBitmap(context, text.iconPixels, text.iconWidth, text.iconHeight,
                                        text.iconRevision);
    if (icon) {
        ID2D1Factory* factory = nullptr;
        context->GetFactory(&factory);
        ID2D1EllipseGeometry* circle = nullptr;
        if (factory) {
            factory->CreateEllipseGeometry(
                D2D1::Ellipse(D2D1::Point2F(cursorX + avatarSide * 0.5f, centreY), avatarSide * 0.5f,
                              avatarSide * 0.5f),
                &circle);
        }
        if (circle) {
            context->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), circle,
                                                      D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                               nullptr);
        }
        context->DrawBitmap(icon,
                           D2D1::RectF(cursorX, centreY - avatarSide * 0.5f, cursorX + avatarSide,
                                       centreY + avatarSide * 0.5f),
                           opacity, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        if (circle) {
            context->PopLayer();
            circle->Release();
        }
    } else {
        // No icon to be had: a filled dot in the strip's own ink, so the timer is never left hanging in space.
        ID2D1SolidColorBrush* dot = nullptr;
        if (SUCCEEDED(context->CreateSolidColorBrush(D2D1::ColorF(0.35f, 0.35f, 0.38f, 0.95f * opacity),
                                                     &dot)) &&
            dot) {
            context->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(cursorX + avatarSide * 0.5f, centreY), avatarSide * 0.5f,
                              avatarSide * 0.5f),
                dot);
            dot->Release();
        }
    }
    cursorX += avatarSide + 10.0f;

    // --- the timer, in the green a call is ------------------------------------------
    wchar_t timer[32] = {};
    CallTimerText(text.seconds, timer, 32);
    if (timer[0] != L'\0' && bodyFormat_) {
        ID2D1SolidColorBrush* green = nullptr;
        // The green iOS uses for a call in progress (0x30D158), as fractions of 255.
        const D2D1_COLOR_F callGreen = D2D1::ColorF(0.188f, 0.820f, 0.345f, 0.98f * opacity);
        if (SUCCEEDED(context->CreateSolidColorBrush(callGreen, &green)) && green) {
            const DWRITE_TEXT_ALIGNMENT previousHorizontal = bodyFormat_->GetTextAlignment();
            const DWRITE_PARAGRAPH_ALIGNMENT previousVertical = bodyFormat_->GetParagraphAlignment();
            bodyFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            bodyFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            const float width = 90.0f;
            context->DrawTextW(timer, static_cast<UINT32>(std::wcslen(timer)), bodyFormat_,
                               D2D1::RectF(cursorX, box.rect.top, cursorX + width, box.rect.bottom),
                               green, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            bodyFormat_->SetTextAlignment(previousHorizontal);
            bodyFormat_->SetParagraphAlignment(previousVertical);
            green->Release();
        }
    }

    // --- the level meter, at the right end -------------------------------------------
    //
    // Twelve bars, drawn from the output's PEAK reading: the shape is a fixed silhouette scaled by the level,
    // so it breathes with a voice instead of flickering. An unknown or silent level still draws the short
    // bars, because a row of disappearances looks like a fault and a row of dots looks like a quiet room.
    const int bars = 12;
    const float barWidth = 3.0f;
    const float barGap = 2.0f;
    const float meterWidth = static_cast<float>(bars) * barWidth + static_cast<float>(bars - 1) * barGap;
    const float meterRight = box.rect.right - pad;
    const float meterLeft = meterRight - meterWidth;
    const float maxBarHeight = height - 14.0f;
    const float level = text.levelKnown ? text.level : 0.0f;
    for (int i = 0; i < bars; ++i) {
        const float fraction = CallBarFraction(i, bars, level);
        const float barHeight = 4.0f + fraction * (maxBarHeight - 4.0f);
        const float x = meterLeft + static_cast<float>(i) * (barWidth + barGap);
        ID2D1SolidColorBrush* bar = nullptr;
        // Warm, like the reference: the meter is a level, and the warm end of the palette is where a level
        // lives. The alpha follows the bar's own height so the tall ones read as the loud ones.
        const D2D1_COLOR_F colour = D2D1::ColorF(0.98f, 0.72f, 0.20f,
                                                 (0.35f + 0.60f * fraction) * opacity);
        if (SUCCEEDED(context->CreateSolidColorBrush(colour, &bar)) && bar) {
            const float radius = barWidth * 0.5f;
            context->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(x, centreY - barHeight * 0.5f, x + barWidth,
                                              centreY + barHeight * 0.5f),
                                  radius, radius),
                bar);
            bar->Release();
        }
    }
}

void TextRenderer::CallTimerText(int seconds, wchar_t* out, size_t capacity) {
    if (!out || capacity == 0) return;
    out[0] = L'\0';
    if (seconds < 0) seconds = 0;

    const int hours = seconds / 3600;
    const int minutes = (seconds % 3600) / 60;
    const int secs = seconds % 60;
    if (hours > 0) {
        std::swprintf(out, capacity, L"%d:%02d:%02d", hours, minutes, secs);
    } else {
        std::swprintf(out, capacity, L"%02d:%02d", minutes, secs);
    }
}

float TextRenderer::CallBarFraction(int index, int count, float level) {
    if (count <= 0) return 0.0f;
    if (index < 0) index = 0;
    if (index >= count) index = count - 1;
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;

    // A fixed shape, scaled. The values are the silhouette a voice meter makes -- a hump in the middle with
    // smaller bars to either side -- so the meter still looks like a meter when the level is low, instead of
    // collapsing into a flat line the moment nobody is talking.
    static const float pattern[] = {0.30f, 0.52f, 0.78f, 0.96f, 0.72f, 0.46f,
                                    0.60f, 0.88f, 1.00f, 0.66f, 0.40f, 0.24f};
    constexpr int patternCount = static_cast<int>(sizeof(pattern) / sizeof(pattern[0]));
    const float shape = pattern[index % patternCount];

    // 0.18 is the floor: four DIP of bar, so an idle meter is a row of small dots rather than nothing.
    const float fraction = shape * (0.18f + 0.82f * level);
    return fraction > 1.0f ? 1.0f : fraction;
}

namespace {
// Declared here, in the SAME anonymous namespace as its definition, because the notification card is
// drawn above it and the card's own surface is part of drawing it (see the call in DrawNotification).
// A declaration outside that namespace would have been a second, different function -- which the compiler
// said in as many words: "ambiguous call".
// The second forward declaration of the same function, in a later unnamed-namespace block; the default
// argument lives on the first one (line 908) because a default may be given only once per parameter.
void FillCardSurface(ID2D1DeviceContext2* context, const RoundedBox& box, float opacity,
                     bool moving);

// One line of text with the alignment overridden for this call only. Declared for the same reason as
// FillCardSurface above: it is defined far below and the media card's no-cover placeholder needs it.
void DrawTextIn(ID2D1DeviceContext2* context, IDWriteTextFormat* format, const wchar_t* text,
                const RectF& box, ID2D1Brush* brush, DWRITE_TEXT_ALIGNMENT horizontal,
                DWRITE_PARAGRAPH_ALIGNMENT vertical);

} // namespace

void TextRenderer::DrawNotification(ID2D1DeviceContext2* context, const RoundedBox& box,
                                   const char* app, const char* title, const char* body,
                                   const uint8_t* iconPixels, int iconWidth, int iconHeight,
                                   unsigned long long iconRevision, float opacity) {
    if (!context || !titleFormat_) return;

    // Notification text arrives as UTF-8 from the pipe and DirectWrite wants UTF-16.
    // Converting here, once per draw, keeps every other module in UTF-8.
    const auto widen = [](const char* utf8, wchar_t* out, int capacity) {
        out[0] = L'\0';
        if (!utf8 || !utf8[0]) return;
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, capacity);
    };

    wchar_t wideApp[kAppNameCapacity] = {};
    wchar_t wideTitle[kTitleCapacity] = {};
    wchar_t wideBody[kBodyCapacity] = {};
    widen(app, wideApp, kAppNameCapacity);
    widen(title, wideTitle, kTitleCapacity);
    widen(body, wideBody, kBodyCapacity);

    if (wideApp[0] == L'\0' && wideTitle[0] == L'\0' && wideBody[0] == L'\0') return;

    // THE CARD'S OWN SURFACE, first, so the words sit on it.
    //
    // This was missing, and the comment above FillCardSurface claimed it was not ("the notification and
    // the media card from their own code" -- the media card does, the notification never did). It looked
    // filled for a long time only because the ISLAND's fill happened to follow the card-sized box during
    // a transition; 8.10 pinned that fill back to the pill (correctly -- it was painting a slab), and from
    // then on the notification card was TEXT FLOATING ON THE DESKTOP. Reported as a foreign window
    // appearing: "我有通知的时候 鼠标放在灵动岛上就会出现 不放是不会出现的" -- and the screenshots of it
    // are, word for word, this card's three lines with nothing behind them.
    FillCardSurface(context, box, opacity);

    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(context->CreateSolidColorBrush(TextColour(opacity), &brush)) || !brush) {
        return;
    }

    // The card is stacked: clock and date on top, a rule, then the notification.
    // Everything is derived from the box so the same call works at any card size.
    const float height = box.rect.bottom - box.rect.top;
    const float top = box.rect.top + height * 0.52f;
    const float bottom = box.rect.bottom - height * 0.06f;
    const float available = bottom - top;

    // The text starts to the right of the sender's icon when there is one. The icon is the sender's own
    // executable icon -- see Notification::senderPid -- and it is drawn at the left of the FIRST line, which
    // is where the shell's own toast puts it.
    ID2D1Bitmap1* icon = NotificationIconBitmap(context, iconPixels, iconWidth, iconHeight,
                                                iconRevision);
    float textLeft = box.rect.left + 14.0f;
    const float iconSide = 18.0f;
    if (icon) textLeft += iconSide + 8.0f;

    const auto draw = [&](const wchar_t* value, IDWriteTextFormat* format,
                          float y, float lineHeight) {
        if (!format || !value || value[0] == L'\0') return;
        const D2D1_RECT_F rect = D2D1::RectF(textLeft, y,
                                             box.rect.right - 14.0f, y + lineHeight);
        context->DrawTextW(value, static_cast<UINT32>(std::wcslen(value)), format, rect, brush);
    };

    // The sender line is optional and small; the title is what identifies the
    // notification, so it gets the room if space runs short.
    //
    // Heights come from the fonts' real metrics for the same reason as the
    // now-playing card: a hardcoded height clips the glyphs on a CJK face, and the
    // symptom looks like a rendering fault.
    const TextMetrics measured = Metrics();
    const float appHeight = (measured.secondaryLineHeight > 1.0f)
                                ? measured.secondaryLineHeight
                                : 15.0f;
    const float titleHeight = (measured.titleLineHeight > 1.0f) ? measured.titleLineHeight
                                                               : 20.0f;

    float y = top;
    if (wideApp[0] != L'\0' && available > appHeight + titleHeight) {
        if (icon) {
            const D2D1_RECT_F where =
                D2D1::RectF(box.rect.left + 14.0f, y + (appHeight - iconSide) * 0.5f,
                            box.rect.left + 14.0f + iconSide, y + (appHeight + iconSide) * 0.5f);
            context->DrawBitmap(icon, where, opacity, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        }
        draw(wideApp, appFormat_, y, appHeight);
        y += appHeight;
    } else if (icon && wideTitle[0] != L'\0') {
        // No sender line: the icon goes beside the TITLE instead of being dropped. Half the information is
        // better than none, and a title line is the one line every notification has.
        const D2D1_RECT_F where =
            D2D1::RectF(box.rect.left + 14.0f, y + (titleHeight - iconSide) * 0.5f,
                        box.rect.left + 14.0f + iconSide, y + (titleHeight + iconSide) * 0.5f);
        context->DrawBitmap(icon, where, opacity, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    }
    if (wideTitle[0] != L'\0') {
        draw(wideTitle, titleFormat_, y, titleHeight);
        y += titleHeight;
    }
    if (wideBody[0] != L'\0' && bottom - y > 12.0f) {
        draw(wideBody, bodyFormat_, y, bottom - y);
    }

    SafeRelease(&brush);
}

void TextRenderer::DrawMusicCard(ID2D1DeviceContext2* context, const RoundedBox& box,
                                 const MusicCardText& text, ID2D1Bitmap1* artwork,
                                 int artworkWidth, int artworkHeight,
                                 ID2D1Bitmap1* background, ID2D1Bitmap1* icon,
                                 float opacity) {
    if (!context || !titleFormat_) return;

    // Time the whole card and its two image layers, so a per-frame cost can be attributed
    // rather than guessed at. "The card is laggy" is not actionable; "the background wash
    // costs 1.2 ms" is.
    LARGE_INTEGER cardStart = {};
    LARGE_INTEGER frequency = {};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&cardStart);
    const auto sinceMicros = [&](const LARGE_INTEGER& from) {
        LARGE_INTEGER now = {};
        QueryPerformanceCounter(&now);
        return frequency.QuadPart > 0
                   ? static_cast<double>(now.QuadPart - from.QuadPart) * 1000000.0 /
                         static_cast<double>(frequency.QuadPart)
                   : 0.0;
    };

    const auto widen = [](const char* utf8, wchar_t* out, int cap) {
        out[0] = L'\0';
        if (!utf8 || !utf8[0]) return;
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, cap);
    };

    wchar_t wideTitle[kMusicTitleCapacity] = {};
    wchar_t wideArtist[kMusicArtistCapacity] = {};
    wchar_t wideAlbum[kMusicAlbumCapacity] = {};
    wchar_t wideApp[kMusicAppCapacity] = {};
    widen(text.title, wideTitle, kMusicTitleCapacity);
    widen(text.artist, wideArtist, kMusicArtistCapacity);
    widen(text.album, wideAlbum, kMusicAlbumCapacity);
    widen(text.app, wideApp, kMusicAppCapacity);

    if (wideTitle[0] == L'\0' && wideArtist[0] == L'\0') return;

    const MusicCardLayout layout = MusicCardFor(box);
    const bool playing = text.playing;

    // --- the rounded silhouette everything is clipped to ----------------
    //
    // One geometry, reused for the background and for each layer. Without the clip the
    // blurred artwork and the scrim would square off the card's corners, which is the
    // most visible way a card like this can look wrong.
    ID2D1RoundedRectangleGeometry* silhouette = nullptr;
    {
        const D2D1_ROUNDED_RECT rounded = {
            D2D1::RectF(box.rect.left, box.rect.top, box.rect.right, box.rect.bottom),
            box.radiusX, box.radiusY};
        ID2D1Factory* factory = nullptr;
        context->GetFactory(&factory);
        if (factory) {
            factory->CreateRoundedRectangleGeometry(rounded, &silhouette);
            factory->Release();
        }
    }

    const auto pushSilhouette = [&]() {
        if (!silhouette) return;
        context->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), silhouette,
                                                  D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                           nullptr);
    };
    const auto popSilhouette = [&]() {
        if (silhouette) context->PopLayer();
    };

    // --- 0. THE CARD'S OWN SURFACE, before anything else ----------------
    //
    // The same surface the panel, the 声音输出 page and the notification card draw (FillCardSurface), and it
    // is here for the one case this card used to have no background in at all: a track whose player publishes
    // NO artwork.
    //
    // Both of this card's own backgrounds come from the cover -- the blurred wash below and the scrim under
    // it -- so with `artworkSize == 0` there was nothing behind the words but a 48% scrim over the desktop.
    // Measured: the empty cover slot read 6.16 out of the GPU, where the surface reads about 26 (the
    // notification card's own top band reads 27.19). Reported, over a bright wallpaper, as "背景色 不显示了":
    // the words and the progress bar floating on the photograph, which is exactly what a settings surface
    // must never look like.
    //
    // It cannot change the look of a card that HAS artwork: the wash is an opaque image drawn over the whole
    // card, so this is hidden by it in the case that already worked.
    FillCardSurface(context, box, opacity);

    // --- 1. the blurred album art, filling the card ---------------------
    //
    // GLASS, not a photograph: the wash is drawn at just over half strength, so the card's own glass and
    // the desktop behind it still show, and the album only TINTS the card. The rim is drawn again after
    // the scrim, because FillCardSurface's rim is under the wash.
    if (background) {
        pushSilhouette();
        DrawBitmapRect(context, background, layout.artBackground, opacity * 0.55f,
                       D2D1_INTERPOLATION_MODE_LINEAR);
        popSilhouette();
    }

    // --- 2. the scrim ---------------------------------------------------
    //
    // A thin scrim only: 0.48 made the card an opaque dark panel, which hid both the album's colour and
    // the glass. The words no longer need it for contrast -- they choose black or white from the album's
    // own average luminance (text.backgroundLuminance).
    {
        pushSilhouette();
        ID2D1SolidColorBrush* scrim = nullptr;
        const D2D1_COLOR_F scrimColour = D2D1::ColorF(0.04f, 0.05f, 0.08f, 0.15f * opacity);
        if (SUCCEEDED(context->CreateSolidColorBrush(scrimColour, &scrim)) && scrim) {
            context->FillRectangle(
                D2D1::RectF(box.rect.left, box.rect.top, box.rect.right, box.rect.bottom),
                scrim);
            scrim->Release();
        }
        popSilhouette();
    }

    // The glass edge, over the album wash and the thin scrim.
    if (island::CurrentTheme().lens) {
        const D2D1_ROUNDED_RECT rounded = {
            D2D1::RectF(box.rect.left, box.rect.top, box.rect.right, box.rect.bottom),
            box.radiusX, box.radiusY};
        island::DrawLensRimBand(context, rounded, island::CurrentTheme(), opacity);
    }

    // Text colour. Everything below uses these three brushes, so the whole card's contrast moves together.
    // The ALBUM is what is behind the words, not the island: black or white comes from the blurred
    // background's average. PillInk is only the fallback for a track that published no cover.
    const bool albumKnown = text.backgroundLuminance > 0.0f;
    const bool darkAlbum = albumKnown && text.backgroundLuminance < 0.52f;
    const D2D1_COLOR_F primary =
        !albumKnown ? PillInk(opacity)
                    : (darkAlbum ? D2D1::ColorF(0.96f, 0.97f, 1.00f, opacity)
                                 : D2D1::ColorF(0.04f, 0.05f, 0.08f, opacity));
    const D2D1_COLOR_F secondary =
        !albumKnown ? PillInk(0.80f * opacity)
                    : (darkAlbum ? D2D1::ColorF(0.90f, 0.92f, 0.96f, 0.85f * opacity)
                                 : D2D1::ColorF(0.18f, 0.20f, 0.26f, 0.86f * opacity));
    // The accent follows the cover; the fixed blue is only for a cover with no colour and for the
    // no-cover case. On a light album it is darkened so the progress bar still reads.
    D2D1_COLOR_F accent = D2D1::ColorF(0.42f, 0.68f, 0.96f, opacity);
    if (text.accent.a > 0.0f) {
        accent = D2D1::ColorF(text.accent.r, text.accent.g, text.accent.b, opacity);
        if (albumKnown && !darkAlbum) {
            accent.r *= 0.62f;
            accent.g *= 0.62f;
            accent.b *= 0.62f;
        }
    }

    ID2D1SolidColorBrush* primaryBrush = nullptr;
    ID2D1SolidColorBrush* secondaryBrush = nullptr;
    ID2D1SolidColorBrush* accentBrush = nullptr;
    context->CreateSolidColorBrush(primary, &primaryBrush);
    context->CreateSolidColorBrush(secondary, &secondaryBrush);
    context->CreateSolidColorBrush(accent, &accentBrush);

    const auto draw = [&](const wchar_t* value, IDWriteTextFormat* format,
                          const RectF& rect, ID2D1Brush* brush) {
        if (!format || !value || value[0] == L'\0' || !brush) return;
        const D2D1_RECT_F target =
            D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom);
        context->DrawTextW(value, static_cast<UINT32>(std::wcslen(value)), format, target,
                           brush);
    };

    // --- 3. the album cover, clipped to a rounded square ----------------
    if (artwork && artworkWidth > 0 && artworkHeight > 0) {
        ID2D1RoundedRectangleGeometry* artClip = nullptr;
        {
            const D2D1_ROUNDED_RECT rounded = {
                D2D1::RectF(layout.artwork.left, layout.artwork.top, layout.artwork.right,
                            layout.artwork.bottom),
                8.0f, 8.0f};
            ID2D1Factory* factory = nullptr;
            context->GetFactory(&factory);
            if (factory) {
                factory->CreateRoundedRectangleGeometry(rounded, &artClip);
                factory->Release();
            }
        }
        if (artClip) {
            context->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), artClip,
                                                      D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                               nullptr);
            // Cover-fit: fill the square, cropping rather than letterboxing.
            const float boxSide = layout.artwork.right - layout.artwork.left;
            const float scale =
                (static_cast<float>(boxSide) / artworkWidth >
                 static_cast<float>(boxSide) / artworkHeight)
                    ? static_cast<float>(boxSide) / artworkWidth
                    : static_cast<float>(boxSide) / artworkHeight;
            const float drawnWidth = artworkWidth * scale;
            const float drawnHeight = artworkHeight * scale;
            const float left = (layout.artwork.left + layout.artwork.right) * 0.5f -
                               drawnWidth * 0.5f;
            const float top = (layout.artwork.top + layout.artwork.bottom) * 0.5f -
                              drawnHeight * 0.5f;
            DrawBitmapRect(context, artwork,
                           RectF{left, top, left + drawnWidth, top + drawnHeight}, opacity);
            context->PopLayer();
            artClip->Release();
        }
    } else {
        // --- 3b. no cover: a PLACEHOLDER, not a hole ---------------------
        //
        // Some tracks publish no thumbnail at all -- the bar's own log says `music: cover NONE (0 bytes)`, and
        // `gsmtc_probe` says `Thumbnail: none` -- and until now the slot those tracks would have filled was
        // simply EMPTY: a dark rectangle where the cover belongs, which reads as "the bar has lost the
        // picture". Reported twice, in those words: "背景色 不显示了" and "切歌的时候 封面图就没有了".
        //
        // The Windows media flyout this card is modelled on draws a music note in that case, so that is what
        // this is: the same square (same radius as the cover's own clip), a slightly lighter fill so it reads
        // as a place rather than as a window, and a ♪ in the secondary colour.
        const D2D1_ROUNDED_RECT placeholder = {
            D2D1::RectF(layout.artwork.left, layout.artwork.top, layout.artwork.right,
                        layout.artwork.bottom),
            8.0f, 8.0f};
        ID2D1SolidColorBrush* slotBrush = nullptr;
        const D2D1_COLOR_F slotColour =
            D2D1::ColorF(primary.r, primary.g, primary.b, 0.07f * opacity);
        if (SUCCEEDED(context->CreateSolidColorBrush(slotColour, &slotBrush)) && slotBrush) {
            context->FillRoundedRectangle(&placeholder, slotBrush);
            slotBrush->Release();
        }
        if (secondaryBrush && coverNoteFormat_) {
            DrawTextIn(context, coverNoteFormat_, L"\u266a", layout.artwork, secondaryBrush,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }

    // --- 4. the app row: the source, on the left -----------------------
    //
    // The reference image's arrangement, and the one that leaves the row's rest clear for the
    // cover below it. The volume control that used to sit at this row's right-hand end was
    // removed on request -- see the note in CardLayout.h above MusicCardLayout.
    {
        const float rowCentreY = (layout.appRow.top + layout.appRow.bottom) * 0.5f;

        // The application's own icon, when one could be extracted. It is what identifies the
        // player at a glance; a coloured dot there was reported as looking unfinished, and it did.
        if (icon) {
            DrawBitmapRect(context, icon, layout.appIcon, opacity,
                           D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        } else if (accentBrush) {
            // No icon: a dot is better than a hole.
            const float radius = layout.appIconSize * 0.5f;
            const D2D1_ELLIPSE dot = D2D1::Ellipse(
                D2D1::Point2F(layout.appIcon.left + radius, rowCentreY), radius, radius);
            context->FillEllipse(&dot, accentBrush);
        }
        draw(wideApp[0] ? wideApp : L"Media", appRowFormat_, layout.appText, secondaryBrush);
    }

    // --- 5. title and subtitle ------------------------------------------
    draw(wideTitle, musicTitleFormat_, layout.title, primaryBrush);

    wchar_t subtitle[kMusicArtistCapacity + kMusicAlbumCapacity + 8] = {};
    if (wideArtist[0] != L'\0' && wideAlbum[0] != L'\0' &&
        std::wcscmp(wideArtist, wideAlbum) != 0) {
        swprintf(subtitle, kMusicArtistCapacity + kMusicAlbumCapacity + 8, L"%s \u00b7 %s",
                 wideArtist, wideAlbum);
    } else if (wideArtist[0] != L'\0') {
        wcsncpy_s(subtitle, kMusicArtistCapacity + kMusicAlbumCapacity + 8, wideArtist,
                  _TRUNCATE);
    } else {
        wcsncpy_s(subtitle, kMusicArtistCapacity + kMusicAlbumCapacity + 8, wideApp,
                  _TRUNCATE);
    }
    draw(subtitle, musicSubtitleFormat_, layout.subtitle, secondaryBrush);

    // --- 6. times and progress ------------------------------------------
    wchar_t elapsed[16] = {};
    wchar_t total[16] = {};
    FormatPosition(text.positionMs, elapsed, 16);
    // A player that publishes no timeline has no total, and `00:00` there would be a claim it never
    // made -- the bar prints `--:--` instead, the way a flyout with an unknown length does. The elapsed
    // time always prints, because the bar counts it itself when nobody else will (see TrackPositionFor,
    // and NetEase Cloud Music, which reports `EndTime 00:00` for the whole song).
    if (text.durationMs > 0) {
        FormatPosition(text.durationMs, total, 16);
    } else {
        wcsncpy_s(total, 16, L"--:--", _TRUNCATE);
    }
    draw(elapsed, appFormat_, layout.elapsed, secondaryBrush);
    draw(total, appFormat_, layout.remaining, secondaryBrush);

    // The bar spans between the two times rather than the whole card width, so it
    // lines up with them the way the flyout's does.
    const float barLeft = layout.elapsed.right + 6.0f;
    const float barRight = layout.remaining.left - 6.0f;
    if (barRight > barLeft + 8.0f) {
        ID2D1SolidColorBrush* trackBrush = nullptr;
        const D2D1_COLOR_F trackColour = PillInk(0.22f * opacity);
        if (SUCCEEDED(context->CreateSolidColorBrush(trackColour, &trackBrush)) &&
            trackBrush) {
            const D2D1_ROUNDED_RECT track = {
                D2D1::RectF(barLeft, layout.progress.top, barRight, layout.progress.bottom),
                kMusicProgressBarDip * 0.5f, kMusicProgressBarDip * 0.5f};
            context->FillRoundedRectangle(&track, trackBrush);
            trackBrush->Release();
        }

        // A live stream reports no duration, and an empty bar would look broken; the
        // track is drawn but never filled in that case.
        if (text.progressFraction > 0.0f && accentBrush) {
            const float fraction =
                text.progressFraction > 1.0f ? 1.0f : text.progressFraction;
            const float filled = barLeft + (barRight - barLeft) * fraction;
            if (filled > barLeft + 1.0f) {
                const D2D1_ROUNDED_RECT bar = {
                    D2D1::RectF(barLeft, layout.progress.top, filled,
                                layout.progress.bottom),
                    kMusicProgressBarDip * 0.5f, kMusicProgressBarDip * 0.5f};
                context->FillRoundedRectangle(&bar, accentBrush);
            }
        }
    }

    // --- 7. transport controls ------------------------------------------
    // --- 7. transport controls ------------------------------------------
    //
    // The play/pause mark is drawn as geometry, not as a glyph. "||" and a triangle in a
    // text face sit on the baseline with the font's side bearings, so inside a small disc
    // they came out off-centre and at the wrong weight -- they read as characters rather
    // than controls. A triangle and two bars are exact and need no font at all.
    //
    // Previous and next keep their glyphs: those do read correctly at this size, and
    // there is no simple geometry for a bar plus a triangle that is worth the code.
    {
        const float centreY = (layout.controls.top + layout.controls.bottom) * 0.5f;

        // Play/pause: a translucent accent disc with the mark in the text colour.
        {
            const float centreX = (layout.playPause.left + layout.playPause.right) * 0.5f;
            const float radius = (layout.playPause.right - layout.playPause.left) * 0.5f;

            ID2D1SolidColorBrush* discBrush = nullptr;
            const D2D1_COLOR_F discColour =
                D2D1::ColorF(0.42f, 0.68f, 0.96f, 0.32f * opacity);
            if (SUCCEEDED(context->CreateSolidColorBrush(discColour, &discBrush)) &&
                discBrush) {
                const D2D1_ELLIPSE disc =
                    D2D1::Ellipse(D2D1::Point2F(centreX, centreY), radius, radius);
                context->FillEllipse(&disc, discBrush);
                discBrush->Release();
            }

            if (primaryBrush) {
                if (playing) {
                    const float barWidth = 4.5f;
                    const float barHeight = 16.0f;
                    const float gap = 5.0f;
                    context->FillRectangle(
                        D2D1::RectF(centreX - gap * 0.5f - barWidth,
                                    centreY - barHeight * 0.5f, centreX - gap * 0.5f,
                                    centreY + barHeight * 0.5f),
                        primaryBrush);
                    context->FillRectangle(
                        D2D1::RectF(centreX + gap * 0.5f, centreY - barHeight * 0.5f,
                                    centreX + gap * 0.5f + barWidth,
                                    centreY + barHeight * 0.5f),
                        primaryBrush);
                } else {
                    // A right-pointing triangle as a filled path, so its edges are
                    // antialiased like the disc rather than like a stair step.
                    ID2D1Factory* factory = nullptr;
                    context->GetFactory(&factory);
                    ID2D1PathGeometry* triangle = nullptr;
                    ID2D1GeometrySink* sink = nullptr;
                    if (factory && SUCCEEDED(factory->CreatePathGeometry(&triangle)) &&
                        triangle && SUCCEEDED(triangle->Open(&sink)) && sink) {
                        const float half = 9.5f;
                        // Nudged right: a triangle's visual centre sits left of its
                        // bounding box's, so a box-centred one looks off-centre in a
                        // circle.
                        const float nudge = 1.8f;
                        sink->BeginFigure(D2D1::Point2F(centreX - half * 0.8f + nudge,
                                                        centreY - half),
                                          D2D1_FIGURE_BEGIN_FILLED);
                        sink->AddLine(D2D1::Point2F(centreX + half * 0.9f + nudge, centreY));
                        sink->AddLine(D2D1::Point2F(centreX - half * 0.8f + nudge,
                                                    centreY + half));
                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                        sink->Close();
                        context->FillGeometry(triangle, primaryBrush);
                        sink->Release();
                        triangle->Release();
                    }
                    if (factory) factory->Release();
                }
            }
        }

        if (glyphFormat_ && primaryBrush) {
            const auto skipGlyph = [&](const RectF& rect, const wchar_t* glyph) {
                const D2D1_RECT_F target =
                    D2D1::RectF(rect.left, centreY - 14.0f, rect.right, centreY + 14.0f);
                context->DrawTextW(glyph, 1, glyphFormat_, target, primaryBrush);
            };
            skipGlyph(layout.previous, L"\u23ee");
            skipGlyph(layout.next, L"\u23ed");
        }
    }

    if (primaryBrush) primaryBrush->Release();
    if (secondaryBrush) secondaryBrush->Release();
    if (accentBrush) accentBrush->Release();
    if (silhouette) silhouette->Release();
}

// Draws an already-created bitmap into a rectangle.
//
// The music card uses this with bitmaps the renderer caches, because creating a D2D bitmap
// COPIES its pixels to the GPU, so building the album art and the blurred background per
// frame re-uploaded about 150 KB every frame for a picture that had not changed.
//
// The filter is a parameter because the two users differ: the background is a smooth blur
// that linear resampling cannot visibly change, while the cover is small and detailed and
// is worth the cubic filter.
void TextRenderer::DrawBitmapRect(ID2D1DeviceContext2* context, ID2D1Bitmap1* bitmap,
                                  const RectF& target, float opacity,
                                  D2D1_INTERPOLATION_MODE mode) {
    if (!context || !bitmap) return;
    context->DrawBitmap(bitmap,
                        D2D1::RectF(target.left, target.top, target.right, target.bottom),
                        opacity, mode);
}

// Draws raw BGRA premultiplied pixels by building a bitmap for them.
//
// Only DrawArtwork uses this now, which runs once per assertion rather than once per
// frame, so the per-call copy does not matter there.
void TextRenderer::DrawBitmapRect(ID2D1DeviceContext2* context, const uint8_t* pixels,
                                  int width, int height, const RectF& target,
                                  float opacity) {
    if (!context || !pixels || width <= 0 || height <= 0) return;

    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);

    const UINT32 stride = static_cast<UINT32>(width) * 4;
    ID2D1Bitmap1* bitmap = nullptr;
    const HRESULT hr = context->CreateBitmap(
        D2D1::SizeU(static_cast<UINT32>(width), static_cast<UINT32>(height)), pixels,
        stride, &properties, &bitmap);
    if (FAILED(hr) || !bitmap) return;

    context->DrawBitmap(bitmap,
                        D2D1::RectF(target.left, target.top, target.right, target.bottom),
                        opacity, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    bitmap->Release();
}


void TextRenderer::DrawArtwork(ID2D1DeviceContext2* context, const RoundedBox& box,
                               const uint8_t* pixels, int width, int height) {
    if (!context || !pixels || width <= 0 || height <= 0) return;

    const CardContentLayout layout = CardContentFor(box);
    const float side = layout.artwork.right - layout.artwork.left;
    if (side <= 1.0f) return;

    // A bitmap source over the caller's pixels. D2D1_BITMAP_PROPERTIES1 with
    // premultiplied alpha matches what WicImage decodes to, so no conversion pass
    // is needed.
    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);

    const UINT32 stride = static_cast<UINT32>(width) * 4;
    ID2D1Bitmap1* bitmap = nullptr;
    const HRESULT hr = context->CreateBitmap(
        D2D1::SizeU(static_cast<UINT32>(width), static_cast<UINT32>(height)),
        pixels, stride, &properties, &bitmap);
    if (FAILED(hr) || !bitmap) return;

    // Cover-fit into the square: scale by the larger ratio so the image fills the
    // box, and centre it. Cropping a non-square cover is better than letterboxing it
    // inside a 44 DIP square, which would leave visible bars.
    const float scale = (static_cast<float>(side) / static_cast<float>(width) >
                         static_cast<float>(side) / static_cast<float>(height))
                            ? static_cast<float>(side) / static_cast<float>(width)
                            : static_cast<float>(side) / static_cast<float>(height);
    const float drawnWidth = width * scale;
    const float drawnHeight = height * scale;
    const float left = (layout.artwork.left + layout.artwork.right) * 0.5f - drawnWidth * 0.5f;
    const float top = (layout.artwork.top + layout.artwork.bottom) * 0.5f - drawnHeight * 0.5f;

    // Rounded corners so the art matches the card's language instead of being a
    // hard rectangle pasted on it.
    const D2D1_ROUNDED_RECT clip = {
        D2D1::RectF(layout.artwork.left, layout.artwork.top, layout.artwork.right,
                    layout.artwork.bottom),
        6.0f, 6.0f};

    ID2D1RoundedRectangleGeometry* geometry = nullptr;
    ID2D1Factory* factory = nullptr;
    context->GetFactory(&factory);
    if (factory) {
        factory->CreateRoundedRectangleGeometry(clip, &geometry);
        factory->Release();
    }

    if (geometry) {
        context->PushLayer(D2D1::LayerParameters1(
                               D2D1::InfiniteRect(), geometry,
                               D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                           nullptr);
    }

    context->DrawBitmap(bitmap,
                        D2D1::RectF(left, top, left + drawnWidth, top + drawnHeight),
                        1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);

    if (geometry) {
        context->PopLayer();
        geometry->Release();
    }
    bitmap->Release();
}

TextRenderer::TextMetrics TextRenderer::Metrics() const {
    TextMetrics metrics;

    // Measure a line the format will actually be given. The line height is a
    // property of the face and size rather than of the string, but asking with a
    // representative string is the API's contract and costs nothing. The probe mixes
    // CJK and Latin because a CJK face's line height is well above its nominal size,
    // and that is precisely what a hand-sized rectangle gets wrong.
    const auto lineHeightOf = [this](IDWriteTextFormat* format) -> float {
        if (!format || !factory_) return 0.0f;
        IDWriteTextLayout* layout = nullptr;
        const wchar_t* probe = L"\u4e00\u4e8c\u4e09 Ag";
        if (FAILED(factory_->CreateTextLayout(probe, 7, format, 4096.0f, 4096.0f,
                                              &layout)) ||
            !layout) {
            return 0.0f;
        }
        DWRITE_TEXT_METRICS textMetrics = {};
        const HRESULT hr = layout->GetMetrics(&textMetrics);
        layout->Release();
        return SUCCEEDED(hr) ? textMetrics.height : 0.0f;
    };

    metrics.titleLineHeight = lineHeightOf(titleFormat_);
    metrics.secondaryLineHeight = lineHeightOf(appFormat_);
    metrics.pillLineHeight = lineHeightOf(pillLineFormat_);
    return metrics;
}

float TextRenderer::TextWidth(const wchar_t* text, bool titleStyle) const {
    if (!text || !factory_) return 0.0f;
    IDWriteTextFormat* format = titleStyle ? titleFormat_ : appFormat_;
    if (!format) return 0.0f;

    IDWriteTextLayout* layout = nullptr;
    if (FAILED(factory_->CreateTextLayout(text, static_cast<UINT32>(std::wcslen(text)),
                                          format, 8192.0f, 512.0f, &layout)) ||
        !layout) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics = {};
    const HRESULT hr = layout->GetMetrics(&metrics);
    layout->Release();
    return SUCCEEDED(hr) ? metrics.width : 0.0f;
}

float TextRenderer::TrackLineWidth(const wchar_t* text) const {
    if (!text || !factory_) return 0.0f;
    IDWriteTextFormat* format = pillLineFormat_ ? pillLineFormat_ : appFormat_;
    if (!format) return 0.0f;

    // The same 8192-wide, 512-tall area TextWidth uses: the width of a line is a property of the TEXT and the
    // face, not of the box it will be drawn into -- putting it in a box is what wrapping is for, and wrapping
    // is exactly what this line must never do.
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(factory_->CreateTextLayout(text, static_cast<UINT32>(std::wcslen(text)), format,
                                          8192.0f, 512.0f, &layout)) ||
        !layout) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics = {};
    const HRESULT hr = layout->GetMetrics(&metrics);
    layout->Release();
    return SUCCEEDED(hr) ? metrics.width : 0.0f;
}

void TextRenderer::DrawLyricLine(ID2D1DeviceContext2* context, const RoundedBox& box,
                                 const char* utf8Line, float scrollDip, float opacity,
                                 const LineRoll& fade) {
    if (!context || !utf8Line || utf8Line[0] == '\0') return;
    DrawTrackLine(context, box, utf8Line, scrollDip, opacity, fade);
}

void TextRenderer::DrawTrackLine(ID2D1DeviceContext2* context, const RoundedBox& box,
                                 const char* utf8Line, float scrollDip, float opacity,
                                 const LineRoll& roll) {
    // The pill's own heavier format; the fallback keeps this working if that one could not be created.
    IDWriteTextFormat* format = pillLineFormat_ ? pillLineFormat_ : appFormat_;
    if (!context || !format || !utf8Line || utf8Line[0] == '\0') return;

    wchar_t wide[512] = {};
    if (MultiByteToWideChar(CP_UTF8, 0, utf8Line, -1, wide, 512) <= 0) return;

    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(context->CreateSolidColorBrush(TextColour(opacity), &brush)) || !brush) {
        return;
    }

    // Clip to the pill so a scrolling line cannot draw outside the island. The
    // resting pill is a stadium, so its radius is half its height.
    const float radius = (box.rect.bottom - box.rect.top) * 0.5f;
    const D2D1_ROUNDED_RECT clip = {
        D2D1::RectF(box.rect.left, box.rect.top, box.rect.right, box.rect.bottom),
        radius, radius};

    ID2D1RoundedRectangleGeometry* geometry = nullptr;
    ID2D1RectangleGeometry* window = nullptr;
    ID2D1Factory* factory = nullptr;
    context->GetFactory(&factory);
    if (factory) {
        factory->CreateRoundedRectangleGeometry(clip, &geometry);
        // ... and the one-line-tall window the roll happens in, made from the same factory ("the pill
        // is 36 DIP and a CJK line is around 20, so a roll drawn into the pill's full height shows two
        // lines at once, cramped and overlapping" -- reported with a screenshot of exactly that, plus
        // "没有过度动画" and "歌词应该和当前唱的一样才对", because half of what was on screen belonged
        // to the line BEFORE. Inside a one-line window the roll reads as a roll: you see the bottom of
        // the leaving line and the top of the arriving one, and never two whole lines).
        const TextMetrics forWindow = Metrics();
        const float windowLine = (forWindow.pillLineHeight > 1.0f)
                                     ? forWindow.pillLineHeight
                                     : 16.0f;
        const float centre = (box.rect.top + box.rect.bottom) * 0.5f;
        const float half = windowLine * 0.5f + 1.0f;
        factory->CreateRectangleGeometry(
            D2D1::RectF(box.rect.left, centre - half, box.rect.right, centre + half), &window);
        factory->Release();
    }
    if (geometry) {
        context->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), geometry,
                                                  D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                           nullptr);
    }
    if (window) {
        context->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), window,
                                                  D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                           nullptr);
    }

    // One line, vertically centred, given the font's real line height so the glyphs
    // are not clipped -- the same trap the card's title fell into.
    const TextMetrics measured = Metrics();
    const float lineHeight = (measured.pillLineHeight > 1.0f)
                                 ? measured.pillLineHeight
                                 : 16.0f;
    const float midY = (box.rect.top + box.rect.bottom) * 0.5f;
    const float available = box.rect.right - box.rect.left;

    // THE ENDS FADE, for a line that does not fit.
    //
    // The strip's clip is a stadium, so a scrolling line is cut by the ROUNDED CAP -- a curve through the
    // middle of a glyph, which reads as a broken character rather than as text on its way out. Reported
    // with a screenshot: "歌词 长的话能不能会这样" (the leading "na" sliced in half at the left edge).
    //
    // The words still leave the strip; they fade over the last `LyricEndFadeDip` first, so the ends read as
    // a slide. A one-dimensional mask, made once and pushed only around a line that actually scrolls, so a
    // line that fits is drawn exactly as before -- no fade, no mask, nothing to see.
    ID2D1LinearGradientBrush* endFade = nullptr;
    {
        const float fadeDip = island::LyricEndFadeDip(available);
        const float fadeFraction = (available > 1.0f) ? fadeDip / available : 0.0f;
        if (fadeFraction > 0.001f) {
            // Premultiplied and useful only for its ALPHA -- that is what a layer's opacity brush reads.
            const float solid = opacity < 0.0f ? 0.0f : (opacity > 1.0f ? 1.0f : opacity);
            const D2D1_GRADIENT_STOP ramp[4] = {
                {0.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f)},
                {fadeFraction, D2D1::ColorF(solid, solid, solid, solid)},
                {1.0f - fadeFraction, D2D1::ColorF(solid, solid, solid, solid)},
                {1.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f)},
            };
            ID2D1GradientStopCollection* stops = nullptr;
            if (SUCCEEDED(context->CreateGradientStopCollection(
                    ramp, 4, D2D1_GAMMA_1_0, D2D1_EXTEND_MODE_CLAMP, &stops)) &&
                stops) {
                context->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(box.rect.left, midY),
                                                        D2D1::Point2F(box.rect.right, midY)),
                    stops, &endFade);
                stops->Release();
            }
        }
    }

    // ONE LINE, drawn at a given height, centred or scrolling:
    //
    // A line that fits is centred. A longer one starts at the left edge and scrolls left; when it has
    // fully passed, a second copy follows so the loop is seamless rather than gapping.
    const auto drawLine = [&](const wchar_t* text, float centreY, float scroll) {
        // Measured with the format that DRAWS it (see TrackLineWidth): sizing this rectangle from the
        // narrower appFormat_ is what made a Latin line wrap inside a box meant for one row.
        const float textWidth = TrackLineWidth(text);
        const bool scrolling = LyricNeedsScroll(textWidth, available);
        const auto drawAt = [&](float left) {
            const D2D1_RECT_F rect = D2D1::RectF(left, centreY - lineHeight * 0.5f,
                                                 left + (textWidth > 1.0f ? textWidth + 4.0f
                                                                          : available),
                                                 centreY + lineHeight * 0.5f);
            context->DrawTextW(text, static_cast<UINT32>(std::wcslen(text)), format, rect,
                               brush);
        };

        if (!scrolling) {
            drawAt(box.rect.left + (available - textWidth) * 0.5f);
            return;
        }
        // The fade goes on the SCROLLING branch only, and around nothing but the draws: a centred line is
        // untouched by construction rather than by a value that happens to be in the middle.
        const bool faded = (endFade != nullptr);
        if (faded) {
            const D2D1_LAYER_PARAMETERS1 fade = D2D1::LayerParameters1(
                D2D1::RectF(box.rect.left, box.rect.top, box.rect.right, box.rect.bottom), nullptr,
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::Matrix3x2F::Identity(), 1.0f, endFade);
            context->PushLayer(fade, nullptr);
        }
        const float stride = textWidth + 40.0f;   // a gap between repetitions
        // Keep the offset within one stride so the arithmetic cannot drift or grow without bound over
        // a long session.
        const float offset =
            scroll - stride * static_cast<float>(static_cast<long long>(scroll / stride));
        const float start = box.rect.left + 8.0f;
        drawAt(start - offset);
        drawAt(start - offset + stride);
        // A third copy covers the moment the first has scrolled almost fully out and the second has not
        // yet reached the left edge.
        if (offset > 0.0f) drawAt(start - offset - stride);
        if (faded) context->PopLayer();
    };

    // THE ROLL. A line change is a vertical one: the arriving line starts one line height BELOW where it
    // settles and rises into place, while the line it replaces climbs out through the top by the same
    // distance. They never overlap, and the pill's one-line window is what reveals them -- so for the whole
    // of the roll the pill holds the bottom half of one row and the top half of another. THAT IS THE SHAPE
    // THE USER ASKED FOR, twice: "歌词最好是切换的时候是从下面向上滚动" first, and then "还是改回去吧"
    // after a day of the cross-fade that 8.31 put here instead.
    float progress = roll.progress;
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;

    // THE TWO SIDES MOVE IN OPPOSITE DIRECTIONS, which is what makes it a roll:
    //
    //   the line being replaced starts where it IS -- the centre -- and climbs out through the top by
    //   one line height, while the new line starts one line height below and rises to the centre.
    //
    // Both offsets were written as `(1 - progress) * lineHeight`, so the leaving line jumped straight
    // out of the window on the first frame and the window sat EMPTY while the new line came up: the
    // old line appeared to blink out rather than roll away. Reported as "歌词向上消失的时候 应该是向上
    // 滚一点一点消失，新的应该是从下面滚上了 但是现在看起来像没有一样".
    const float arrivingOffset = (1.0f - progress) * lineHeight;
    const float leavingOffset = progress * lineHeight;

    if (progress < 1.0f && roll.previous && roll.previous[0] != '\0') {
        wchar_t previous[512] = {};
        if (MultiByteToWideChar(CP_UTF8, 0, roll.previous, -1, previous, 512) > 0) {
            drawLine(previous, midY - leavingOffset, roll.previousScrollDip);
        }
    }
    drawLine(wide, midY + arrivingOffset, scrollDip);

    // Popped in the reverse of the order they were pushed: the window is inside the stadium.
    if (window) {
        context->PopLayer();
    }
    if (geometry) {
        context->PopLayer();
    }
    SafeRelease(&window);
    if (geometry) geometry->Release();
    SafeRelease(&brush);
    SafeRelease(&endFade);
}

float TextRenderer::TaskTitleLineHeight() const {
    const TextMetrics measured = Metrics();
    return (measured.secondaryLineHeight > 1.0f) ? measured.secondaryLineHeight : 16.0f;
}

float TextRenderer::AudioWaveEnvelope(int step, int steps, float phase) {
    if (steps <= 0) return 0.0f;
    const float t = static_cast<float>(step) / static_cast<float>(steps);   // 0..1 across the pill

    // Two waves of different wavelength and speed, so the shape never looks like it is looping: a long
    // swell across the pill with a short ripple riding on it. One sine alone reads as a flag flapping.
    constexpr float kPi = 3.14159265f;
    const float swell = 0.5f + 0.5f * std::sin(t * kPi * 2.0f + phase);
    // WHOLE periods across the width, both of them: at t = 1 the argument is an EVEN multiple of pi plus
    // the same phase term, so the envelope's first and last heights are equal and the ribbon closes. An
    // odd multiple (the first version used 7) leaves a step at the seam, which is exactly what the
    // assertion below catches.
    const float ripple = 0.5f + 0.5f * std::sin(t * kPi * 8.0f - phase * 1.7f);
    float v = 0.62f * swell + 0.38f * ripple;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return v;
}

void TextRenderer::DrawAudioWave(ID2D1DeviceContext2* context, const RoundedBox& box, float phase,
                                 float level, const D2D1_COLOR_F& accent, float opacity) {
    if (!context) return;
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    const float height = box.rect.bottom - box.rect.top;
    const float width = box.rect.right - box.rect.left;
    if (height <= 4.0f || width <= 8.0f || opacity <= 0.01f) return;

    // Clipped to the pill, so the wave cannot paint outside the island's rounded ends.
    const float radius = height * 0.5f;
    ID2D1RoundedRectangleGeometry* clipGeometry = nullptr;
    ID2D1Factory* factory = nullptr;
    context->GetFactory(&factory);
    if (factory) {
        const D2D1_ROUNDED_RECT clip = {
            D2D1::RectF(box.rect.left, box.rect.top, box.rect.right, box.rect.bottom),
            radius, radius};
        factory->CreateRoundedRectangleGeometry(clip, &clipGeometry);
    }
    if (clipGeometry) {
        context->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), clipGeometry,
                                                  D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                           nullptr);
    }

    // The ribbon: the envelope above the centre line, back along its mirror below, filled as one shape.
    constexpr int kSteps = 48;
    const float midY = (box.rect.top + box.rect.bottom) * 0.5f;
    const float maxHalf = height * 0.5f - 1.0f;
    // Never nothing: a wave that vanished between words would flicker in and out with the music.
    //
    // The floor is a THIRD of the pill, not a tenth. A tenth was invisible in the one place it matters:
    // at the levels music actually plays at, the ribbon sits near the centre line -- which is also where
    // the gradient is dimmest -- so the wave the eye was supposed to notice was the faintest part of it.
    // Measured on a live capture: the pill read as a plain dark pill with a lyric in it.
    const float scale = (0.32f + 0.68f * level) * maxHalf;

    ID2D1PathGeometry* path = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (factory) factory->CreatePathGeometry(&path);
    if (path && SUCCEEDED(path->Open(&sink)) && sink) {
        sink->BeginFigure(D2D1::Point2F(box.rect.left, midY - AudioWaveEnvelope(0, kSteps, phase) * scale),
                          D2D1_FIGURE_BEGIN_FILLED);
        for (int i = 1; i <= kSteps; ++i) {
            const float x = box.rect.left + width * static_cast<float>(i) / kSteps;
            sink->AddLine(D2D1::Point2F(x, midY - AudioWaveEnvelope(i, kSteps, phase) * scale));
        }
        for (int i = kSteps; i >= 0; --i) {
            const float x = box.rect.left + width * static_cast<float>(i) / kSteps;
            sink->AddLine(D2D1::Point2F(x, midY + AudioWaveEnvelope(i, kSteps, phase) * scale));
        }
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
    }
    SafeRelease(&sink);

    // The colour comes from the cover, and the BRIGHTNESS is a vertical gradient: nearly the full accent
    // at the pill's top and bottom edges, a tenth of it across the middle band -- which is exactly where
    // the words are. The wave can then fill the whole pill without ever competing with the line.
    ID2D1GradientStopCollection* stops = nullptr;
    ID2D1LinearGradientBrush* brush = nullptr;
    if (context && accent.a != 0.0f) {
        const auto at = [&](float a) {
            return D2D1::ColorF(accent.r, accent.g, accent.b, a);
        };
        const D2D1_GRADIENT_STOP gradient[3] = {
            {0.0f, at(opacity * 0.75f)},
            {0.5f, at(opacity * 0.35f)},
            {1.0f, at(opacity * 0.75f)},
        };
        if (SUCCEEDED(context->CreateGradientStopCollection(gradient, 3, &stops)) && stops) {
            const D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES props = {
                D2D1::Point2F(box.rect.left, box.rect.top),
                D2D1::Point2F(box.rect.left, box.rect.bottom),
            };
            context->CreateLinearGradientBrush(props, stops, &brush);
        }
    }

    if (path && brush) {
        context->FillGeometry(path, brush);
    }

    SafeRelease(&brush);
    SafeRelease(&stops);
    SafeRelease(&path);
    if (clipGeometry) {
        context->PopLayer();
        clipGeometry->Release();
    }
}

void TextRenderer::DrawTaskIcons(ID2D1DeviceContext2* context, const RoundedBox& island,
                               const TaskButton* buttons, int count, int hoveredIndex,
                               float opacity, const float* rowAmounts, int rowCount,
                               float cardLeftDip, float cardWidthDip,
                               const float* appearAmounts) {
    // Reported whenever the arguments change.
    //
    // "The icons are not drawn" has two causes that a screenshot cannot separate: the draw was called
    // with nothing to draw, or it was called and produced nothing visible. This says which -- and it is
    // here rather than in the caller because the caller's own log already says it asked.
    // The smallest arrival in the row, so "an icon is on its way in" is visible in the log rather than
    // being something only a photograph could show. 1.00 means every icon is settled.
    float least = 1.0f;
    if (appearAmounts) {
        for (int i = 0; i < count; ++i) {
            if (appearAmounts[i] < least) least = appearAmounts[i];
        }
    }
    if (count != lastDrawnCount_ || static_cast<int>(island.rect.left) != lastIslandLeft_ ||
        (least < 0.999f && least != lastLeastAppear_)) {
        lastDrawnCount_ = count;
        lastIslandLeft_ = static_cast<int>(island.rect.left);
        lastLeastAppear_ = least;
        LogLine(L"  draw icons: count=%d, island x [%.0f, %.0f], opacity %.2f, arrive %.2f", count,
                island.rect.left, island.rect.right, opacity, least);
    }

    if (!context || !buttons || count <= 0) return;

    // No clip layer here, unlike the old full-width strip.
    //
    // That strip had rounded ends and a background of its own, so a button could paint over the
    // curve. These buttons sit on the desktop with the island beside them and have no containing
    // shape to overflow, so a layer would cost a push and a pop every frame for nothing.

    // Brushes once for the whole row: a row of a dozen would otherwise create and release a dozen
    // brushes every frame, and the bar draws at the compositor's rate.
    ID2D1SolidColorBrush* hoverBrush = nullptr;
    ID2D1SolidColorBrush* textBrush = nullptr;
    ID2D1SolidColorBrush* activeTextBrush = nullptr;
    const D2D1_COLOR_F hoverColour = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f * opacity);
    const D2D1_COLOR_F textColour = PillInk(0.84f * opacity);
    const D2D1_COLOR_F activeColour = PillInk(0.98f * opacity);
    context->CreateSolidColorBrush(hoverColour, &hoverBrush);
    context->CreateSolidColorBrush(textColour, &textBrush);
    context->CreateSolidColorBrush(activeColour, &activeTextBrush);

    const float lineHeight = TaskTitleLineHeight();

    // Every button's OWN opening amount, gathered once.
    //
    // The geometry is derived from these and not from the hovered index, and that is not a
    // detail. While the pointer travels along the row, one button is opening and the one before it is
    // still closing, so two buttons are mid-transition at the same time. Laying the row out from a
    // single "the hovered one" width ignored the closing neighbour's width entirely, and the opening
    // button then painted over it. The live check reported exactly that:
    //
    //   OVERLAP: button 5 ends at 1641.0 but button 6 starts at 1632.7 (amounts 0.42 / 0.30)
    //
    // `hoveredIndex` is still passed in, but only to decide WHICH button shows a title: that is a
    // question about the pointer, not about geometry.
    float amounts[island::kMaxTaskIcons] = {};
    int shown = count < island::kMaxTaskIcons ? count : island::kMaxTaskIcons;
    for (int i = 0; i < shown; ++i) amounts[i] = buttons[i].hoverAmount;

    // The row these icons sit in is WIDER than the icons when system indicators follow them, so the
    // row parameters are used for the geometry and the local array only as a fallback for callers
    // that have no indicators. Without this the icons would be laid out as though the row ended at
    // the last icon, and the indicators would begin where the icons think they end.
    const float* row = rowAmounts ? rowAmounts : amounts;
    const int rowTotal = (rowAmounts && rowCount >= count) ? rowCount : count;

    for (int i = 0; i < count; ++i) {
        const TaskButton& button = buttons[i];
        // HOW FAR IN THIS ICON IS. A row whose membership changes in one frame pops its icons in and out;
        // "灵动岛右侧 图标出现和消失有点太突兀了没有动画". The amount arrives from the caller (the frame loop
        // owns the clock and the list), and everything this button draws is scaled by it -- including the
        // icon, which also grows into place rather than appearing at full size.
        const float appear = appearAmounts ? appearAmounts[i] : 1.0f;
        if (appear <= 0.01f) continue;
        const RectF box = (i < shown)
                              ? TaskIconButtonAtAmounts(island, i, rowTotal, row, cardLeftDip, cardWidthDip)
                              : TaskIconButtonAt(island, i, count, -1, cardLeftDip, cardWidthDip);
        if (box.right <= box.left) continue;

        // The button's own background, in the ISLAND'S OWN COLOUR.
        //
        // This is what makes the row readable. The icons used to be drawn straight onto whatever was
        // behind them, so on a bright wallpaper both the icon and its title were lost -- which is
        // what was reported. The colour arrives from the caller, which uses the same value it fills
        // the island's pill with, so the two cannot drift apart.
        //
        // The rectangle comes from `box`, the same layout call that positions the icon and the title
        // below, so the background cannot end up somewhere the button is not. A stadium radius --
        // half the height -- matches the island's own roundness.
        //
        // The hairline stroke in the fill colour is there for the same reason the island has one: a
        // plain rounded-rectangle fill leaves an antialiased fringe along the curve, which shows as
        // a pale seam against the desktop.
        // THE BODY IS THE ISLAND'S OWN, not whatever fill the task data carries. The active window's
        // button can arrive with a black fill -- that was the black plate under the black title the user
        // saw. On the glass theme the body is the theme's own pill fill, the same 0.08 the island draws.
        const island::Theme& theme = island::CurrentTheme();
        D2D1_COLOR_F fill = theme.lens ? theme.pillTop : button.fill;
        fill.a *= appear;
        if (fill.a > 0.001f) {
            const float radius = (box.bottom - box.top) * 0.5f;
            const D2D1_ROUNDED_RECT rounded = {
                D2D1::RectF(box.left, box.top, box.right, box.bottom), radius, radius};
            ID2D1SolidColorBrush* background = nullptr;
            if (SUCCEEDED(context->CreateSolidColorBrush(fill, &background)) &&
                background) {
                context->FillRoundedRectangle(&rounded, background);
                // The glass theme's hairline is the 2.5 DIP rim band below; a 1px stroke in the fill
                // colour on top of it is the line the user saw on the island.
                if (!theme.lens) {
                    context->DrawRoundedRectangle(&rounded, background, 1.0f);
                }
                background->Release();
            }
        }

        // THE SAME GLASS AS THE ISLAND. The task buttons used to be flat circles. Drawing the same 2.5 DIP
        // rim band -- bright top, almost nothing at the sides, soft dark bottom -- makes them the same
        // material without a second copy of the numbers.
        if (theme.lens) {
            const float radius = (box.bottom - box.top) * 0.5f;
            const D2D1_ROUNDED_RECT rounded = {
                D2D1::RectF(box.left, box.top, box.right, box.bottom), radius, radius};
            island::DrawLensRimBand(context, rounded, theme, opacity * appear);
        }

        // The window in front is marked by a LIGHTER wash ON TOP of the glass, so the mark is visible
        // whatever the island's own colour happens to be. White, not the ink: on a light glass PillInk is
        // black, and a black wash turned the active button into the black plate the user saw -- with the
        // black title on it.
        if (button.active && hoverBrush) {
            const float radius = (box.bottom - box.top) * 0.5f;
            const D2D1_ROUNDED_RECT pill = {
                D2D1::RectF(box.left, box.top, box.right, box.bottom), radius, radius};
            context->FillRoundedRectangle(&pill, hoverBrush);
        }

        // The icon, placed from THE SAME BOX the background was just drawn in.
        //
        // Not from the resting geometry. That was the bug behind "the background moves but the icons
        // do not": the button was positioned from the animated row while its icon was positioned from
        // the resting row, so as soon as the row made room for a neighbour the button slid and left its
        // icon behind. Deriving both from one rectangle means they cannot part company.
        if (button.icon) {
            RectF iconBox = TaskIconInBox(box);
            // Grown into place, not switched on: at half way the icon is smaller than its box, which is
            // what makes the arrival read as one -- a fade alone at a fixed size still pops.
            if (appear < 0.999f) {
                const float centreX = (iconBox.left + iconBox.right) * 0.5f;
                const float centreY = (iconBox.top + iconBox.bottom) * 0.5f;
                const float scale = 0.70f + 0.30f * appear;
                const float halfW = (iconBox.right - iconBox.left) * 0.5f * scale;
                const float halfH = (iconBox.bottom - iconBox.top) * 0.5f * scale;
                iconBox.left = centreX - halfW;
                iconBox.right = centreX + halfW;
                iconBox.top = centreY - halfH;
                iconBox.bottom = centreY + halfH;
            }
            // ... AND A MINIMISED WINDOW'S ICON IS DIMMED, which nothing did before.
            //
            // The frame has carried `minimised` from the window list all along and no drawing code ever
            // looked at it: measured, zero uses in the renderer. So with ten windows open a minimised one
            // and a normal one were the same picture, and the only way to tell was to click it. The wash
            // marks the window that is IN FRONT; this marks the ones that are put away, which is the other
            // half of the same question.
            const float iconStrength = button.minimised ? 0.55f : 1.0f;
            DrawBitmapRect(context, button.icon, iconBox, opacity * appear * iconStrength,
                           D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        }

        // The title exists only in the ONE growing button, and only when its box is genuinely wide
        // enough to read.
        //
        // The gate used to be a fraction of the growth -- 0.55 -- which sounds like "mostly open" but
        // is a width of only about 24 DIP. A title in a 24 DIP box is trimmed to a single ellipsis,
        // so the button appeared to contain "..." and nothing else, which is exactly what was
        // reported. The gate is now expressed as the thing that actually matters: the room the text
        // has. Below this width there is no point drawing a title at all, because what would be drawn
        // is an ellipsis standing in for a title the user cannot read.
        if (i != hoveredIndex || !button.title || button.title[0] == L'\0') continue;

        const RectF titleBox = (i < shown)
            ? TaskTitleAtAmounts(island, i, count, amounts, hoveredIndex, cardLeftDip, cardWidthDip)
            : TaskTitleAt(island, i, count, -1, 1.0f, cardLeftDip, cardWidthDip);
        if (titleBox.right - titleBox.left < kTaskTitleMinWidthDip) continue;

        // Reported whenever the box the title has to fit in changes size.
        //
        // A pill containing nothing but an ellipsis is what a too-narrow title box looks like, and a
        // screenshot cannot say whether that is because the animation had not finished, because the
        // box is miscomputed, or because the title is genuinely that long. This line gives the width
        // the text was actually given.
        {
            const float width = titleBox.right - titleBox.left;
            if (width != lastTitleWidth_) {
                lastTitleWidth_ = width;
                LogLine(L"  title: button %d, box %.0f..%.0f (%.0f DIP wide), hover %.2f, "
                        L"\"%s\"",
                        i, titleBox.left, titleBox.right, width, button.hoverAmount,
                        button.title ? button.title : L"(none)");
            }
        }

        IDWriteTextFormat* format = button.active ? appRowFormat_ : appFormat_;
        if (!format || !factory_) continue;

        IDWriteTextLayout* layout = nullptr;
        if (FAILED(factory_->CreateTextLayout(
                button.title, static_cast<UINT32>(std::wcslen(button.title)), format,
                titleBox.right - titleBox.left, lineHeight, &layout)) ||
            !layout) {
            continue;
        }

        // A real ellipsis rather than a hard cut: a Chinese title truncated mid-character reads as a
        // rendering fault, while an ellipsis reads as "there is more".
        DWRITE_TRIMMING trimming = {};
        trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
        IDWriteInlineObject* ellipsis = nullptr;
        if (SUCCEEDED(factory_->CreateEllipsisTrimmingSign(format, &ellipsis)) && ellipsis) {
            layout->SetTrimming(&trimming, ellipsis);
            ellipsis->Release();
        }

        const D2D1_POINT_2F origin =
            D2D1::Point2F(titleBox.left,
                          (titleBox.top + titleBox.bottom) * 0.5f - lineHeight * 0.5f);
        ID2D1SolidColorBrush* brush =
            (button.active && activeTextBrush) ? activeTextBrush : textBrush;
        if (brush) {
            context->DrawTextLayout(origin, layout, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        layout->Release();
    }

    SafeRelease(&hoverBrush);
    SafeRelease(&textBrush);
    SafeRelease(&activeTextBrush);
}


// The Segoe Fluent Icons codepoints, in one table.
//
// Every value here was CONFIRMED BY DRAWING IT, because a codepoint the face does not contain renders
// as an empty box and reports no error at all: U+E0CE is an hourglass-shaped nothing in this face while
// U+E702 is the Bluetooth rune. A table is used rather than a switch so the set is visible at a glance
// and the next value can be added and checked the same way.
wchar_t TextRenderer::IndicatorGlyph(IndicatorIcon icon, int value) {
    switch (icon) {
        case IndicatorIcon::InputMethod:
            return 0;   // drawn from its label, not from the icon face
        case IndicatorIcon::Network:
            // The adapter present but not associated. The strength is shown by the arcs the
            // indicator row draws above the dot rather than by a different glyph.
            return 0xE701;
        case IndicatorIcon::Bluetooth:
            return 0xE702;
        case IndicatorIcon::Volume:
            return value == 0 ? 0xE74F : 0xE767;   // muted, or with sound
        case IndicatorIcon::Brightness:
            // The sun, which is the mark the shell's own brightness row carries.
            return 0xE706;
        case IndicatorIcon::Battery:
            if (value <= 10) return 0xE850;
            if (value <= 30) return 0xE851;
            if (value <= 60) return 0xE852;
            if (value <= 90) return 0xE853;
            return 0xE83F;
        case IndicatorIcon::Airplane:
            return 0xE709;
        case IndicatorIcon::Settings:
            return 0xE713;
        case IndicatorIcon::Accessibility:
            return 0xE776;
        case IndicatorIcon::BatterySaver:
            // A LEAF, which is the shell's own mark for 节能模式. It used to be a battery, which is the
            // wrong picture: the tile is not about the battery's charge, it is about saving power, and the
            // reference flyout draws a leaf for it.
            return 0xE8BE;
        case IndicatorIcon::Captions:
            return 0xE7F4;
        case IndicatorIcon::Moon:
            return 0xE708;   // ClearNight
        case IndicatorIcon::Hotspot:
            // Wifi arcs with an antenna between them, which is the shell's 移动热点 mark. E8B2 was the
            // obvious guess ("MobileHotSpot") and it draws a FILM STRIP -- checked by rendering it, which is
            // the only way to tell a wrong codepoint from a right one.
            return 0xE704;
        case IndicatorIcon::Share:
            return 0xE72D;   // Share
        case IndicatorIcon::WiredDisplay:
            return 0xE7F4;   // TVMonitor -- the plain screen, which is the shell's own 有线显示器 mark
        case IndicatorIcon::Projection:
            // Two overlapping screens: the shell's 投影 mark, and the picture of what the tile lists.
            return 0xE7C4;
        case IndicatorIcon::Headphones:
            return 0xE7F6;   // Headphones, for a headset output device
        case IndicatorIcon::DisplayAudio:
            return 0xE7F4;   // A screen, for audio that comes out over HDMI or DisplayPort
        case IndicatorIcon::OutputDevice:
            // Drawn as strokes, not set from the face: the mark is a level control stacked over a speaker,
            // and no single codepoint here is that shape. See DrawOutputDeviceMark.
            return 0;
    }
    return 0;
}

// The stadium background the deck buttons are drawn with: one solid fill, plus a hairline stroke in the
// SAME colour, which is what covers the antialiased fringe a Fill on its own leaves along the rounded
// edges. The island carries the same line for the same reason.
//
// TWO CALLERS, and the reason they are one function is the stroke: it is not decoration, it is the reason
// the shape has no ragged edge, and it was written out twice with only a comment's wording to tell them
// apart (see check-duplicate-code.ps1). The guards are repeated at the call sites because they are about
// control flow there -- the Start button draws a wash after this and must not do it for an empty box.
static void FillStadiumBackground(ID2D1DeviceContext2* context, const RectF& box,
                                  const D2D1_COLOR_F& fill) {
    if (!context || box.right <= box.left || fill.a <= 0.001f) return;
    const float radius = (box.bottom - box.top) * 0.5f;
    const D2D1_ROUNDED_RECT rounded = {
        D2D1::RectF(box.left, box.top, box.right, box.bottom), radius, radius};
    ID2D1SolidColorBrush* background = nullptr;
    if (SUCCEEDED(context->CreateSolidColorBrush(fill, &background)) && background) {
        context->FillRoundedRectangle(&rounded, background);
        context->DrawRoundedRectangle(&rounded, background, 1.0f);
        background->Release();
    }
}

void TextRenderer::DrawSystemButton(ID2D1DeviceContext2* context, const RectF& box, float wash,
                                    bool open, const D2D1_COLOR_F& fill, float opacity) {
    if (!context) return;
    if (box.right <= box.left) return;

    // The same background the other deck buttons get, in the island's own colour, so the Start button,
    // this button and the indicators read as one cluster rather than as marks on the wallpaper.
    FillStadiumBackground(context, box, fill);

    // Spring-animated, like the other two washes (FrameState::systemButtonWash).
    if (wash > 0.01f && opacity > 0.01f) {
        const float radius = (box.bottom - box.top) * 0.5f;
        const D2D1_ROUNDED_RECT rounded = {
            D2D1::RectF(box.left, box.top, box.right, box.bottom), radius, radius};
        ID2D1SolidColorBrush* washBrush = nullptr;
        const D2D1_COLOR_F colour = PillInk(0.16f * opacity * wash);
        if (SUCCEEDED(context->CreateSolidColorBrush(colour, &washBrush)) && washBrush) {
            context->FillRoundedRectangle(&rounded, washBrush);
            washBrush->Release();
        }
    }

    // A chevron, drawn as two strokes rather than set as a glyph: the panel's own icon face has one,
    // but it is a mark with a direction, and the direction is the whole message -- up means "this will
    // close", down means "this will open". Drawing it makes the direction a parameter rather than a
    // second codepoint to get wrong, which is exactly how the Bluetooth glyph went wrong.
    ID2D1SolidColorBrush* ink = nullptr;
    const D2D1_COLOR_F colour = PillInk(0.92f * opacity);
    if (FAILED(context->CreateSolidColorBrush(colour, &ink)) || !ink) return;

    const float cx = (box.left + box.right) * 0.5f;
    const float cy = (box.top + box.bottom) * 0.5f;
    const float halfWidth = 4.6f;
    const float halfHeight = 2.8f;
    const float tipY = open ? cy - halfHeight : cy + halfHeight;
    const float wingY = open ? cy + halfHeight : cy - halfHeight;

    ID2D1Factory* factory = nullptr;
    context->GetFactory(&factory);
    if (factory) {
        ID2D1PathGeometry* geometry = nullptr;
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(factory->CreatePathGeometry(&geometry)) && geometry &&
            SUCCEEDED(geometry->Open(&sink)) && sink) {
            sink->BeginFigure(D2D1::Point2F(cx - halfWidth, wingY), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(D2D1::Point2F(cx, tipY));
            sink->AddLine(D2D1::Point2F(cx + halfWidth, wingY));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();

            ID2D1StrokeStyle* stroke = nullptr;
            if (SUCCEEDED(factory->CreateStrokeStyle(
                    D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                    nullptr, 0, &stroke)) &&
                stroke) {
                context->DrawGeometry(geometry, ink, 1.8f, stroke);
                stroke->Release();
            }
            sink->Release();
            geometry->Release();
        }
    }
    ink->Release();
}

// One mark from the icon face, centred in `box`.
//
// This was a member, `DrawIndicator`, back when the bar drew a ROW of system indicators on its left
// deck. That row is gone, but the marks are not: the system panel's tiles and its volume slider draw
// the same glyphs, and they must keep drawing them from the same table -- the whole point of
// IndicatorGlyph is that one place decides which codepoint a given reading uses.
//
// A free function now because nothing outside this file draws a bare mark any more, and a member would
// invite a caller to re-create the row that was removed.
namespace {

void DrawIndicatorGlyph(ID2D1DeviceContext2* context, TextRenderer::IndicatorIcon icon,
                        const RectF& box, int value, const wchar_t* label, float opacity,
                        IDWriteTextFormat* glyphFormat, IDWriteTextFormat* textFormat) {
    if (!context || !glyphFormat) return;

    ID2D1SolidColorBrush* brush = nullptr;
    const D2D1_COLOR_F ink = D2D1::ColorF(0.94f, 0.95f, 0.98f, 0.95f * opacity);
    if (FAILED(context->CreateSolidColorBrush(ink, &brush)) || !brush) return;

    const float cx = (box.left + box.right) * 0.5f;
    const float cy = (box.top + box.bottom) * 0.5f;
    const float half = (box.bottom - box.top) * 0.5f;
    const D2D1_RECT_F area = D2D1::RectF(cx - half, cy - half, cx + half, cy + half);

    // The input method's content IS a character, so it needs a text face; every other mark is a symbol
    // and comes from the icon face. The marks are glyphs rather than hand-built paths because the first
    // attempt drew them and the Bluetooth rune came out as an hourglass: six straight segments do not
    // describe that shape.
    if (icon == TextRenderer::IndicatorIcon::InputMethod && label && label[0] != L'\0' && textFormat) {
        context->DrawTextW(label, static_cast<UINT32>(std::wcslen(label)), textFormat, area, brush);
    } else {
        const wchar_t glyph = TextRenderer::IndicatorGlyph(icon, value);
        if (glyph != L'\0') {
            context->DrawTextW(&glyph, 1, glyphFormat, area, brush);
        }
    }
    brush->Release();
}

// The "output device" mark: a level control over a speaker, which is the pair of marks the shell's own
// flyout puts at the right of the volume row -- the door into the 声音输出 page.
//
// Drawn rather than taken from the icon face for the reason the deck's chevron is drawn: it is two marks
// stacked, and no single codepoint in either icon face is this shape. A wrong codepoint, meanwhile, comes
// out as an empty box that reports no error at all, which is how the Bluetooth glyph went wrong once.
void DrawOutputDeviceMark(ID2D1DeviceContext2* context, const RectF& box, float opacity) {
    if (!context) return;
    if (box.right <= box.left || box.bottom <= box.top) return;

    ID2D1SolidColorBrush* brush = nullptr;
    const D2D1_COLOR_F ink = D2D1::ColorF(0.94f, 0.95f, 0.98f, 0.90f * opacity);
    if (FAILED(context->CreateSolidColorBrush(ink, &brush)) || !brush) return;

    ID2D1Factory* factory = nullptr;
    context->GetFactory(&factory);
    if (!factory) {
        brush->Release();
        return;
    }

    ID2D1StrokeStyle* round = nullptr;
    factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                           D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                               nullptr, 0, &round);
    const float stroke = 1.6f;

    // The two marks are drawn in a SQUARE, whatever the box's proportions, and that square is 20 DIP --
    // the size a tile's glyph is drawn at. Laying them out as fractions of the rectangle was the first
    // version and it collapsed the speaker into the knob as soon as the button was narrower than it was
    // tall, which is exactly what the row gives it.
    const float side = 20.0f;
    const float cx = (box.left + box.right) * 0.5f;
    const float cy = (box.top + box.bottom) * 0.5f;
    const float left = cx - side * 0.5f;
    const float right = cx + side * 0.5f;
    const float top = cy - side * 0.5f;

    // --- the level control on top: a line broken by an open knob ---
    const float knobY = top + 5.0f;
    const float knobX = left + side * 0.5f;
    const float knobR = 3.3f;
    context->DrawLine(D2D1::Point2F(left, knobY), D2D1::Point2F(knobX - knobR, knobY), brush, stroke,
                      round);
    context->DrawLine(D2D1::Point2F(knobX + knobR, knobY), D2D1::Point2F(right, knobY), brush, stroke,
                      round);
    // Open, so the line's break reads as a knob rather than as a filled dot: the mark is drawn on the
    // card's fill, and a knock-out would have to know what colour that is.
    const D2D1_ELLIPSE knob = D2D1::Ellipse(D2D1::Point2F(knobX, knobY), knobR, knobR);
    context->DrawEllipse(&knob, brush, stroke, round);

    // --- the speaker under it: a line, the cone, and two waves ---
    const float speakerY = top + 15.0f;
    const float coneX = left + 6.0f;
    const float tipX = left + 12.0f;
    context->DrawLine(D2D1::Point2F(left, speakerY), D2D1::Point2F(coneX, speakerY), brush, stroke,
                      round);

    ID2D1PathGeometry* cone = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (SUCCEEDED(factory->CreatePathGeometry(&cone)) && cone &&
        SUCCEEDED(cone->Open(&sink)) && sink) {
        // A rectangle for the magnet and a triangle for the cone, drawn as one filled outline: two
        // shapes would leave a seam where they meet.
        sink->BeginFigure(D2D1::Point2F(coneX, speakerY - 5.6f), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(coneX + 2.2f, speakerY - 5.6f));
        sink->AddLine(D2D1::Point2F(tipX, speakerY - 2.4f));
        sink->AddLine(D2D1::Point2F(tipX, speakerY + 2.4f));
        sink->AddLine(D2D1::Point2F(coneX + 2.2f, speakerY + 5.6f));
        sink->AddLine(D2D1::Point2F(coneX, speakerY + 5.6f));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        context->FillGeometry(cone, brush);
        sink->Release();
        cone->Release();
    }

    // The waves: two arcs off the cone's mouth, in their own geometry because an open figure inside the
    // filled one would be filled as well -- the fill closes every figure, drawn open or not.
    ID2D1PathGeometry* waves = nullptr;
    ID2D1GeometrySink* waveSink = nullptr;
    if (SUCCEEDED(factory->CreatePathGeometry(&waves)) && waves &&
        SUCCEEDED(waves->Open(&waveSink)) && waveSink) {
        for (int i = 0; i < 2; ++i) {
            const float radius = 3.4f + static_cast<float>(i) * 2.8f;
            waveSink->BeginFigure(D2D1::Point2F(tipX + 1.2f, speakerY - radius * 0.60f),
                                  D2D1_FIGURE_BEGIN_HOLLOW);
            const D2D1_ARC_SEGMENT arc = D2D1::ArcSegment(
                D2D1::Point2F(tipX + 1.2f, speakerY + radius * 0.60f),
                D2D1::SizeF(radius, radius), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                D2D1_ARC_SIZE_SMALL);
            waveSink->AddArc(&arc);
            waveSink->EndFigure(D2D1_FIGURE_END_OPEN);
        }
        waveSink->Close();
        context->DrawGeometry(waves, brush, stroke, round);
        waveSink->Release();
        waves->Release();
    }

    if (round) round->Release();
    brush->Release();
}

// The mark for one output device, chosen from its form factor.
//
// EndpointFormFactor (mmdeviceapi.h) says what kind of thing the endpoint is, and the shell's own list
// draws a different picture for each kind. The numbers are stable and the ones that matter here are the
// speakers (0, 1), headphones and a headset (3, 5), and the digital outputs -- SPDIF, HDMI, DisplayPort --
// which are the ones where the sound comes out of a screen.
wchar_t DeviceGlyphFor(int formFactor) {
    switch (formFactor) {
        case 3:    // Headphones
        case 5:    // Headset
            return TextRenderer::IndicatorGlyph(TextRenderer::IndicatorIcon::Headphones, 0);
        case 4:    // SPDIF
        case 6:    // DigitalAudio
        case 8:    // HDMI
        case 10:   // DisplayPort
            return TextRenderer::IndicatorGlyph(TextRenderer::IndicatorIcon::DisplayAudio, 0);
        default:
            // Speakers, and anything the enum does not name: a speaker is the honest default, because an
            // unknown endpoint is much more likely to be one than to be a screen.
            return TextRenderer::IndicatorGlyph(TextRenderer::IndicatorIcon::Volume, 1);
    }
}

// The card's own fill.
//
// The panel and the 声音输出 page call this, the notification card calls it (since 8.21) and the MEDIA card
// calls it too (since 8.33, as its base layer -- the cover's wash and scrim go on top). The media card was
// the last hold-out, and it was the comment right here that let it stay one: "the media card has its own
// (the cover's wash plus a scrim)" is true only when there IS a cover, and a track with `Thumbnail: none`
// got a card with nothing but a scrim over the desktop -- "背景色 不显示了".
//
// The alpha is high for the PAINTED themes: those are settings surfaces that have to stay readable over
// whatever is behind them, which is why the shell's own flyout is opaque too. The glass theme instead uses
// the island's own material -- a 0.10 body, the 2.5 DIP rim band and the bottom caustic, and none of the
// 1px edge, the narrow specular or the grain (see the branch below). Its ink stays the theme's black: on a
// bright desktop the 0.10 light body reads as a lit pane, and the text is readable; on a dark one that is
// the next thing to tune, not something this fill can solve on its own.
//
// The notification card did NOT call it, and this comment used to SAY it did -- which is how a card with
// no background survived review: the only thing that would have caught it is a screenshot of the real bar,
// because the test frames gave the pill a card-sized box (and so a card-sized fill) that the frame loop
// never gives it.
//
// A hairline in the same colour covers the antialiased fringe for the painted themes, for the reason the
// island itself has one: a rounded fill alone leaves a pale seam along the curve. The glass theme uses the
// rim band instead.
void FillCardSurface(ID2D1DeviceContext2* context, const RoundedBox& box, float opacity,
                     bool moving) {
    if (!context) return;
    const float radius = std::max(0.0f, box.radiusX);
    const D2D1_ROUNDED_RECT rounded = {
        D2D1::RectF(box.rect.left, box.rect.top, box.rect.right, box.rect.bottom), radius,
        std::max(0.0f, box.radiusY)};
    // ONE PLACE FOR EVERY CARD'S MATERIAL, which is why a theme re-colours the panel, the tray page,
    // the 声音输出 page, the music card and a notification all at once and cannot leave two of them
    // disagreeing. The edge is the theme's ink at the theme's own edge strength: a hairline that
    // catches the light rather than a second fill colour, which is the plan's "细微边缘反射".
    const island::Theme& theme = island::CurrentTheme();
    // The shadow under the card, then the surface, then the grain over it -- the same three steps the pill
    // is drawn in, from the same two functions, so a card and the bar cannot end up as different materials.
    // THE SHADOW AND THE GRAIN ARE SKIPPED WHILE THE CARD IS MOVING.
    //
    // Both are the least perceptible parts of the material in motion and the most expensive to draw: the
    // shadow is a 22 DIP blur and the grain is a tiled noise fill, and neither can be seen for the length of
    // an animation or under a moving pointer. What is KEPT is everything that makes the surface the surface
    // -- the fill, the hairline edge and the lighting -- so a moving card and a settled one cannot look like
    // two different materials.
    //
    // Reported as "这个托盘卡片里卡卡的": with a card up the loop redraws the whole card on every mouse
    // message AND on every frame of the opening animation, and each of those frames paid for the blur. The
    // full material is drawn again on the first frame that is not moving (see FrameState::cardSurfaceMoving),
    // so the settled look is unchanged.
    if (!moving) {
        island::DrawSoftShadow(context, rounded, theme.shadow, 22.0f);
    }
    const D2D1_COLOR_F fillColour = D2D1::ColorF(theme.cardFill.r, theme.cardFill.g, theme.cardFill.b,
                                                theme.cardFill.a * opacity);
    const D2D1_COLOR_F edgeColour = island::PillInk(theme.cardEdge * opacity);
    // Created here rather than borrowed from the text renderer's brush cache: this function is free
    // rather than a member (the music card and the notification call it before the class is in scope),
    // and it runs once or twice a frame rather than once per icon or per tile.
    ID2D1SolidColorBrush* fill = nullptr;
    if (SUCCEEDED(context->CreateSolidColorBrush(fillColour, &fill)) && fill) {
        context->FillRoundedRectangle(&rounded, fill);
        fill->Release();
    }

    // THE GLASS CARD IS THE ISLAND AT CARD SIZE (2026-10-18): the same 2.5 DIP rim band and the same bottom
    // caustic, over the 0.10 body. No 1px edge (the band is the edge), no surface lighting's narrow
    // specular, no grain: those are what made a card look like frosted plastic beside the lens.
    if (theme.lens) {
        island::DrawLensRimBand(context, rounded, theme, opacity);
        island::DrawLensBottomCaustic(context, rounded, theme, opacity * 0.80f);
        return;
    }

    ID2D1SolidColorBrush* edge = nullptr;
    if (SUCCEEDED(context->CreateSolidColorBrush(edgeColour, &edge)) && edge) {
        context->DrawRoundedRectangle(&rounded, edge, 1.0f);
        edge->Release();
    }
    // THE SAME LIGHTING THE PILL GETS, from the same function: a card's surface and the bar's are the same
    // material at different sizes, and this is where that stops being a claim in a comment.
    island::DrawSurfaceLighting(context, rounded, theme, opacity);
    if (!moving) {
        island::DrawGrain(context, rounded, theme.grain * opacity);
    }
}

// One line of text, with the format's own alignment overridden for the call and put back afterwards.
//
// The formats are shared with half the bar, so the alternative is a new format per alignment -- which is
// how the notification card ended up with three of them. Overriding is what the captions already do.
void DrawTextIn(ID2D1DeviceContext2* context, IDWriteTextFormat* format, const wchar_t* text,
                const RectF& box, ID2D1Brush* brush, DWRITE_TEXT_ALIGNMENT horizontal,
                DWRITE_PARAGRAPH_ALIGNMENT vertical) {
    if (!context || !format || !text || !text[0] || !brush) return;
    if (box.right <= box.left) return;

    const DWRITE_TEXT_ALIGNMENT previousHorizontal = format->GetTextAlignment();
    const DWRITE_PARAGRAPH_ALIGNMENT previousVertical = format->GetParagraphAlignment();
    format->SetTextAlignment(horizontal);
    format->SetParagraphAlignment(vertical);
    const D2D1_RECT_F area = D2D1::RectF(box.left, box.top, box.right, box.bottom);
    context->DrawTextW(text, static_cast<UINT32>(std::wcslen(text)), format, area, brush,
                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
    format->SetTextAlignment(previousHorizontal);
    format->SetParagraphAlignment(previousVertical);
}

}  // namespace

void TextRenderer::DrawSystemPanel(ID2D1DeviceContext2* context, const RoundedBox& box,
                                   const SystemPanelText& text, float opacity) {
    if (!context || !titleFormat_ || !bodyFormat_) return;

    // One slider, or two when the display answers for its brightness (see SystemPanelText::hasBrightness).
    const SystemPanelLayout layout = SystemPanelFor(box, text.tileCount,
                                                    text.hasBrightness ? 2 : 1,
                                                    text.pageCount > 1, text.pageIndex,
                                                    text.pageCount);

    // The card's own fill, FIRST, so everything else sits on it.
    //
    // The system panel has no artwork behind it -- the music card gets its background from the album
    // cover's blurred wash plus a scrim -- so without this the tiles and the slider are drawn straight
    // onto the desktop. Measured on screen: a bright wallpaper showed the tile BORDERS and nothing
    // else, because the tiles are 5% white and there was nothing dark under them.
    //
    // The alpha is high rather than translucent because this is a settings surface: it has to stay
    // readable over whatever is behind it, and the shell's own flyout is opaque for the same reason.
    FillCardSurface(context, box, opacity);

    // The tiles: background, glyph, name.
    for (int i = 0; i < layout.tileCount; ++i) {
        const SystemPanelTile& tile = text.tiles[i];
        const RectF& rect = layout.tiles[i];

        // ON and OFF are different FILLS, which is the whole point of a tile as against a row of text:
        // the state is visible before the label is read. An ON tile takes the system accent so it reads
        // as "this is active"; an OFF tile is a surface with a border, so it still looks pressable.
        {
            // 8 DIP rather than 6: the tiles are taller now that they share the panel's space, and the
            // squarer corner read as a different control from the card's own rounded surface.
            const float radius = 8.0f;
            const D2D1_ROUNDED_RECT rounded = {
                D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom), radius, radius};

            ID2D1SolidColorBrush* fillBrush = nullptr;
            if (tile.on) {
                // The shell's own accent, at a strength that keeps white text readable on it.
                const D2D1_COLOR_F source = island::CurrentTheme().accent;
                const D2D1_COLOR_F accent =
                    D2D1::ColorF(source.r, source.g, source.b, source.a * 0.97f * opacity);
                if ((fillBrush = BrushFor(context, accent)) != nullptr) {
                    context->FillRoundedRectangle(&rounded, fillBrush);
                }
            } else {
                // The OFF tile: a wash of the theme's ink over the card, and a hairline of the same ink.
                // Both were fixed white, which is the one colour a theme with a coloured ink would show up
                // against as a grey patch.
                const D2D1_COLOR_F surface = island::PillInk(0.055f * opacity);
                if ((fillBrush = BrushFor(context, surface)) != nullptr) {
                    context->FillRoundedRectangle(&rounded, fillBrush);
                }
                ID2D1SolidColorBrush* edge = nullptr;
                const D2D1_COLOR_F border = island::PillInk(0.13f * opacity);
                if ((edge = BrushFor(context, border)) != nullptr) {
                    context->DrawRoundedRectangle(&rounded, edge, 1.0f);
                }
            }
        }

        // The glyph, at the box's LEFT and vertically centred, where the shell's flyout puts it.
        DrawIndicatorGlyph(context, tile.icon, layout.tileIcons[i], tile.iconValue, nullptr, opacity,
                           gearGlyphFormat_, bodyFormat_);

        // A CHEVRON on the box's right when the tile opens something, again as the shell does it: the arrow is
        // what tells the eye that pressing this one leads somewhere rather than toggling in place.
        // ... and the CHEVRON only where the tile leads to a LIST, which is the question `chevron` answers rather
        // than "does this tile do anything". The shell's flyout draws it on Wi-Fi, Bluetooth, 辅助功能 and the
        // two display tiles, and on none of the plain switches -- see the field's own comment in the header.
        if (tile.chevron && gearGlyphFormat_ && layout.tiles[i].right > layout.tiles[i].left) {
            ID2D1SolidColorBrush* chevronBrush = nullptr;
            const D2D1_COLOR_F ink = PillInk(0.72f * opacity);
            if ((chevronBrush = BrushFor(context, ink)) != nullptr) {
                const float side = 14.0f;
                const float centreY = (layout.tiles[i].top + layout.tiles[i].bottom) * 0.5f;
                const D2D1_RECT_F area = D2D1::RectF(
                    layout.tiles[i].right - side - 10.0f, centreY - side * 0.5f,
                    layout.tiles[i].right - 10.0f, centreY + side * 0.5f);
                const wchar_t chevron[] = {L'\uE76C', L'\0'};   // ChevronRight, Segoe MDL2 Assets
                context->DrawTextW(chevron, 1, gearGlyphFormat_, area, chevronBrush,
                                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }

        // The CAPTION, centred under the box: the state when there is one, the name otherwise. That is the
        // shell's arrangement -- its Wi-Fi tile reads 可用, not "Wi-Fi" -- and it is what makes the grid
        // scannable without reading every label.
        const wchar_t* caption =
            (tile.stateText && tile.stateText[0] != L'\0') ? tile.stateText : tile.label;
        if (caption && caption[0] != L'\0') {
            ID2D1SolidColorBrush* labelBrush = nullptr;
            const D2D1_COLOR_F ink = PillInk(0.94f * opacity);
            if ((labelBrush = BrushFor(context, ink)) != nullptr) {
                // Centred in both directions: the box IS the caption's line.
                const DWRITE_PARAGRAPH_ALIGNMENT previous = bodyFormat_->GetParagraphAlignment();
                const DWRITE_TEXT_ALIGNMENT previousText = bodyFormat_->GetTextAlignment();
                bodyFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                bodyFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                const D2D1_RECT_F area =
                    D2D1::RectF(layout.tileLabels[i].left, layout.tileLabels[i].top,
                                layout.tileLabels[i].right, layout.tileLabels[i].bottom);
                context->DrawTextW(caption, static_cast<UINT32>(std::wcslen(caption)),
                                   bodyFormat_, area, labelBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                bodyFormat_->SetParagraphAlignment(previous);
                bodyFormat_->SetTextAlignment(previousText);
            }
        }
    }

    // The sliders: a glyph at the left, then a track with the filled part and a handle.
    //
    // Each row carries its OWN mark and its OWN reading -- the speaker with the volume, the sun with the
    // brightness -- and the first version of this drew the speaker on both rows, because the loop was
    // written when there was only ever one row.
    for (int i = 0; i < layout.sliderCount; ++i) {
        const RectF& track = layout.sliders[i];
        const float centreY = (track.top + track.bottom) * 0.5f;
        const float barHeight = 4.0f;
        const float radius = barHeight * 0.5f;

        // Row 0 is the volume, row 1 the brightness (see SystemPanelFor: the rows stack upward from the
        // gear, so index 0 -- the one the device button belongs to -- is the one above).
        const bool brightnessRow = i == 1;
        const float value = brightnessRow ? text.brightness : text.volume;
        const bool mutedRow = !brightnessRow && text.muted;

        if (brightnessRow) {
            DrawIndicatorGlyph(context, IndicatorIcon::Brightness, layout.brightnessIcon,
                               static_cast<int>(value * 100.0f + 0.5f), nullptr, opacity,
                               gearGlyphFormat_, bodyFormat_);
        } else {
            // The speaker, which is also the mute state: the panel must show what the volume says.
            DrawIndicatorGlyph(context, IndicatorIcon::Volume, layout.speakerIcon,
                               text.muted ? 0 : static_cast<int>(text.volume * 100.0f + 0.5f),
                               nullptr, opacity, gearGlyphFormat_, bodyFormat_);
        }

        // The track, then the filled portion, then the handle. Drawn in that order so the fill covers
        // the track and the handle covers the fill.
        ID2D1SolidColorBrush* trackBrush = nullptr;
        const D2D1_COLOR_F trackColour = PillInk(0.22f * opacity);
        if ((trackBrush = BrushFor(context, trackColour)) != nullptr) {
            const D2D1_ROUNDED_RECT rounded = {
                D2D1::RectF(track.left, centreY - barHeight * 0.5f, track.right,
                            centreY + barHeight * 0.5f),
                radius, radius};
            context->FillRoundedRectangle(&rounded, trackBrush);
        }

        float fraction = value;
        if (fraction < 0.0f) fraction = 0.0f;
        if (fraction > 1.0f) fraction = 1.0f;
        if (mutedRow) fraction = 0.0f;

        const float handleX = track.left + (track.right - track.left) * fraction;

        ID2D1SolidColorBrush* fillBrush = nullptr;
        // Muted draws NO fill even at a non-zero level, so the state is visible in the bar itself and
        // not only in the glyph.
        if (!mutedRow && handleX > track.left + 0.5f) {
            // The slider's fill takes the theme's accent; the track and the handle stay neutral, so the
            // part that shows the LEVEL is the part that carries the colour.
            const D2D1_COLOR_F themeAccent = island::CurrentTheme().accent;
            const D2D1_COLOR_F accent =
                D2D1::ColorF(themeAccent.r, themeAccent.g, themeAccent.b, themeAccent.a * opacity);
            if ((fillBrush = BrushFor(context, accent)) != nullptr) {
                const D2D1_ROUNDED_RECT rounded = {
                    D2D1::RectF(track.left, centreY - barHeight * 0.5f, handleX,
                                centreY + barHeight * 0.5f),
                    radius, radius};
                context->FillRoundedRectangle(&rounded, fillBrush);
            }
        }

        ID2D1SolidColorBrush* handleBrush = nullptr;
        const D2D1_COLOR_F handleColour = PillInk(0.96f * opacity);
        if ((handleBrush = BrushFor(context, handleColour)) != nullptr) {
            // The handle is a touch larger while its row is the one being used (see sliderEmphasis): the
            // change is 2 DIP on the radius, which reads as "this row" without the track moving at all.
            const float emphasis = (i >= 0 && i < 2) ? text.sliderEmphasis[i] : 0.0f;
            const float grown = 6.5f +
                                2.0f * (emphasis < 0.0f ? 0.0f : (emphasis > 1.0f ? 1.0f : emphasis));
            const D2D1_ELLIPSE handle =
                D2D1::Ellipse(D2D1::Point2F(handleX, centreY), grown, grown);
            context->FillEllipse(&handle, handleBrush);
        }
    }

    // The output-device button at the right of the volume row: the mark, then the chevron that says it
    // leads somewhere. It is the shell's own arrangement, and it is the door into the 声音输出 page.
    if (layout.sliderCount > 0 && layout.volumeButton.right > layout.volumeButton.left) {
        const float centreY = (layout.volumeButton.top + layout.volumeButton.bottom) * 0.5f;
        const float markSide = 20.0f;   // the mark's own square; see DrawOutputDeviceMark
        const RectF mark = {layout.volumeButton.left, centreY - markSide * 0.5f,
                            layout.volumeButton.left + markSide, centreY + markSide * 0.5f};
        DrawOutputDeviceMark(context, mark, opacity);

        if (gearGlyphFormat_) {
            ID2D1SolidColorBrush* chevronBrush = nullptr;
            const D2D1_COLOR_F ink = PillInk(0.72f * opacity);
            if (SUCCEEDED(context->CreateSolidColorBrush(ink, &chevronBrush)) && chevronBrush) {
                const float side = 13.0f;
                const D2D1_RECT_F area = D2D1::RectF(
                    layout.volumeButton.right - side - 1.0f, centreY - side * 0.5f,
                    layout.volumeButton.right - 1.0f, centreY + side * 0.5f);
                const wchar_t chevron[] = {L'\uE76C', L'\0'};   // ChevronRight
                context->DrawTextW(chevron, 1, gearGlyphFormat_, area, chevronBrush,
                                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
                chevronBrush->Release();
            }
        }
    }

    // The reading as a percentage, while that slider is the thing in hand.
    //
    // The handle's position is the only reading the slider itself carries, and "a bit over a third of the
    // way along" is not an answer to "how loud is it". The shell prints the number while the control is
    // being used and takes it away again afterwards, which is what the two `show*Percent` flags mirror.
    for (int i = 0; i < layout.sliderCount; ++i) {
        const bool brightnessRow = i == 1;
        if (!(brightnessRow ? text.showBrightnessPercent : text.showVolumePercent) ||
            !volumeFormat_) {
            continue;
        }

        const RectF& track = layout.sliders[i];
        float fraction = brightnessRow ? text.brightness : text.volume;
        if (fraction < 0.0f) fraction = 0.0f;
        if (fraction > 1.0f) fraction = 1.0f;
        if (!brightnessRow && text.muted) fraction = 0.0f;
        const float handleX = track.left + (track.right - track.left) * fraction;

        wchar_t reading[16] = {};
        std::swprintf(reading, 16, L"%d%%", static_cast<int>(fraction * 100.0f + 0.5f));

        ID2D1SolidColorBrush* numberBrush = nullptr;
        const D2D1_COLOR_F ink = PillInk(0.94f * opacity);
        if (SUCCEEDED(context->CreateSolidColorBrush(ink, &numberBrush)) && numberBrush) {
            // The band BETWEEN the slider row and the grid above it, which the layout leaves empty, and a
            // 11 DIP face so the digits fit inside it. The box is the band's full width around the handle
            // so the number follows the handle without ever running off the card at either end.
            const float half = 60.0f;
            const D2D1_RECT_F area = D2D1::RectF(handleX - half, track.top - 12.0f,
                                                 handleX + half, track.top);
            const DWRITE_PARAGRAPH_ALIGNMENT previous = volumeFormat_->GetParagraphAlignment();
            const DWRITE_TEXT_ALIGNMENT previousText = volumeFormat_->GetTextAlignment();
            volumeFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            volumeFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            context->DrawTextW(reading, static_cast<UINT32>(std::wcslen(reading)), volumeFormat_,
                               area, numberBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            volumeFormat_->SetParagraphAlignment(previous);
            volumeFormat_->SetTextAlignment(previousText);
            numberBrush->Release();
        }
    }

    // The settings button, bottom right.
    if (gearGlyphFormat_) {
        ID2D1SolidColorBrush* gearBrush = nullptr;
        const D2D1_COLOR_F ink = PillInk(0.80f * opacity);
        if (SUCCEEDED(context->CreateSolidColorBrush(ink, &gearBrush)) && gearBrush) {
            const D2D1_RECT_F area =
                D2D1::RectF(layout.gearButton.left, layout.gearButton.top, layout.gearButton.right,
                            layout.gearButton.bottom);
            const wchar_t gear[] = {IndicatorGlyph(IndicatorIcon::Settings, 0), L'\0'};
            context->DrawTextW(gear, 1, gearGlyphFormat_, area, gearBrush,
                               D2D1_DRAW_TEXT_OPTIONS_CLIP);
            gearBrush->Release();
        }
    }

    // The paging column LAST: the dots (the current one lit) and the up/down arrows, exactly where the layout
    // put them.
    //
    // Last and not first, which is where the first version of this drew them -- before the card's own fill, so
    // the fill painted straight over them and the panel's paging controls were invisible while being drawn.
    if (layout.pageDotCount > 0 && gearGlyphFormat_) {
        for (int i = 0; i < layout.pageDotCount; ++i) {
            ID2D1SolidColorBrush* dotBrush = nullptr;
            const float alpha = (i == layout.pageIndex ? 0.95f : 0.35f) * opacity;
            if (SUCCEEDED(context->CreateSolidColorBrush(PillInk(alpha),
                                                        &dotBrush)) &&
                dotBrush) {
                const D2D1_ELLIPSE dot = D2D1::Ellipse(
                    D2D1::Point2F((layout.pageDots[i].left + layout.pageDots[i].right) * 0.5f,
                                  (layout.pageDots[i].top + layout.pageDots[i].bottom) * 0.5f),
                    (layout.pageDots[i].right - layout.pageDots[i].left) * 0.5f,
                    (layout.pageDots[i].bottom - layout.pageDots[i].top) * 0.5f);
                context->FillEllipse(&dot, dotBrush);
                dotBrush->Release();
            }
        }
        ID2D1SolidColorBrush* arrowBrush = nullptr;
        const D2D1_COLOR_F arrowInk = PillInk(0.70f * opacity);
        if (SUCCEEDED(context->CreateSolidColorBrush(arrowInk, &arrowBrush)) && arrowBrush) {
            // ChevronUp / ChevronDown out of Segoe MDL2 Assets, the family the icons come from.
            const wchar_t up[] = {L'\uE70E', L'\0'};
            const wchar_t down[] = {L'\uE70D', L'\0'};
            const D2D1_RECT_F upArea = D2D1::RectF(layout.pageUp.left, layout.pageUp.top,
                                                   layout.pageUp.right, layout.pageUp.bottom);
            const D2D1_RECT_F downArea = D2D1::RectF(layout.pageDown.left, layout.pageDown.top,
                                                     layout.pageDown.right, layout.pageDown.bottom);
            context->DrawTextW(up, 1, gearGlyphFormat_, upArea, arrowBrush,
                               D2D1_DRAW_TEXT_OPTIONS_CLIP);
            context->DrawTextW(down, 1, gearGlyphFormat_, downArea, arrowBrush,
                               D2D1_DRAW_TEXT_OPTIONS_CLIP);
            arrowBrush->Release();
        }
    }
}

ID2D1Bitmap1* TextRenderer::SoundIconAt(ID2D1DeviceContext2* context, int slot,
                                        const unsigned char* png, size_t size,
                                        unsigned long long revision) {
    if (!context || slot < 0 || slot >= SoundPageText::kMaxMixRows) return nullptr;
    SoundIconSlot& cached = soundIcons_[slot];

    if (!png || size == 0) {
        // The row has no icon at all -- a session whose process could not be read, or the device's own row.
        // Whatever was cached for this slot belonged to a different row and must go.
        if (cached.bitmap) {
            SafeRelease(&cached.bitmap);
            cached.revision = 0;
        }
        return nullptr;
    }
    if (cached.bitmap && cached.revision == revision) return cached.bitmap;

    DecodedImage decoded;
    if (!island::DecodeImage(png, size, &decoded) || !decoded.pixels || decoded.width <= 0 ||
        decoded.height <= 0) {
        return cached.bitmap;
    }

    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    const UINT32 width = static_cast<UINT32>(decoded.width);
    const UINT32 height = static_cast<UINT32>(decoded.height);
    ID2D1Bitmap1* bitmap = nullptr;
    const HRESULT hr = context->CreateBitmap(D2D1::SizeU(width, height), decoded.pixels,
                                             width * 4, &properties, &bitmap);
    if (FAILED(hr) || !bitmap) return cached.bitmap;

    SafeRelease(&cached.bitmap);
    cached.bitmap = bitmap;
    cached.revision = revision;
    return cached.bitmap;
}

void TextRenderer::DrawSoundPage(ID2D1DeviceContext2* context, const RoundedBox& box,
                                 const SoundPageText& text, float opacity) {
    if (!context || !bodyFormat_ || !titleFormat_ || !gearGlyphFormat_) return;
    if (text.mixCount < 0) return;

    const SoundPageLayout layout = SoundPageFor(box, text.deviceCount, text.mixCount, text.scroll);
    FillCardSurface(context, box, opacity);

    const auto brushFor = [&](float alpha, ID2D1SolidColorBrush** brush) {
        *brush = nullptr;
        return SUCCEEDED(context->CreateSolidColorBrush(
                   D2D1::ColorF(0.94f, 0.95f, 0.98f, alpha * opacity), brush)) &&
               *brush != nullptr;
    };

    // --- the frame: the two hairlines and the back arrow ---
    {
        ID2D1SolidColorBrush* rule = nullptr;
        if (brushFor(0.10f, &rule)) {
            context->DrawLine(D2D1::Point2F(layout.headerRule.left, layout.headerRule.top),
                              D2D1::Point2F(layout.headerRule.right, layout.headerRule.top), rule,
                              1.0f);
            context->DrawLine(D2D1::Point2F(layout.footerRule.left, layout.footerRule.top),
                              D2D1::Point2F(layout.footerRule.right, layout.footerRule.top), rule,
                              1.0f);
            rule->Release();
        }
    }

    // The back arrow: a glyph, not strokes, because this one IS in the icon face and its shape is not
    // something to approximate -- it is the same mark the shell's own page shows.
    if (layout.backButton.right > layout.backButton.left) {
        ID2D1SolidColorBrush* ink = nullptr;
        if (brushFor(0.92f, &ink)) {
            const wchar_t arrow[] = {L'\uE72B', L'\0'};   // Back
            const D2D1_RECT_F area =
                D2D1::RectF(layout.backButton.left, layout.backButton.top, layout.backButton.right,
                            layout.backButton.bottom);
            context->DrawTextW(arrow, 1, gearGlyphFormat_, area, ink, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            ink->Release();
        }
    }

    if (titleFormat_) {
        ID2D1SolidColorBrush* ink = nullptr;
        if (brushFor(0.95f, &ink)) {
            DrawTextIn(context, titleFormat_, text.title, layout.title, ink,
                       DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            ink->Release();
        }
    }

    // The footer, which does not scroll: it is the page's frame, and it is the way out to the settings
    // page when the control wanted is not on this one.
    if (bodyFormat_) {
        ID2D1SolidColorBrush* ink = nullptr;
        if (brushFor(0.85f, &ink)) {
            const RectF row = {layout.footer.left + kSoundIndentDip, layout.footer.top,
                               layout.footer.right - kSoundIndentDip, layout.footer.bottom};
            DrawTextIn(context, bodyFormat_, text.footer, row, ink, DWRITE_TEXT_ALIGNMENT_LEADING,
                       DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            ink->Release();
        }
    }

    // --- the content, clipped to its viewport ---
    //
    // A layer rather than nothing at all: the rows are laid out at their scrolled positions, so the ones
    // above the viewport would otherwise be painted over the header -- and a row that is not on screen must
    // not be readable there either. The round corners of the card are not cut, because the clip is inside
    // them.
    ID2D1RectangleGeometry* clip = nullptr;
    ID2D1Factory* factory = nullptr;
    context->GetFactory(&factory);
    if (factory) {
        factory->CreateRectangleGeometry(
            D2D1::RectF(layout.viewport.left, layout.viewport.top, layout.viewport.right,
                        layout.viewport.bottom),
            &clip);
    }
    if (clip) {
        context->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), clip,
                                                  D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                           nullptr);
    }

    const auto sectionLabel = [&](const wchar_t* label, const RectF& where) {
        ID2D1SolidColorBrush* ink = nullptr;
        if (!brushFor(0.88f, &ink)) return;
        DrawTextIn(context, bodyFormat_, label, where, ink, DWRITE_TEXT_ALIGNMENT_LEADING,
                   DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        ink->Release();
    };

    // The device list.
    sectionLabel(text.deviceLabel, layout.deviceLabel);
    for (int i = 0; i < layout.deviceCount; ++i) {
        const SoundPageText::DeviceRow& row = text.devices[i];

        // The row in use is marked the way the shell marks it: an accent bar against the card's left edge
        // and a slightly lighter row. A list of identical rows with no answer to "which one is playing"
        // would leave the one question the page exists to answer to the user's memory.
        if (row.isDefault) {
            ID2D1SolidColorBrush* wash = nullptr;
            if (brushFor(0.07f, &wash)) {
                const RectF& r = layout.deviceRows[i];
                context->FillRectangle(D2D1::RectF(r.left, r.top, r.right, r.bottom), wash);
                wash->Release();
            }
            ID2D1SolidColorBrush* accent = nullptr;
            if (SUCCEEDED(context->CreateSolidColorBrush(
                    D2D1::ColorF(0.36f, 0.70f, 0.98f, 0.95f * opacity), &accent)) &&
                accent) {
                const RectF& mark = layout.deviceMarks[i];
                const float radius = (mark.right - mark.left) * 0.5f;
                context->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(mark.left, mark.top, mark.right, mark.bottom),
                                      radius, radius),
                    accent);
                accent->Release();
            }
        }

        // The glyph, chosen from the device's form factor: a screen for audio that comes out of one, a
        // headset for headphones, a speaker for everything else.
        const wchar_t glyph = DeviceGlyphFor(row.formFactor);
        if (glyph != L'\0') {
            ID2D1SolidColorBrush* ink = nullptr;
            if (brushFor(0.92f, &ink)) {
                const RectF& icon = layout.deviceIcons[i];
                const D2D1_RECT_F area =
                    D2D1::RectF(icon.left, icon.top, icon.right, icon.bottom);
                context->DrawTextW(&glyph, 1, gearGlyphFormat_, area, ink,
                                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
                ink->Release();
            }
        }
        if (row.name && row.name[0]) {
            ID2D1SolidColorBrush* ink = nullptr;
            if (brushFor(row.isDefault ? 0.97f : 0.80f, &ink)) {
                DrawTextIn(context, bodyFormat_, row.name, layout.deviceLabels[i], ink,
                           DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                ink->Release();
            }
        }
    }

    // The spatial sound rows.
    sectionLabel(text.spatialLabel, layout.spatialLabel);
    for (int i = 0; i < kSoundSpatialRows; ++i) {
        const SoundPageText::SpatialRow& row = text.spatial[i];
        if (!row.label || !row.label[0]) continue;
        if (row.selected) {
            ID2D1SolidColorBrush* accent = nullptr;
            if (SUCCEEDED(context->CreateSolidColorBrush(
                    D2D1::ColorF(0.36f, 0.70f, 0.98f, 0.95f * opacity), &accent)) &&
                accent) {
                const RectF& mark = layout.spatialMarks[i];
                const float radius = (mark.right - mark.left) * 0.5f;
                context->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(mark.left, mark.top, mark.right, mark.bottom),
                                      radius, radius),
                    accent);
                accent->Release();
            }
        }
        ID2D1SolidColorBrush* ink = nullptr;
        if (brushFor(row.selected ? 0.97f : 0.80f, &ink)) {
            DrawTextIn(context, bodyFormat_, row.label, layout.spatialLabels[i], ink,
                       DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            ink->Release();
        }
    }

    // The mixer.
    sectionLabel(text.mixerLabel, layout.mixerLabel);
    if (layout.mixerGear.right > layout.mixerGear.left) {
        ID2D1SolidColorBrush* ink = nullptr;
        if (brushFor(0.85f, &ink)) {
            const wchar_t gear[] = {IndicatorGlyph(IndicatorIcon::Settings, 0), L'\0'};
            const RectF& where = layout.mixerGear;
            context->DrawTextW(gear, 1, gearGlyphFormat_,
                               D2D1::RectF(where.left, where.top, where.right, where.bottom), ink,
                               D2D1_DRAW_TEXT_OPTIONS_CLIP);
            ink->Release();
        }
    }
    for (int i = 0; i < layout.sessionCount; ++i) {
        const SoundPageText::MixRow& row = text.mix[i];
        const RectF& iconBox = layout.mixerIcons[i];
        const float iconSide = iconBox.right - iconBox.left;

        // The application's own icon, which is what makes the row recognisable at a glance -- the shell's
        // mixer shows the icon and nothing else. The first row is the output device and has no icon, so it
        // takes the speaker glyph.
        ID2D1Bitmap1* bitmap =
            SoundIconAt(context, i, row.iconPng, row.iconSize, row.iconRevision);
        if (bitmap) {
            context->DrawBitmap(bitmap,
                                D2D1::RectF(iconBox.left, iconBox.top, iconBox.right, iconBox.bottom),
                                opacity, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        } else if (gearGlyphFormat_) {
            ID2D1SolidColorBrush* ink = nullptr;
            if (brushFor(0.92f, &ink)) {
                const wchar_t glyph =
                    i == 0 ? IndicatorGlyph(IndicatorIcon::Volume, 1)
                           : IndicatorGlyph(IndicatorIcon::Settings, 0);
                context->DrawTextW(&glyph, 1, gearGlyphFormat_,
                                   D2D1::RectF(iconBox.left, iconBox.top, iconBox.right,
                                               iconBox.bottom),
                                   ink, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                ink->Release();
            }
        }
        (void)iconSide;

        // The slider: track, then the filled part, then the handle -- drawn in that order so the fill covers
        // the track and the handle covers the fill. The same arrangement the panel's own slider uses, so the
        // two read as one control in two places.
        const RectF& track = layout.mixerTracks[i];
        const float centreY = (track.top + track.bottom) * 0.5f;
        const float barHeight = 4.0f;
        const float radius = barHeight * 0.5f;
        ID2D1SolidColorBrush* trackBrush = nullptr;
        if (brushFor(0.22f, &trackBrush)) {
            context->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(track.left, centreY - barHeight * 0.5f, track.right,
                                              centreY + barHeight * 0.5f),
                                  radius, radius),
                trackBrush);
            trackBrush->Release();
        }

        float fraction = row.muted ? 0.0f : row.volume;
        if (fraction < 0.0f) fraction = 0.0f;
        if (fraction > 1.0f) fraction = 1.0f;
        const float handleX = track.left + (track.right - track.left) * fraction;
        if (fraction > 0.001f) {
            ID2D1SolidColorBrush* accent = nullptr;
            if (SUCCEEDED(context->CreateSolidColorBrush(
                    D2D1::ColorF(0.36f, 0.70f, 0.98f, 0.95f * opacity), &accent)) &&
                accent) {
                context->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(track.left, centreY - barHeight * 0.5f, handleX,
                                                  centreY + barHeight * 0.5f),
                                      radius, radius),
                    accent);
                accent->Release();
            }
        }
        ID2D1SolidColorBrush* handle = nullptr;
        if (brushFor(0.96f, &handle)) {
            context->FillEllipse(D2D1::Ellipse(D2D1::Point2F(handleX, centreY), 6.0f, 6.0f), handle);
            handle->Release();
        }
    }

    if (clip) {
        context->PopLayer();
        clip->Release();
    }

    // The scrollbar, last: it belongs over the content, and it is what says the page continues below.
    {
        ID2D1SolidColorBrush* trackBrush = nullptr;
        if (brushFor(0.10f, &trackBrush)) {
            const RectF& bar = layout.scrollTrack;
            context->FillRoundedRectangle(
                D2D1::RoundedRect(
                    D2D1::RectF(bar.left, bar.top, bar.right, bar.bottom),
                    (bar.right - bar.left) * 0.5f, (bar.right - bar.left) * 0.5f),
                trackBrush);
            trackBrush->Release();
        }
        ID2D1SolidColorBrush* thumbBrush = nullptr;
        if (brushFor(0.34f, &thumbBrush)) {
            const RectF& thumb = layout.scrollThumb;
            context->FillRoundedRectangle(
                D2D1::RoundedRect(
                    D2D1::RectF(thumb.left, thumb.top, thumb.right, thumb.bottom),
                    (thumb.right - thumb.left) * 0.5f, (thumb.right - thumb.left) * 0.5f),
                thumbBrush);
            thumbBrush->Release();
        }
    }
}

void TextRenderer::DrawStartButton(ID2D1DeviceContext2* context, const RoundedBox& island,
                                   float wash, bool open, const D2D1_COLOR_F& fill, float opacity) {
    // Reported on the FIRST call and on every change, so "never called" and "called and invisible" can
    // be told apart. A screenshot cannot separate those, and the difference decides whether to look at
    // the layout or at the drawing.
    if (!startTraced_ || static_cast<int>(island.rect.left) != startTraceLeft_ ||
        fill.a != startTraceAlpha_ || opacity != startTraceOpacity_) {
        startTraced_ = true;
        startTraceLeft_ = static_cast<int>(island.rect.left);
        startTraceAlpha_ = fill.a;
        startTraceOpacity_ = opacity;
        const RectF box = StartButtonFor(island);
        LogLine(L"  start button: island x [%.0f, %.0f] -> button x [%.0f, %.0f] y [%.0f, %.0f], "
                L"fill.a %.2f, opacity %.2f, context %d",
                island.rect.left, island.rect.right, box.left, box.right, box.top, box.bottom,
                fill.a, opacity, context ? 1 : 0);
    }

    if (!context) return;

    const RectF box = StartButtonFor(island);
    if (box.right <= box.left) return;

    // The same background the icon buttons get, in the island's own colour, so the two ends of the bar
    // read as one object. Without it the Windows mark would sit on whatever the wallpaper happens to be,
    // which is the problem the icon buttons already had.
    FillStadiumBackground(context, box, fill);

    // The pointer on it gets the same wash the active window's button gets, so "this is under the
    // pointer" looks the same wherever it happens on the bar.
    // ... AND IT COMES UP OVER ABOUT A TENTH OF A SECOND, not in one frame: the amount is the frame
    // loop's spring (FrameState::startWash). A wash that switches on reads as a hard edge, which is the
    // opposite of what a hover should feel like.
    // The wash, with the same two cases the tray button has: the pointer's own amount, or a full one when
    // this is the control whose card is up.
    const float strength = open ? 1.0f : wash;
    if (strength > 0.01f) {
        const float radius = (box.bottom - box.top) * 0.5f;
        const D2D1_ROUNDED_RECT rounded = {
            D2D1::RectF(box.left, box.top, box.right, box.bottom), radius, radius};
        ID2D1SolidColorBrush* washBrush = nullptr;
        const float amount = open ? 0.16f : 0.16f * strength;
        const D2D1_COLOR_F colour = PillInk(amount * opacity);
        if (SUCCEEDED(context->CreateSolidColorBrush(colour, &washBrush)) && washBrush) {
            context->FillRoundedRectangle(&rounded, washBrush);
            washBrush->Release();
        }
    }

    // The mark, drawn rather than loaded.
    //
    // Four panes in a 2x2 grid, which is the Windows logo at this size. Drawn as geometry on purpose:
    // an icon file would be one more thing to ship, one more thing to fail to load, and this shape
    // cannot go missing.
    {
        ID2D1SolidColorBrush* mark = nullptr;
        const D2D1_COLOR_F colour = PillInk(0.95f * opacity);
        if (SUCCEEDED(context->CreateSolidColorBrush(colour, &mark)) && mark) {
            const float side = kStartIconDip;
            const float centreX = (box.left + box.right) * 0.5f;
            const float centreY = (box.top + box.bottom) * 0.5f;
            const float pane = (side - 3.0f) * 0.5f;   // 3 DIP of gap between the panes
            const float left = centreX - side * 0.5f;
            const float top = centreY - side * 0.5f;

            const D2D1_RECT_F panes[4] = {
                D2D1::RectF(left, top, left + pane, top + pane),
                D2D1::RectF(left + pane + 3.0f, top, left + side, top + pane),
                D2D1::RectF(left, top + pane + 3.0f, left + pane, top + side),
                D2D1::RectF(left + pane + 3.0f, top + pane + 3.0f, left + side, top + side),
            };
            for (const D2D1_RECT_F& quad : panes) {
                context->FillRectangle(&quad, mark);
            }
            mark->Release();
        }
    }
}

void TextRenderer::CurrentSecond(int* second) {    if (!second) return;
    Refresh();
    *second = static_cast<int>(time_.wSecond);
}

void TextRenderer::CurrentTimeText(wchar_t* out, size_t capacity) const {
    if (!out || capacity == 0) return;
    if (timeText_[0] == L'\0') {
        out[0] = L'\0';
        return;
    }
    wcsncpy_s(out, capacity, timeText_, _TRUNCATE);
}

ID2D1Bitmap1* TextRenderer::StartBitmapFor(ID2D1DeviceContext2* context, StartBitmap* slot,
                                           const uint8_t* png, size_t size) {
    if (!context || !slot || !png || size == 0) return nullptr;
    if (slot->bitmap && slot->png == png && slot->size == size) return slot->bitmap;

    ID2D1Bitmap1* bitmap = nullptr;
    DecodedImage decoded;
    if (DecodeImage(png, size, &decoded) && decoded.pixels) {
        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        const UINT32 stride = static_cast<UINT32>(decoded.width) * 4;
        context->CreateBitmap(
            D2D1::SizeU(static_cast<UINT32>(decoded.width), static_cast<UINT32>(decoded.height)),
            decoded.pixels, stride, &properties, &bitmap);
    }
    SafeRelease(&slot->bitmap);
    slot->bitmap = bitmap;
    slot->png = png;
    slot->size = size;
    return bitmap;
}

void TextRenderer::DrawStartPage(ID2D1DeviceContext2* context, const RoundedBox& card,
                                 const StartCardText& text, float opacity) {
    if (!context || opacity <= 0.01f) return;
    const island::StartPageLayout layout = island::StartPageFor(card);
    if (!layout.valid) return;

    FillCardSurface(context, card, opacity);

    const D2D1_COLOR_F ink = PillInk(0.92f * opacity);
    const D2D1_COLOR_F dim = PillInk(0.50f * opacity);
    const D2D1_COLOR_F warm = D2D1::ColorF(1.0f, 0.72f, 0.35f, 0.95f * opacity);

    ID2D1SolidColorBrush* inkBrush = nullptr;
    ID2D1SolidColorBrush* dimBrush = nullptr;
    ID2D1SolidColorBrush* warmBrush = nullptr;
    ID2D1SolidColorBrush* washBrush = nullptr;
    ID2D1SolidColorBrush* lineBrush = nullptr;
    context->CreateSolidColorBrush(ink, &inkBrush);
    context->CreateSolidColorBrush(dim, &dimBrush);
    context->CreateSolidColorBrush(warm, &warmBrush);
    context->CreateSolidColorBrush(PillInk(0.10f * opacity), &washBrush);
    context->CreateSolidColorBrush(PillInk(0.07f * opacity), &lineBrush);

    const auto fillRounded = [&](const RectF& rect, float radius, ID2D1SolidColorBrush* brush) {
        if (!brush) return;
        const D2D1_ROUNDED_RECT rounded = {
            D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom), radius, radius};
        context->FillRoundedRectangle(&rounded, brush);
    };
    // The highlight for one item: its own animated amount when the caller supplied them, and a hard 0/1
    // otherwise (which keeps every other caller -- and the drawings the self-test makes -- unchanged).
    const auto washAt = [&](const float* amounts, int index, int hovered) {
        if (index < 0) return 0.0f;
        if (amounts) return amounts[index];
        return index == hovered ? 1.0f : 0.0f;
    };
    // A wash brush at a given strength, made per draw because the strength varies per item. Cheap: two
    // fields, no geometry, and there are at most a handful of items on screen with a non-zero amount.
    const auto fillWash = [&](const RectF& rect, float radius, float amount) {
        if (!washBrush || amount <= 0.01f) return;
        ID2D1SolidColorBrush* brush = nullptr;
        if (SUCCEEDED(context->CreateSolidColorBrush(
                PillInk(0.12f * amount * opacity), &brush)) &&
            brush) {
            const D2D1_ROUNDED_RECT rounded = {
                D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom), radius, radius};
            context->FillRoundedRectangle(&rounded, brush);
            brush->Release();
        }
    };
    const auto drawGlyph = [&](const wchar_t* glyph, const RectF& rect, ID2D1Brush* brush,
                               IDWriteTextFormat* format) {
        if (!brush || !format) return;
        const D2D1_RECT_F target = D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom);
        context->DrawTextW(glyph, 1, format, target, brush);
    };

    if (text.page == 2) {
        // --- the power list, standing above the button it came from --------------------------------
        static const wchar_t* const kLabels[5] = {L"锁定", L"睡眠", L"注销", L"重启", L"关机"};
        for (int i = 0; i < 5; ++i) {
            const RectF row = island::StartPowerRowFor(card, i);
            if (row.right <= row.left) continue;
            fillWash(row, 8.0f, washAt(text.powerWashes, i, text.hoveredPowerRow));
            // The three that lose work are ARMED rather than acted on: a mis-click on a list that appears
            // above a button the user was already reaching for must not sign him out.
            const bool armed = (text.powerConfirmRow == i);
            const wchar_t* label = armed ? L"再点一次确认" : kLabels[i];
            ID2D1SolidColorBrush* brush = armed ? warmBrush : inkBrush;
            if (brush && startRowFormat_) {
                const D2D1_RECT_F target = D2D1::RectF(row.left + 10.0f, row.top, row.right - 10.0f,
                                                       row.bottom);
                context->DrawTextW(label, static_cast<UINT32>(std::wcslen(label)), startRowFormat_,
                                   target, brush);
            }
        }
    } else if (text.page == 1) {
        // --- the picker: every application the shell offers, one per row ---------------------------
        const int rowsOnScreen =
            static_cast<int>((layout.grid.bottom - layout.grid.top) / island::kStartRowHeight);
        for (int visible = 0; visible <= rowsOnScreen; ++visible) {
            const int index = text.pickerScrollRow + visible;
            if (index < 0 || index >= text.listCount) break;
            const RectF row = island::StartPickerRowFor(card, visible);
            if (row.bottom > layout.grid.bottom) break;
            fillWash(row, 8.0f, washAt(text.rowWashes, index, text.hoveredPickerRow));
            const island::StartApp& app = text.list[index];
            if (startRowFormat_ && inkBrush) {
                const D2D1_RECT_F target =
                    D2D1::RectF(row.left + 10.0f, row.top, row.right - 84.0f, row.bottom);
                context->DrawTextW(app.name, static_cast<UINT32>(std::wcslen(app.name)),
                                   startRowFormat_, target, inkBrush);
            }
            // Already pinned: said on the row rather than left to be discovered by pinning it twice.
            if (dimBrush && startLabelFormat_) {
                const wchar_t* mark = L"";
                for (int p = 0; p < text.pinCount; ++p) {
                    if (text.pins && _wcsicmp(text.pins[p].target, app.target) == 0) {
                        mark = L"已固定";
                        break;
                    }
                }
                if (mark[0] != L'\0') {
                    const D2D1_RECT_F target =
                        D2D1::RectF(row.right - 74.0f, row.top, row.right - 10.0f, row.bottom);
                    context->DrawTextW(mark, static_cast<UINT32>(std::wcslen(mark)), startLabelFormat_,
                                       target, dimBrush);
                }
            }
        }
        // A scroll bar on the right of the list, so "there is more" is visible without trying the wheel.
        if (text.listCount > 0 && lineBrush) {
            const float gridHeight = layout.grid.bottom - layout.grid.top;
            const float visibleFraction =
                static_cast<float>(rowsOnScreen + 1) / static_cast<float>(text.listCount);
            const float barHeight = std::max(24.0f, gridHeight * visibleFraction);
            const float maxRow =
                static_cast<float>(std::max(1, text.listCount - (rowsOnScreen + 1)));
            const float travelled =
                layout.grid.top + (gridHeight - barHeight) * (static_cast<float>(text.pickerScrollRow) /
                                                              maxRow);
            const RectF bar = {layout.grid.right + 6.0f, travelled, layout.grid.right + 9.0f,
                               travelled + barHeight};
            fillRounded(bar, 1.5f, lineBrush);
        }
    } else {
        // --- the pins, which start empty -----------------------------------------------------------
        const int capacity = island::StartPinCapacity();
        const int shown = text.pinCount < capacity ? text.pinCount : capacity;
        for (int i = 0; i < shown; ++i) {
            const RectF cell = island::StartPinCellFor(layout, i);
            fillWash(cell, 10.0f, washAt(text.pinWashes, i, text.hoveredPin));

            const uint8_t* png = (text.pinIcons && i < static_cast<int>(sizeof(startPinBitmaps_) /
                                                                       sizeof(startPinBitmaps_[0])))
                                     ? text.pinIcons[i]
                                     : nullptr;
            const size_t pngSize = text.pinIconSizes ? text.pinIconSizes[i] : 0;
            ID2D1Bitmap1* icon =
                StartBitmapFor(context, &startPinBitmaps_[i], png, pngSize);
            // 28 in a 50 DIP cell, with the label under it: the cell has to hold a picture and one line of
            // 10.5 pixel text, and three of them plus gaps have to fit the grid (see kStartCellHeight).
            const float iconSide = 28.0f;
            const float centreX = (cell.left + cell.right) * 0.5f;
            const float iconTop = cell.top + 5.0f;
            if (icon && washBrush) {
                const D2D1_RECT_F target =
                    D2D1::RectF(centreX - iconSide * 0.5f, iconTop, centreX + iconSide * 0.5f,
                                iconTop + iconSide);
                context->DrawBitmap(icon, &target, opacity,
                                    D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            } else if (washBrush) {
                // No icon YET (it is resolved on a worker) or none at all: a quiet square holds the place,
                // so a pin never appears as a name floating in space.
                const RectF placeholder = {centreX - iconSide * 0.5f, iconTop, centreX + iconSide * 0.5f,
                                           iconTop + iconSide};
                fillRounded(placeholder, 6.0f, lineBrush);
            }
            if (startLabelFormat_ && inkBrush && text.pins) {
                const D2D1_RECT_F target =
                    D2D1::RectF(cell.left + 2.0f, iconTop + iconSide + 1.0f, cell.right - 2.0f,
                                cell.bottom);
                context->DrawTextW(text.pins[i].name,
                                   static_cast<UINT32>(std::wcslen(text.pins[i].name)),
                                   startLabelFormat_, target, inkBrush);
            }
        }
        if (shown == 0) {
            // THE EMPTY CARD, which is what the user asked for: nothing pinned yet, and the whole upper
            // area is the thing to press to change that.
            const float centreX = (layout.grid.left + layout.grid.right) * 0.5f;
            const float centreY = (layout.grid.top + layout.grid.bottom) * 0.5f;
            if (warmBrush) {
                const RectF plus = {centreX - 12.0f, centreY - 34.0f, centreX + 12.0f, centreY - 10.0f};
                drawGlyph(L"+", plus, warmBrush, glyphFormat_);
            }
            // CENTRED, with the label format: the hint belongs to the middle of the area it is inviting the
            // user to use. Written first with the row format, which is left-aligned -- the text then started
            // at the grid's left edge and ran off the side of the card, which the second photograph of this
            // card showed as one character at the card's edge.
            if (dimBrush && startLabelFormat_) {
                const wchar_t* hint = L"点这里添加应用";
                const D2D1_RECT_F target =
                    D2D1::RectF(layout.grid.left, centreY - 4.0f, layout.grid.right, centreY + 24.0f);
                context->DrawTextW(hint, static_cast<UINT32>(std::wcslen(hint)), startLabelFormat_, target,
                                   dimBrush);
            }
        }
    }

    // --- the footer: the account picture, the name, and the power button --------------------------
    if (lineBrush) {
        const RectF hair = {layout.footer.left, layout.footer.top, layout.footer.right,
                            layout.footer.top + 1.0f};
        context->FillRectangle(D2D1::RectF(hair.left, hair.top, hair.right, hair.bottom), lineBrush);
    }
    if (text.page == 1) {
        // While picking, the footer says what picking means and offers the way out, and the account
        // picture steps aside: the card has one line at the bottom and this is the more useful one.
        if (dimBrush && startRowFormat_) {
            wchar_t hint[128] = {};
            std::swprintf(hint, 128, L"全部应用 %d 个 · 滚轮翻页 · 点一个固定", text.listCount);
            const D2D1_RECT_F target = D2D1::RectF(layout.footer.left, layout.footer.top,
                                                   layout.footer.right - 40.0f, layout.footer.bottom);
            context->DrawTextW(hint, static_cast<UINT32>(std::wcslen(hint)), startRowFormat_, target,
                               dimBrush);
        }
    } else {
        ID2D1Bitmap1* avatar =
            StartBitmapFor(context, &startAvatarBitmap_, text.avatarPng, text.avatarPngSize);
        if (avatar) {
            // Round, like the shell's own footer, and for the same reason the notification's avatar is
            // round: a square picture beside a name reads as two unrelated things.
            ID2D1Factory* factory = nullptr;
            context->GetFactory(&factory);
            ID2D1EllipseGeometry* circle = nullptr;
            if (factory) {
                factory->CreateEllipseGeometry(
                    D2D1::Ellipse(D2D1::Point2F((layout.avatar.left + layout.avatar.right) * 0.5f,
                                                (layout.avatar.top + layout.avatar.bottom) * 0.5f),
                                  (layout.avatar.right - layout.avatar.left) * 0.5f,
                                  (layout.avatar.bottom - layout.avatar.top) * 0.5f),
                    &circle);
            }
            if (circle) {
                context->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), circle,
                                                          D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                                   nullptr);
            }
            const D2D1_RECT_F target =
                D2D1::RectF(layout.avatar.left, layout.avatar.top, layout.avatar.right, layout.avatar.bottom);
            context->DrawBitmap(avatar, &target, opacity,
                                D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            if (circle) {
                context->PopLayer();
                circle->Release();
            }
            if (factory) factory->Release();
        } else if (lineBrush) {
            const D2D1_ROUNDED_RECT rounded = {
                D2D1::RectF(layout.avatar.left, layout.avatar.top, layout.avatar.right, layout.avatar.bottom),
                (layout.avatar.right - layout.avatar.left) * 0.5f,
                (layout.avatar.bottom - layout.avatar.top) * 0.5f};
            context->FillRoundedRectangle(&rounded, lineBrush);
        }
        if (startRowFormat_ && inkBrush && text.accountName && text.accountName[0] != L'\0') {
            const D2D1_RECT_F target =
                D2D1::RectF(layout.avatar.right + 10.0f, layout.footer.top,
                            layout.powerButton.left - 10.0f, layout.footer.bottom);
            context->DrawTextW(text.accountName, static_cast<UINT32>(std::wcslen(text.accountName)),
                               startRowFormat_, target, inkBrush);
        }
    }

    // The power button: a wash while the pointer is on it (or while its own list is up), and the power
    // rune -- the glyph the shell's own Start uses, from the icon font the bar already draws with. With the
    // picker up the same slot is the way out, so it draws a multiplication sign instead.
    {
        if (washBrush && (text.powerButtonHovered || text.page == 2)) {
            fillRounded(layout.powerButton, 8.0f, washBrush);
        }
        if (text.page == 1) {
            // The way out of the picker: a multiplication sign, in the text font, where the power button
            // is. Two marks in one place, chosen by the page -- which is what the slot means.
            drawGlyph(L"×", layout.powerButton, inkBrush, startRowFormat_);
        } else {
            // THE POWER MARK, DRAWN AS GEOMETRY, for the reason the Start button's mark is: no glyph to get
            // wrong and nothing to fail to load. That rule was earned here again -- the icon font's power
            // codepoint (E7E8) came out as an empty box on this machine, photographed, and a symbol that is
            // the whole meaning of a button cannot be a thing that might not be there.
            const float cx = (layout.powerButton.left + layout.powerButton.right) * 0.5f;
            const float cy = (layout.powerButton.top + layout.powerButton.bottom) * 0.5f;
            const float radius = 6.5f;
            ID2D1Factory* factory = nullptr;
            context->GetFactory(&factory);
            if (factory && dimBrush) {
                ID2D1EllipseGeometry* ring = nullptr;
                factory->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius),
                                               &ring);
                if (ring) {
                    // A ring with a gap at the top, and a stem through the gap: the power symbol. Drawn as
                    // an arc rather than a full circle so the gap is real rather than a second stroke in the
                    // card's own colour.
                    ID2D1PathGeometry* arc = nullptr;
                    ID2D1GeometrySink* sink = nullptr;
                    if (SUCCEEDED(factory->CreatePathGeometry(&arc)) && arc &&
                        SUCCEEDED(arc->Open(&sink)) && sink) {
                        const float start = -1.15f;   // radians, just right of the top
                        const float end = start + 5.42f;   // all the way round, back to just left of it
                        sink->BeginFigure(D2D1::Point2F(cx + radius * std::sin(start),
                                                        cy - radius * std::cos(start)),
                                          D2D1_FIGURE_BEGIN_HOLLOW);
                        const int steps = 24;
                        for (int i = 1; i <= steps; ++i) {
                            const float t = start + (end - start) * static_cast<float>(i) / steps;
                            sink->AddLine(D2D1::Point2F(cx + radius * std::sin(t),
                                                        cy - radius * std::cos(t)));
                        }
                        sink->EndFigure(D2D1_FIGURE_END_OPEN);
                        sink->Close();
                        sink->Release();
                        context->DrawGeometry(arc, dimBrush, 1.6f);
                    }
                    if (arc) arc->Release();
                    ring->Release();
                }
                context->DrawLine(D2D1::Point2F(cx, cy - radius - 1.5f), D2D1::Point2F(cx, cy - 1.0f),
                                  dimBrush, 1.8f);
            }
            if (factory) factory->Release();
        }
    }

    if (inkBrush) inkBrush->Release();
    if (dimBrush) dimBrush->Release();
    if (warmBrush) warmBrush->Release();
    if (washBrush) washBrush->Release();
    if (lineBrush) lineBrush->Release();
}

} // namespace island

