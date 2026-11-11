// =====================================================================
//  TextRenderer.cpp
// =====================================================================
#include "render/TextRenderer.h"
#include "render/Ambient.h"   // the card's sky: the same weather the island is showing
#include "render/Theme.h"   // the material: the card's surface, the edge, the accent and the ink
#include "render/GlassMaterial.h"   // the shadow under a card and the grain over it
#include "shell/Weather.h"   // SkyMark: the weather mark the status line draws, per WMO code

#include "media/WicImage.h"   // DecodeImage, for the mixer's application icons
#include "domain/NiriBarLayout.h"
#include "domain/TaskStripLayout.h"

// For the notification field capacities. TextRenderer itself stays free of the
// notification model; it only needs to know how large the buffers it is handed can
// be, and taking those from the model keeps the two from drifting apart.
#include "services/Notification.h"
#include "core/Log.h"

#include <algorithm>
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
    return island::CardInk(a);
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

    const auto makeNiriFormat = [this](float size, DWRITE_FONT_WEIGHT weight,
                                       DWRITE_TEXT_ALIGNMENT horizontal,
                                       IDWriteTextFormat** out) -> bool {
        const HRESULT created = factory_->CreateTextFormat(
            L"Segoe UI Variable Display", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", out);
        if (FAILED(created) || !*out) return false;
        (*out)->SetTextAlignment(horizontal);
        (*out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return true;
    };
    const auto makeCommandFormat = [this](float size, DWRITE_FONT_WEIGHT weight,
                                          DWRITE_TEXT_ALIGNMENT horizontal,
                                          IDWriteTextFormat** out) -> bool {
        const HRESULT created = factory_->CreateTextFormat(
            L"Noto Sans SC", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", out);
        if (FAILED(created) || !*out) return false;
        (*out)->SetTextAlignment(horizontal);
        (*out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return true;
    };
    makeNiriFormat(17.0f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING,
                   &niriClockFormat_);
    makeNiriFormat(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING,
                   &niriDateFormat_);
    makeNiriFormat(11.0f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER,
                   &niriLyricFormat_);
    makeNiriFormat(58.0f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING,
                   &niriHeroFormat_);
    makeNiriFormat(12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING,
                   &niriMetricFormat_);
    makeCommandFormat(44.0f, DWRITE_FONT_WEIGHT_EXTRA_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING,
                      &niriCommandTimeFormat_);
    makeCommandFormat(12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING,
                      &niriCommandDateFormat_);
    makeCommandFormat(11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING,
                      &niriCommandChipFormat_);
    makeCommandFormat(24.0f, DWRITE_FONT_WEIGHT_BLACK, DWRITE_TEXT_ALIGNMENT_CENTER,
                      &niriWeatherTempFormat_);
    makeCommandFormat(10.0f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER,
                      &niriWeatherQualityFormat_);
    makeCommandFormat(12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER,
                      &niriWeatherWordFormat_);
    makeCommandFormat(11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER,
                      &niriCalendarWeekdayFormat_);
    makeCommandFormat(11.0f, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER,
                      &niriCalendarDayFormat_);
    makeCommandFormat(11.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING,
                      &niriMediaSubtitleFormat_);
    makeCommandFormat(9.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING,
                      &niriMediaTimeFormat_);
    makeCommandFormat(14.0f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING,
                      &niriNotificationHeaderFormat_);
    makeCommandFormat(12.0f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING,
                      &niriNotificationAppFormat_);
    makeCommandFormat(12.0f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING,
                      &niriNotificationTitleFormat_);
    makeCommandFormat(11.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING,
                      &niriNotificationMetaFormat_);

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
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        12.0f, L"zh-CN", &volumeFormat_);
    if (SUCCEEDED(hr)) {
        volumeFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        volumeFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // The Start card's two faces.
    //
    // The label under a tile is centred, 11.5 DIP against the reference's 12, and one size smaller than a
    // list row: it is read at a glance under a 56 DIP picture, and it is the NAME of the thing above it
    // rather than a line of text to be read. `startRowFormat_` is left-aligned because every list in this
    // bar is.
    //
    // BOTH CUT WITH AN ELLIPSIS. A 76 DIP tile cannot hold "Application Verifier (X64)" or "Documentation
    // for..." -- with NO_WRAP and no trimming the words simply ran out of the tile and across its
    // neighbour, reported as "全部里面的 图标文字重叠了". The all-applications grid made that visible
    // because its names are the shell's own and often long.
    const auto trimWithEllipsis = [this](IDWriteTextFormat* format) {
        IDWriteInlineObject* ellipsis = nullptr;
        if (format &&
            SUCCEEDED(factory_->CreateEllipsisTrimmingSign(format, &ellipsis)) && ellipsis) {
            DWRITE_TRIMMING trimming = {};
            trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
            format->SetTrimming(&trimming, ellipsis);
            ellipsis->Release();
        }
    };
    hr = factory_->CreateTextFormat(
        L"Noto Sans SC", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.5f, L"zh-CN",
        &niriNotificationBodyFormat_);
    if (SUCCEEDED(hr) && niriNotificationBodyFormat_) {
        niriNotificationBodyFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        niriNotificationBodyFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        niriNotificationBodyFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        niriNotificationBodyFormat_->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,
                                                    14.0f, 11.0f);
        trimWithEllipsis(niriNotificationBodyFormat_);
    }
    hr = factory_->CreateTextFormat(
        L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 11.5f, L"zh-CN", &startLabelFormat_);
    if (SUCCEEDED(hr)) {
        startLabelFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        startLabelFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        startLabelFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        trimWithEllipsis(startLabelFormat_);
    }
    hr = factory_->CreateTextFormat(
        L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 12.5f, L"zh-CN", &startRowFormat_);
    if (SUCCEEDED(hr)) {
        startRowFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        startRowFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        startRowFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        trimWithEllipsis(startRowFormat_);
    }
    hr = factory_->CreateTextFormat(
        L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 10.5f, L"zh-CN", &startCaptionFormat_);
    if (SUCCEEDED(hr)) {
        startCaptionFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        startCaptionFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        startCaptionFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        trimWithEllipsis(startCaptionFormat_);
    }
    hr = factory_->CreateTextFormat(
        L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"zh-CN", &startSearchFormat_);
    if (SUCCEEDED(hr)) {
        startSearchFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        startSearchFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        startSearchFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        trimWithEllipsis(startSearchFormat_);
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
        24.0f, L"zh-CN", &musicTitleFormat_);
    if (SUCCEEDED(hr)) {
        musicTitleFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        musicTitleFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        musicTitleFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        trimWithEllipsis(musicTitleFormat_);
    }

    hr = factory_->CreateTextFormat(
        L"Segoe UI Variable Text", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        14.0f, L"zh-CN", &musicSubtitleFormat_);
    if (SUCCEEDED(hr)) {
        musicSubtitleFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        musicSubtitleFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        musicSubtitleFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        trimWithEllipsis(musicSubtitleFormat_);
    }

    // THE CARD'S LYRIC LINES: the current one (with room for the karaoke) and the next one, dimmed.
    // Both leading-aligned, because the karaoke sweeps from the left edge of the words.
    hr = factory_->CreateTextFormat(
        L"Segoe UI Variable Text", nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        16.5f, L"zh-CN", &musicLyricFormat_);
    if (SUCCEEDED(hr)) {
        musicLyricFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        musicLyricFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        musicLyricFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        trimWithEllipsis(musicLyricFormat_);
    }
    hr = factory_->CreateTextFormat(
        L"Segoe UI Variable Text", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        14.0f, L"zh-CN", &musicLyricNextFormat_);
    if (SUCCEEDED(hr)) {
        musicLyricNextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        musicLyricNextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        musicLyricNextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        trimWithEllipsis(musicLyricNextFormat_);
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

    // THE SYSTEM PANEL'S THREE FACES (from the reference control centre).
    //
    // The tile's name is 13 DIP semibold, the state under it and the slider rows' titles are 11 DIP
    // dim, and the tiles' glyphs are 18 DIP -- the 12 DIP face used by the deck's chevrons was the
    // size of a caption, which is exactly how it read on a large panel tile.
    hr = factory_->CreateTextFormat(
        L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 12.5f, L"zh-CN", &panelLabelFormat_);
    if (SUCCEEDED(hr) && panelLabelFormat_) {
        panelLabelFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        panelLabelFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        panelLabelFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        trimWithEllipsis(panelLabelFormat_);
    }
    hr = factory_->CreateTextFormat(
        L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"zh-CN", &panelSubFormat_);
    if (SUCCEEDED(hr) && panelSubFormat_) {
        panelSubFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        panelSubFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        panelSubFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        trimWithEllipsis(panelSubFormat_);
    }
    hr = factory_->CreateTextFormat(
        L"Segoe Fluent Icons", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 18.0f, L"en-US", &panelIconFormat_);
    if (SUCCEEDED(hr) && panelIconFormat_) {
        panelIconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        panelIconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
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
    SafeRelease(&commandArtworkBitmap_.bitmap);
    commandArtworkBitmap_.png = nullptr;
    commandArtworkBitmap_.size = 0;
    SafeRelease(&bodyFormat_);
    SafeRelease(&titleFormat_);
    SafeRelease(&musicTitleFormat_);
    SafeRelease(&coverNoteFormat_);
    SafeRelease(&musicSubtitleFormat_);
    SafeRelease(&glyphFormat_);
    SafeRelease(&inputMethodFormat_);
    SafeRelease(&gearGlyphFormat_);
    SafeRelease(&panelLabelFormat_);
    SafeRelease(&panelSubFormat_);
    SafeRelease(&panelIconFormat_);
    SafeRelease(&appRowFormat_);
    SafeRelease(&volumeFormat_);
    SafeRelease(&appFormat_);
    SafeRelease(&startLabelFormat_);
    SafeRelease(&startRowFormat_);
    SafeRelease(&startCaptionFormat_);
    SafeRelease(&startSearchFormat_);
    SafeRelease(&statusWeatherFormat_);
    SafeRelease(&niriCommandTimeFormat_);
    SafeRelease(&niriNotificationBodyFormat_);
    SafeRelease(&niriNotificationMetaFormat_);
    SafeRelease(&niriNotificationTitleFormat_);
    SafeRelease(&niriNotificationAppFormat_);
    SafeRelease(&niriNotificationHeaderFormat_);
    SafeRelease(&niriMediaTimeFormat_);
    SafeRelease(&niriMediaSubtitleFormat_);
    SafeRelease(&niriCalendarDayFormat_);
    SafeRelease(&niriCalendarWeekdayFormat_);
    SafeRelease(&niriWeatherWordFormat_);
    SafeRelease(&niriWeatherQualityFormat_);
    SafeRelease(&niriCommandChipFormat_);
    SafeRelease(&niriCommandDateFormat_);
    SafeRelease(&niriWeatherTempFormat_);
    SafeRelease(&niriMetricFormat_);
    SafeRelease(&niriHeroFormat_);
    SafeRelease(&niriLyricFormat_);
    SafeRelease(&niriDateFormat_);
    SafeRelease(&niriClockFormat_);
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
    niriTimeText_[0] = L'\0';
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
    // The layouts are shaped against the formats, not the theme, but a theme switch is exactly the
    // moment this cache is cold anyway; dropping it here means there is one place that frees them.
    for (auto& entry : layoutCache_) {
        if (entry.layout) {
            entry.layout->Release();
            entry.layout = nullptr;
        }
        entry.text.clear();
    }
}

// See the header: the cache owns the layout, and callers must not release what this returns.
IDWriteTextLayout* TextRenderer::LayoutFor(const wchar_t* text, IDWriteTextFormat* format,
                                           float width, float height) const {
    if (!factory_ || !text || !format) return nullptr;
    ++layoutCacheTick_;
    for (auto& entry : layoutCache_) {
        if (entry.layout && entry.format == format && entry.width == width &&
            entry.height == height && entry.text == text) {
            entry.used = layoutCacheTick_;
            return entry.layout;
        }
    }
    IDWriteTextLayout* created = nullptr;
    if (FAILED(factory_->CreateTextLayout(text, static_cast<UINT32>(std::wcslen(text)), format, width,
                                          height, &created)) ||
        !created) {
        return nullptr;
    }
    // Evict the least recently used slot, or use an empty one.
    LayoutEntry* victim = &layoutCache_[0];
    for (auto& entry : layoutCache_) {
        if (!entry.layout) {
            victim = &entry;
            break;
        }
        if (entry.used < victim->used) victim = &entry;
    }
    if (victim->layout) victim->layout->Release();
    victim->layout = created;
    victim->text = text;
    victim->format = format;
    victim->width = width;
    victim->height = height;
    victim->used = layoutCacheTick_;
    return created;
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
    std::swprintf(niriTimeText_, 16, L"%02u:%02u:%02u",
                  static_cast<unsigned>(now.wHour), static_cast<unsigned>(now.wMinute),
                  static_cast<unsigned>(now.wSecond));

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

    // THE STATUS RUN IS MEASURED EVERY FRAME, and the answer changes once a minute for the time and once
    // a day for the date. A small cache of the measured widths saves a CreateTextLayout per piece per
    // frame -- the weekday, the date, the time, the weather mark and its words.
    struct Measured {
        IDWriteTextFormat* format = nullptr;
        std::wstring text;
        float width = 0.0f;
        unsigned long long used = 0;
    };
    static Measured measured[32];
    static unsigned long long tick = 0;
    ++tick;
    for (auto& entry : measured) {
        if (entry.format == format && !entry.text.empty() && entry.text == text) {
            entry.used = tick;
            return entry.width;
        }
    }

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
    const float width = SUCCEEDED(hr) ? metrics.width : 0.0f;
    if (width > 0.0f) {
        Measured* victim = &measured[0];
        for (auto& entry : measured) {
            if (entry.text.empty()) {
                victim = &entry;
                break;
            }
            if (entry.used < victim->used) victim = &entry;
        }
        victim->format = format;
        victim->text = text;
        victim->width = width;
        victim->used = tick;
    }
    return width;
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
        slot.sourceSide = static_cast<float>(decoded.width < decoded.height ? decoded.width
                                                                           : decoded.height);
    } else {
        slot.sourceSide = 0.0f;
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

// One line of text with the format's alignment overridden for the call, defined beside DrawSystemPanel
// further down. Declared here for the same reason FillCardSurface is: the tray page is drawn above it.
void DrawTextIn(ID2D1DeviceContext2* context, IDWriteTextFormat* format, const wchar_t* text,
                const RectF& box, ID2D1Brush* brush, DWRITE_TEXT_ALIGNMENT horizontal,
                DWRITE_PARAGRAPH_ALIGNMENT vertical);

void FillRounded(ID2D1DeviceContext2* context, const RectF& box, float radius,
                 const D2D1_COLOR_F& colour) {
    if (!context || colour.a <= 0.001f || box.right <= box.left || box.bottom <= box.top) return;
    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(context->CreateSolidColorBrush(colour, &brush)) && brush) {
        const D2D1_ROUNDED_RECT rounded = {
            D2D1::RectF(box.left, box.top, box.right, box.bottom), radius, radius};
        context->FillRoundedRectangle(&rounded, brush);
        brush->Release();
    }
}

int DaysInMonth(int year, int month) {
    static constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
    return days[month - 1];
}

int DayOfWeekMondayFirst(int year, int month, int day) {
    static constexpr int offsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (month < 3) --year;
    const int sundayFirst = (year + year / 4 - year / 100 + year / 400 + offsets[month - 1] +
                             day) %
                            7;
    return (sundayFirst + 6) % 7;
}

void DrawWeatherMarkLarge(ID2D1DeviceContext2* context, const RectF& box, int code,
                          float opacity) {
    if (!context || opacity <= 0.01f) return;
    const float cx = (box.left + box.right) * 0.5f;
    const float cy = (box.top + box.bottom) * 0.5f;
    const float side = std::min(box.right - box.left, box.bottom - box.top);
    const float scale = side / 72.0f;
    const SkyMark mark = SkyMarkForCode(code);
    ID2D1SolidColorBrush* cloud = nullptr;
    ID2D1SolidColorBrush* sun = nullptr;
    ID2D1SolidColorBrush* accent = nullptr;
    context->CreateSolidColorBrush(D2D1::ColorF(0.35f, 0.43f, 0.76f, opacity), &cloud);
    context->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.78f, 0.24f, opacity), &sun);
    context->CreateSolidColorBrush(D2D1::ColorF(0.45f, 0.36f, 0.78f, opacity), &accent);
    const auto circles = [&](float dx, float dy, float factor) {
        if (!cloud) return;
        context->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(cx + dx - 13.0f * scale, cy + dy), 11.0f * scale * factor,
                          10.0f * scale * factor),
            cloud);
        context->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(cx + dx + 1.0f * scale, cy + dy - 7.0f * scale),
                          15.0f * scale * factor, 14.0f * scale * factor),
            cloud);
        context->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(cx + dx + 15.0f * scale, cy + dy - 1.0f * scale),
                          10.0f * scale * factor, 9.0f * scale * factor),
            cloud);
        const D2D1_ROUNDED_RECT base = {
            D2D1::RectF(cx + dx - 22.0f * scale, cy + dy - 3.0f * scale,
                        cx + dx + 23.0f * scale, cy + dy + 10.0f * scale),
            6.0f * scale, 6.0f * scale};
        context->FillRoundedRectangle(&base, cloud);
    };
    if (mark == SkyMark::Sun) {
        if (sun) {
            context->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 18.0f * scale,
                                               18.0f * scale),
                                 sun);
            for (int i = 0; i < 8; ++i) {
                const float angle = static_cast<float>(i) * 3.14159265f / 4.0f;
                context->DrawLine(
                    D2D1::Point2F(cx + std::cos(angle) * 23.0f * scale,
                                  cy + std::sin(angle) * 23.0f * scale),
                    D2D1::Point2F(cx + std::cos(angle) * 30.0f * scale,
                                  cy + std::sin(angle) * 30.0f * scale),
                    sun, 3.0f * scale);
            }
        }
    } else if (mark == SkyMark::SunCloud) {
        if (sun) {
            context->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + 12.0f * scale,
                                                              cy - 15.0f * scale),
                                               13.0f * scale, 13.0f * scale),
                                 sun);
        }
        circles(-4.0f, 7.0f, 1.0f);
    } else if (mark == SkyMark::Cloud) {
        circles(0.0f, 4.0f, 1.0f);
    } else if (mark == SkyMark::Rain || mark == SkyMark::Snow) {
        circles(0.0f, -4.0f, 0.9f);
        if (accent) {
            for (int i = -2; i <= 2; ++i) {
                const float x = cx + static_cast<float>(i) * 8.0f * scale;
                context->DrawLine(D2D1::Point2F(x, cy + 22.0f * scale),
                                  D2D1::Point2F(x + (mark == SkyMark::Snow ? 0.0f : 4.0f) * scale,
                                                cy + 32.0f * scale),
                                  accent, 3.0f * scale);
            }
        }
    } else if (mark == SkyMark::Thunder) {
        circles(0.0f, -4.0f, 0.9f);
        if (sun) {
            context->DrawLine(D2D1::Point2F(cx - 2.0f * scale, cy + 18.0f * scale),
                              D2D1::Point2F(cx + 8.0f * scale, cy + 18.0f * scale), sun,
                              6.0f * scale);
            context->DrawLine(D2D1::Point2F(cx + 8.0f * scale, cy + 18.0f * scale),
                              D2D1::Point2F(cx + 1.0f * scale, cy + 34.0f * scale), sun,
                              6.0f * scale);
        }
    } else if (mark == SkyMark::Fog) {
        if (cloud) {
            for (int i = 0; i < 3; ++i) {
                context->DrawLine(
                    D2D1::Point2F(cx - 27.0f * scale, cy + static_cast<float>(i) * 10.0f * scale),
                    D2D1::Point2F(cx + 27.0f * scale, cy + static_cast<float>(i) * 10.0f * scale),
                    cloud, 4.0f * scale);
            }
        }
    }
    if (cloud) cloud->Release();
    if (sun) sun->Release();
    if (accent) accent->Release();
}

} // namespace

void TextRenderer::DrawTrayPage(ID2D1DeviceContext2* context, const RoundedBox& card,
                                const TrayPageText& text, const TrayIcon* icons, int count,
                                unsigned long long revision, float opacity, int hoveredIndex) {
    if (!context || opacity <= 0.01f) return;

    const TrayPageLayout layout = TrayPageFor(card, count);
    {
        static int lastCount = -1;
        if (layout.count != lastCount) {
            lastCount = layout.count;
            int withPng = 0;
            if (icons) {
                for (int i = 0; i < layout.count; ++i) {
                    if (icons[i].png && icons[i].pngSize) ++withPng;
                }
            }
            island::LogLine(L"  tray page: %d icon(s) in %d column(s), %d with a picture, card %.0fx%.0f",
                            layout.count, layout.columns, withPng, card.rect.right - card.rect.left,
                            card.rect.bottom - card.rect.top);
        }
    }

    // --- the header row: the status dot, the title and the count chip --------------------------------
    //
    // The rule under the row separates the header from the grid the way the panel's footer rule separates
    // the footer from the card.
    constexpr float kGreenR = 0.2039f;
    constexpr float kGreenG = 0.8275f;
    constexpr float kGreenB = 0.6000f;
    // The reference's accent, #0078d4. Marked here rather than taken from the theme because the page's
    // interactive highlight language uses the same blue on every theme.
    constexpr float kAccentR = 0.0f;
    constexpr float kAccentG = 0.4706f;
    constexpr float kAccentB = 0.8314f;

    if (ID2D1SolidColorBrush* dot =
            BrushFor(context, D2D1::ColorF(kGreenR, kGreenG, kGreenB, opacity))) {
        const D2D1_ELLIPSE circle = D2D1::Ellipse(
            D2D1::Point2F((layout.headerDot.left + layout.headerDot.right) * 0.5f,
                          (layout.headerDot.top + layout.headerDot.bottom) * 0.5f),
            (layout.headerDot.right - layout.headerDot.left) * 0.5f,
            (layout.headerDot.bottom - layout.headerDot.top) * 0.5f);
        context->FillEllipse(&circle, dot);
    }

    if (text.title && text.title[0] != L'\0' && panelLabelFormat_) {
        // The title in the THEME's ink, like every other word on the card: it was a hard-coded near-white,
        // which is the one colour that shows up as a grey patch in a theme with a coloured ink.
        if (ID2D1SolidColorBrush* ink = BrushFor(context, island::CardInk(0.92f * opacity))) {
            DrawTextIn(context, panelLabelFormat_, text.title, layout.title, ink,
                       DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }

    // THE COUNT CHIP: "N 个进程" in a small rounded plate, the reference's pill beside the title. The number
    // is the icons the frame brought, not the registry's, so the chip and the grid cannot disagree about how
    // many applications this page is showing.
    if (layout.countChip.right > layout.countChip.left) {
        const D2D1_ROUNDED_RECT chip = {
            D2D1::RectF(layout.countChip.left, layout.countChip.top, layout.countChip.right,
                        layout.countChip.bottom),
            8.0f, 8.0f};
        if (ID2D1SolidColorBrush* fill = BrushFor(context, island::CardInk(0.06f * opacity))) {
            context->FillRoundedRectangle(&chip, fill);
        }
        if (ID2D1SolidColorBrush* edge = BrushFor(context, island::CardInk(0.09f * opacity))) {
            context->DrawRoundedRectangle(&chip, edge, 1.0f);
        }
        if (panelSubFormat_) {
            wchar_t countText[32] = {};
            std::swprintf(countText, 32, L"%d \u4e2a\u5b88\u62a4\u8fdb\u7a0b",
                          layout.count);   // 个守护进程
            if (ID2D1SolidColorBrush* ink = BrushFor(context, island::CardInk(0.62f * opacity))) {
                DrawTextIn(context, panelSubFormat_, countText, layout.countText, ink,
                           DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
        }
    }

    if (ID2D1SolidColorBrush* rule = BrushFor(context, island::CardInk(0.08f * opacity))) {
        context->DrawLine(D2D1::Point2F(layout.header.left, layout.header.bottom),
                          D2D1::Point2F(layout.header.right, layout.header.bottom), rule, 1.0f);
    }

    // --- the section row --------------------------------------------------------------------------
    if (panelSubFormat_) {
        if (ID2D1SolidColorBrush* ink = BrushFor(context, island::CardInk(0.55f * opacity))) {
            DrawTextIn(context, panelSubFormat_, L"\u9a7b\u7559\u6258\u76d8\u5e94\u7528",
                       layout.sectionLabel, ink, DWRITE_TEXT_ALIGNMENT_LEADING,
                       DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }

    // --- the grid ---------------------------------------------------------------------------------
    const island::Theme& theme = island::CurrentTheme();
    for (int i = 0; i < layout.count && icons; ++i) {
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
        //
        // RADIUS 14, the panel's tiles' own 16 less two: an icon cell is smaller than a panel tile and the
        // same 16 would eat a sixth of its edge.
        constexpr float kTileRadius = 16.0f;
        const D2D1_ROUNDED_RECT tile = {
            D2D1::RectF(cell.left, cell.top, cell.right, cell.bottom), kTileRadius, kTileRadius};
        if (theme.lens) {
            island::DrawLensRimBand(context, tile, theme, opacity);
            if (i == hoveredIndex) {
                if (ID2D1SolidColorBrush* wash =
                        BrushFor(context, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f * opacity))) {
                    context->FillRoundedRectangle(&tile, wash);
                }
            }
        } else {
            const bool hot = i == hoveredIndex;
            const D2D1_COLOR_F base = hot
                                          ? D2D1::ColorF(kAccentR, kAccentG, kAccentB,
                                                         0.16f * opacity)
                                          : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.25f * opacity);
            if (ID2D1SolidColorBrush* tileFill = BrushFor(context, base)) {
                context->FillRoundedRectangle(&tile, tileFill);
            }
            const D2D1_COLOR_F tileEdgeColour =
                hot ? D2D1::ColorF(kAccentR, kAccentG, kAccentB, 0.58f * opacity)
                    : island::CardInk(0.07f * opacity);
            if (ID2D1SolidColorBrush* tileEdge = BrushFor(context, tileEdgeColour)) {
                context->DrawRoundedRectangle(&tile, tileEdge, 1.0f);
            }
            if (hot) {
                ID2D1SolidColorBrush* wash = nullptr;
                if ((wash = BrushFor(context,
                                     D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.055f * opacity))) != nullptr) {
                    context->FillRoundedRectangle(&tile, wash);
                }
            }
        }

        ID2D1Bitmap1* bitmap = TrayIconBitmap(context, i, icons[i], revision);
        if (bitmap) {
            // The picture is capped near its SOURCE resolution: Windows' registry snapshots are commonly only
            // 16 or 32 pixels, and stretching a 16-pixel state icon to 42 DIP is what made the page look soft.
            // A 64-pixel application icon still gets the full visual size.
            const float cellDip = cell.right - cell.left;
            float side = cellDip * 0.60f;
            if (side > 42.0f) side = 42.0f;
            if (side < 22.0f) side = 22.0f;
            const float sourceSide = trayIconSlots_[i].sourceSide;
            if (sourceSide > 0.0f && side > sourceSide * 1.20f) side = sourceSide * 1.20f;
            if (side < 20.0f) side = 20.0f;
            const float centreX = (cell.left + cell.right) * 0.5f;
            const float centreY = (cell.top + cell.bottom) * 0.5f;
            const D2D1_RECT_F target = D2D1::RectF(centreX - side * 0.5f, centreY - side * 0.5f,
                                                   centreX + side * 0.5f, centreY + side * 0.5f);
            context->DrawBitmap(bitmap, &target, opacity,
                                D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        }

    }

    // --- the live system-load well ---------------------------------------------------------------------
    if (layout.resources.right > layout.resources.left) {
        constexpr float kWellRadius = 16.0f;
        const D2D1_ROUNDED_RECT well = {
            D2D1::RectF(layout.resources.left, layout.resources.top, layout.resources.right,
                        layout.resources.bottom),
            kWellRadius, kWellRadius};
        if (ID2D1SolidColorBrush* fill =
                BrushFor(context, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.20f * opacity))) {
            context->FillRoundedRectangle(&well, fill);
        }
        if (ID2D1SolidColorBrush* edge = BrushFor(context, island::CardInk(0.07f * opacity))) {
            context->DrawRoundedRectangle(&well, edge, 1.0f);
        }

        // Memory chip mark.
        const RectF& memoryIcon = layout.memoryIcon;
        const D2D1_ROUNDED_RECT chip = {
            D2D1::RectF(memoryIcon.left + 2.0f, memoryIcon.top + 3.0f,
                        memoryIcon.right - 2.0f, memoryIcon.bottom - 3.0f),
            3.0f, 3.0f};
        if (ID2D1SolidColorBrush* mark =
                BrushFor(context, D2D1::ColorF(0.52f, 0.38f, 1.0f, 0.92f * opacity))) {
            context->FillRoundedRectangle(&chip, mark);
            for (int pin = 0; pin < 3; ++pin) {
                const float x = memoryIcon.left + 4.0f + static_cast<float>(pin) * 4.0f;
                context->DrawLine(D2D1::Point2F(x, memoryIcon.top), D2D1::Point2F(x, memoryIcon.top + 3.0f),
                                  mark, 1.0f);
                context->DrawLine(D2D1::Point2F(x, memoryIcon.bottom - 3.0f),
                                  D2D1::Point2F(x, memoryIcon.bottom), mark, 1.0f);
            }
        }

        if (panelSubFormat_) {
            if (ID2D1SolidColorBrush* ink =
                    BrushFor(context, island::CardInk(0.82f * opacity))) {
                DrawTextIn(context, panelSubFormat_, L"\u5185\u5B58\u8D1F\u8F7D",
                           layout.memoryLabel, ink, DWRITE_TEXT_ALIGNMENT_LEADING,
                           DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
        }
        constexpr double kGigabyte = 1024.0 * 1024.0 * 1024.0;
        wchar_t memoryText[96] = {};
        std::swprintf(memoryText, 96, L"%.1f GB / %.1f GB (%.0f%%)",
                      static_cast<double>(text.memoryUsedBytes) / kGigabyte,
                      static_cast<double>(text.memoryTotalBytes) / kGigabyte,
                      static_cast<double>(text.memoryPercent));
        if (panelSubFormat_) {
            if (ID2D1SolidColorBrush* ink =
                    BrushFor(context, island::CardInk(0.88f * opacity))) {
                DrawTextIn(context, panelSubFormat_, memoryText, layout.memoryValue, ink,
                           DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
        }

        // The memory bar uses the same blue-to-green accent language as the reference. It is drawn as small
        // solid segments because Direct2D's plain solid brush is the renderer's only retained brush type.
        if (ID2D1SolidColorBrush* track =
                BrushFor(context, island::CardInk(0.12f * opacity))) {
            const D2D1_ROUNDED_RECT shape = {
                D2D1::RectF(layout.memoryBar.left, layout.memoryBar.top, layout.memoryBar.right,
                            layout.memoryBar.bottom),
                2.0f, 2.0f};
            context->FillRoundedRectangle(&shape, track);
        }
        float memoryFraction = text.memoryPercent / 100.0f;
        if (memoryFraction < 0.0f) memoryFraction = 0.0f;
        if (memoryFraction > 1.0f) memoryFraction = 1.0f;
        const float memoryRight =
            layout.memoryBar.left + (layout.memoryBar.right - layout.memoryBar.left) * memoryFraction;
        constexpr int kSegments = 32;
        for (int segment = 0; segment < kSegments; ++segment) {
            const float t = (static_cast<float>(segment) + 0.5f) / kSegments;
            const float left = layout.memoryBar.left +
                               (memoryRight - layout.memoryBar.left) * static_cast<float>(segment) /
                                   kSegments;
            const float right = layout.memoryBar.left +
                                (memoryRight - layout.memoryBar.left) *
                                    static_cast<float>(segment + 1) / kSegments;
            if (right <= left) break;
            const D2D1_COLOR_F colour =
                D2D1::ColorF(0.26f + 0.02f * (1.0f - t), 0.52f + 0.34f * t,
                             1.0f - 0.34f * t, 0.95f * opacity);
            if (ID2D1SolidColorBrush* segmentBrush = BrushFor(context, colour)) {
                const D2D1_ROUNDED_RECT shape = {
                    D2D1::RectF(left, layout.memoryBar.top, right, layout.memoryBar.bottom), 2.0f,
                    2.0f};
                context->FillRoundedRectangle(&shape, segmentBrush);
            }
        }

        // CPU mark: three ascending bars, warm like the reference's CPU indicator.
        if (ID2D1SolidColorBrush* mark =
                BrushFor(context, D2D1::ColorF(1.0f, 0.70f, 0.24f, 0.94f * opacity))) {
            constexpr float heights[3] = {5.0f, 9.0f, 13.0f};
            for (int bar = 0; bar < 3; ++bar) {
                const float x = layout.cpuIcon.left + 2.0f + static_cast<float>(bar) * 5.0f;
                const float bottom = layout.cpuIcon.bottom - 1.0f;
                context->FillRoundedRectangle(
                    D2D1::RoundedRect(
                        D2D1::RectF(x, bottom - heights[bar], x + 3.0f, bottom), 1.5f, 1.5f),
                    mark);
            }
        }

        if (panelSubFormat_) {
            if (ID2D1SolidColorBrush* ink =
                    BrushFor(context, island::CardInk(0.82f * opacity))) {
                DrawTextIn(context, panelSubFormat_, L"CPU \u4F7F\u7528\u7387",
                           layout.cpuLabel, ink, DWRITE_TEXT_ALIGNMENT_LEADING,
                           DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
            wchar_t cpuText[32] = {};
            std::swprintf(cpuText, 32, L"%.0f%%", static_cast<double>(text.cpuPercent));
            const float cpu = text.cpuPercent;
            const D2D1_COLOR_F cpuInk =
                cpu >= 90.0f ? D2D1::ColorF(0.98f, 0.38f, 0.34f, opacity)
                : cpu >= 70.0f ? D2D1::ColorF(1.0f, 0.70f, 0.24f, opacity)
                               : D2D1::ColorF(0.24f, 0.86f, 0.66f, opacity);
            if (ID2D1SolidColorBrush* ink = BrushFor(context, cpuInk)) {
                DrawTextIn(context, panelSubFormat_, cpuText, layout.cpuValue, ink,
                           DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
        }
    }

    // --- the footer: one real Task Manager action at the right -----------------------------------------
    if (layout.footer.right > layout.footer.left) {
        if (ID2D1SolidColorBrush* rule = BrushFor(context, island::CardInk(0.08f * opacity))) {
            context->DrawLine(D2D1::Point2F(layout.footer.left, layout.footer.top),
                              D2D1::Point2F(layout.footer.right, layout.footer.top), rule, 1.0f);
        }
        const bool hot = text.taskManagerHovered;
        const D2D1_ROUNDED_RECT button = {
            D2D1::RectF(layout.footerTaskManager.left, layout.footerTaskManager.top,
                        layout.footerTaskManager.right, layout.footerTaskManager.bottom),
            18.0f,
            18.0f};
        if (ID2D1SolidColorBrush* fill = BrushFor(
                context, island::CardInk((hot ? 0.13f : 0.065f) * opacity))) {
            context->FillRoundedRectangle(&button, fill);
        }
        if (ID2D1SolidColorBrush* edge = BrushFor(
                context, island::CardInk((hot ? 0.24f : 0.11f) * opacity))) {
            context->DrawRoundedRectangle(&button, edge, 1.0f);
        }

        if (panelSubFormat_) {
            if (ID2D1SolidColorBrush* ink =
                    BrushFor(context, island::CardInk((hot ? 0.96f : 0.76f) * opacity))) {
                DrawTextIn(context, panelSubFormat_, L"\u4efb\u52a1\u7ba1\u7406\u5668",
                           layout.footerTaskManagerLabel, ink, DWRITE_TEXT_ALIGNMENT_LEADING,
                           DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
        }
        if (layout.footerGear.right > layout.footerGear.left) {
            const float centreX =
                (layout.footerGear.left + layout.footerGear.right) * 0.5f;
            const float centreY =
                (layout.footerGear.top + layout.footerGear.bottom) * 0.5f;
            if (ID2D1SolidColorBrush* mark = BrushFor(
                    context, D2D1::ColorF(kAccentR, kAccentG, kAccentB,
                                          (hot ? 0.96f : 0.72f) * opacity))) {
                constexpr float kBarWidth = 3.0f;
                constexpr float heights[3] = {7.0f, 13.0f, 10.0f};
                for (int bar = 0; bar < 3; ++bar) {
                    const float x = centreX - 5.0f + static_cast<float>(bar) * 5.0f;
                    const float top = centreY + 6.0f - heights[bar];
                    const D2D1_ROUNDED_RECT shape = {
                        D2D1::RectF(x, top, x + kBarWidth, centreY + 6.0f), 1.5f, 1.5f};
                    context->FillRoundedRectangle(&shape, mark);
                }
            }
        }
    }
}

void TextRenderer::DrawTrayMenu(ID2D1DeviceContext2* context, const island::RectF* rows, int count,
                                const wchar_t* const* labels, int hoveredRow, float opacity) {
    if (!context || !rows || !labels || count <= 0 || opacity <= 0.01f) return;
    Refresh();

    // The surface: a raised rectangle that sits ON the card, so it reads as something in front rather than
    // as a hole in it. The PAINTED themes get the dark panel; the glass theme gets a SHEET of its own --
    // not the card's own 2% crystal, which let the tray page's icons show through the words (see the note
    // in Theme.h above TrayMenuBody: "托盘卡片里 鼠标右键弹窗看不清", measured 2026-11-05).
    const float pad = kTrayMenuPadDip;
    const D2D1_ROUNDED_RECT box = {
        D2D1::RectF(rows[0].left - pad, rows[0].top - pad, rows[0].right + pad,
                    rows[count - 1].bottom + pad),
        10.0f, 10.0f};
    const island::Theme& theme = island::CurrentTheme();
    // The sheet and its ink are decided TOGETHER. Painted themes (including Niri) use the fixed dark
    // menu plate below, so the words must be white even though the light Niri card uses dark ink. The
    // old code paired the dark plate with CardInk(), which made every row nearly invisible on Niri.
    const bool lensMenu = theme.lens;
    D2D1_COLOR_F menuInk = lensMenu ? CardInk(1.0f)
                                   : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
    const bool darkSheet = !lensMenu || island::CardInkIsLight();
    if (theme.lens) {
        // The body first, then the same crystal the card wears -- outline, facet and sheen -- so the menu
        // is still the same material, just thick enough to be a menu instead of a window.
        ID2D1SolidColorBrush* body = nullptr;
        if (SUCCEEDED(context->CreateSolidColorBrush(island::TrayMenuBody(menuInk, opacity), &body)) &&
            body) {
            context->FillRoundedRectangle(&box, body);
            body->Release();
        }
        island::DrawCardCrystal(context, box, opacity);
    } else {
        ID2D1SolidColorBrush* fill = nullptr;
        if (SUCCEEDED(context->CreateSolidColorBrush(
                D2D1::ColorF(0.13f, 0.13f, 0.16f, 0.99f * opacity), &fill)) &&
            fill) {
            context->FillRoundedRectangle(&box, fill);
            fill->Release();
        }
        D2D1_COLOR_F edgeInk = menuInk;
        edgeInk.a = 0.14f * opacity;
        ID2D1SolidColorBrush* edge = nullptr;
        if (SUCCEEDED(context->CreateSolidColorBrush(edgeInk, &edge)) &&
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
            // A white gloss on a DARK sheet, the ink tint on a light one -- and on every painted theme,
            // whose panel is dark and whose ink is white, which is where the ink tint came from. The white
            // wash used to be unconditional for the glass theme, where it landed on whichever body the ink
            // is not, so on the light sheet it was invisible (2026-11-05).
            const D2D1_COLOR_F hover = darkSheet ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f * opacity)
                                                 : CardInk(0.16f * opacity);
            ID2D1SolidColorBrush* wash = nullptr;
            if (SUCCEEDED(context->CreateSolidColorBrush(hover, &wash)) && wash) {
                context->FillRoundedRectangle(&rowBox, wash);
                wash->Release();
            }
        }
        const wchar_t* label = labels[i] ? labels[i] : L"";
        if (label[0] == L'\0') continue;
        D2D1_COLOR_F inkColour = menuInk;
        inkColour.a = 0.96f * opacity;
        ID2D1SolidColorBrush* ink = nullptr;
        if (SUCCEEDED(context->CreateSolidColorBrush(inkColour, &ink)) && ink) {
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

void TextRenderer::DrawNiriTopBar(ID2D1DeviceContext2* context, const RoundedBox& box,
                                  float opacity, int unreadNotifications, const char* sideLine,
                                  bool musicPresent, float musicLevel, float musicPhase,
                                  D2D1_COLOR_F musicAccent, bool callActive, float callLevel,
                                  float lyricScrollDip, float searchMarkOpacity) {
    if (!context || !niriClockFormat_ || !niriDateFormat_ || opacity <= 0.01f) return;
    Refresh();

    const float centreY = (box.rect.top + box.rect.bottom) * 0.5f;
    ID2D1SolidColorBrush* ink = nullptr;
    if (FAILED(context->CreateSolidColorBrush(PillInk(0.92f * opacity), &ink)) || !ink) return;
    ID2D1SolidColorBrush* muted = nullptr;
    context->CreateSolidColorBrush(PillInk(0.55f * opacity), &muted);
    ID2D1SolidColorBrush* accent = nullptr;
    D2D1_COLOR_F accentColour = island::CurrentTheme().accent;
    accentColour.a *= opacity;
    context->CreateSolidColorBrush(accentColour, &accent);
    ID2D1SolidColorBrush* searchInk = nullptr;
    if (searchMarkOpacity > 0.01f) {
        context->CreateSolidColorBrush(
            PillInk(0.92f * opacity * std::clamp(searchMarkOpacity, 0.0f, 1.0f)),
            &searchInk);
    }

    // The search mark has one fixed home in both the normal and animated
    // search states. The content after it is laid out independently, so the
    // mark never shifts when the lavender field grows around it.
    constexpr float kSearchCentreFromLeft = 16.0f;
    constexpr float kContentLeftFromLeft = 42.0f;
    constexpr float kTimeWidth = 74.0f;
    constexpr float kSeparatorWidth = 14.0f;
    constexpr float kDateWidth = 116.0f;
    constexpr float kStatusGap = 6.0f;
    const float statusWidth = callActive ? 32.0f : (unreadNotifications > 0 ? 32.0f : 12.0f);
    float cursor = box.rect.left + kContentLeftFromLeft;

    const auto drawMusicWave = [&](float waveCentreX) {
        // Keep the cover's HUE, but move very pale accents away from the white Niri island so the
        // waveform remains readable. Dark accents are lifted by the same rule.
        D2D1_COLOR_F visibleAccent = musicAccent;
        const float luma =
            0.2126f * visibleAccent.r + 0.7152f * visibleAccent.g + 0.0722f * visibleAccent.b;
        float scale = 1.0f;
        if (luma > 0.68f && luma > 0.001f) scale = 0.48f / luma;
        if (luma < 0.20f && luma > 0.001f) scale = 0.34f / luma;
        visibleAccent.r = std::min(1.0f, visibleAccent.r * scale);
        visibleAccent.g = std::min(1.0f, visibleAccent.g * scale);
        visibleAccent.b = std::min(1.0f, visibleAccent.b * scale);

        D2D1_COLOR_F fillColour = visibleAccent;
        fillColour.a = 0.18f * opacity;
        ID2D1SolidColorBrush* circle = nullptr;
        context->CreateSolidColorBrush(fillColour, &circle);
        if (circle) {
            context->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(waveCentreX, centreY), 13.0f, 13.0f),
                circle);
            circle->Release();
        }
        D2D1_COLOR_F ringColour = visibleAccent;
        ringColour.a = 0.62f * opacity;
        context->CreateSolidColorBrush(ringColour, &circle);
        if (circle) {
            context->DrawEllipse(
                D2D1::Ellipse(D2D1::Point2F(waveCentreX, centreY), 13.0f, 13.0f),
                circle, 1.25f);
            circle->Release();
        }
        ID2D1SolidColorBrush* wave = nullptr;
        D2D1_COLOR_F waveColour = visibleAccent;
        waveColour.a = 0.96f * opacity;
        context->CreateSolidColorBrush(waveColour, &wave);
        if (wave) {
            for (int i = 0; i < 5; ++i) {
                const float phase = musicPhase * 5.0f + static_cast<float>(i) * 0.9f;
                const float envelope = 0.22f + 0.78f * (0.5f + 0.5f * std::sin(phase));
                const float level = 0.25f + 0.75f * std::min(1.0f, musicLevel);
                const float half = 1.5f + 9.5f * envelope * level;
                const float x = waveCentreX - 5.2f + static_cast<float>(i) * 2.6f;
                context->DrawLine(D2D1::Point2F(x, centreY - half),
                                  D2D1::Point2F(x, centreY + half), wave, 2.2f);
            }
            wave->Release();
        }
    };

    // Search mark, drawn rather than typed so it remains crisp at every DPI.
    if (searchInk) {
        const float searchX = box.rect.left + kSearchCentreFromLeft;
        context->DrawEllipse(
            D2D1::Ellipse(D2D1::Point2F(searchX, centreY - 1.0f), 5.2f, 5.2f),
            searchInk, 1.7f);
        context->DrawLine(D2D1::Point2F(searchX + 4.0f, centreY + 3.0f),
                          D2D1::Point2F(searchX + 8.0f, centreY + 7.0f), searchInk, 1.7f);
    }

    const RectF timeBox = {cursor, box.rect.top + 1.0f, cursor + kTimeWidth,
                           box.rect.bottom - 1.0f};
    DrawTextIn(context, niriClockFormat_, niriTimeText_, timeBox, ink,
               DWRITE_TEXT_ALIGNMENT_CENTER,
               DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    cursor += kTimeWidth;

    const float dotX = cursor + 8.0f;
    context->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dotX, centreY), 2.0f, 2.0f),
                         accent ? accent : (muted ? muted : ink));
    cursor += kSeparatorWidth;

    if (musicPresent && !callActive) {
        // While music is playing the date/notification group becomes the lyric line. The moving
        // waveform moves to the far right, where it no longer competes with the search mark.
        constexpr float kWaveDiameter = 26.0f;
        constexpr float kWaveGap = 8.0f;
        const float lyricRight = box.rect.right - kWaveDiameter - kWaveGap - 6.0f;
        const RectF lyricBox = {cursor, box.rect.top + 1.0f, lyricRight,
                                box.rect.bottom - 1.0f};
        wchar_t lyric[512] = {};
        if (sideLine && sideLine[0] != '\0' &&
            MultiByteToWideChar(CP_UTF8, 0, sideLine, -1, lyric, 512) > 0 &&
            lyricBox.right > lyricBox.left) {
            IDWriteTextFormat* lyricFormat =
                niriLyricFormat_ ? niriLyricFormat_ : niriDateFormat_;
            const float available = lyricBox.right - lyricBox.left;
            IDWriteTextLayout* measuredLayout =
                LayoutFor(lyric, lyricFormat, 8192.0f, 512.0f);
            DWRITE_TEXT_METRICS metrics = {};
            const bool measured =
                measuredLayout &&
                SUCCEEDED(measuredLayout->GetMetrics(&metrics));
            const float textWidth = measured ? metrics.width : 0.0f;
            // LayoutFor is cache-owned; releasing it here poisons the cache and crashes the next draw.
            if (!measured || !LyricNeedsScroll(textWidth, available)) {
                DrawTextIn(context, lyricFormat, lyric, lyricBox, ink,
                           DWRITE_TEXT_ALIGNMENT_CENTER,
                           DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            } else {
                ID2D1Factory* d2dFactory = nullptr;
                context->GetFactory(&d2dFactory);
                ID2D1RectangleGeometry* clip = nullptr;
                if (d2dFactory) {
                    d2dFactory->CreateRectangleGeometry(
                        D2D1::RectF(lyricBox.left, lyricBox.top, lyricBox.right,
                                    lyricBox.bottom),
                        &clip);
                    d2dFactory->Release();
                }
                if (clip) {
                    context->PushLayer(
                        D2D1::LayerParameters1(D2D1::InfiniteRect(), clip,
                                               D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                        nullptr);
                }
                const float stride = textWidth + 40.0f;
                const float offset =
                    lyricScrollDip -
                    stride * static_cast<float>(
                                 static_cast<long long>(lyricScrollDip / stride));
                const auto drawAt = [&](float left) {
                    const RectF drawBox = {left, lyricBox.top, left + textWidth + 4.0f,
                                           lyricBox.bottom};
                    DrawTextIn(context, lyricFormat, lyric, drawBox, ink,
                               DWRITE_TEXT_ALIGNMENT_LEADING,
                               DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                };
                const float start = lyricBox.left + 6.0f;
                drawAt(start - offset);
                drawAt(start - offset + stride);
                if (offset > 0.0f) drawAt(start - offset - stride);
                if (clip) {
                    context->PopLayer();
                    clip->Release();
                }
            }
        }
        const RectF waveButton = island::NiriMusicWaveButtonFor(box);
        drawMusicWave((waveButton.left + waveButton.right) * 0.5f);
    } else {
        wchar_t fullDate[80] = {};
        std::swprintf(fullDate, 80, L"%ls %04u.%02u.%02u", weekdayText_,
                      static_cast<unsigned>(time_.wYear),
                      static_cast<unsigned>(
                          time_.wMonth >= 1 && time_.wMonth <= 12 ? time_.wMonth : 1),
                      static_cast<unsigned>(time_.wDay));
        const RectF secondaryBox = {cursor, box.rect.top + 1.0f, cursor + kDateWidth,
                                    box.rect.bottom - 1.0f};
        const wchar_t* secondary = fullDate;
        wchar_t noticeText[512] = {};
        if (sideLine && sideLine[0] != '\0' &&
            MultiByteToWideChar(CP_UTF8, 0, sideLine, -1, noticeText, 512) > 0) {
            secondary = noticeText;
        }
        if (muted && secondaryBox.right > secondaryBox.left) {
            DrawTextIn(context, niriDateFormat_, secondary, secondaryBox, muted,
                   DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        cursor += kDateWidth + kStatusGap;

        // The right-hand status is one compact group: bell (or call waveform) and
        // the plain count beside it, exactly as in the reference.
        const float bellX = cursor + 7.0f;
        if (callActive) {
            ID2D1SolidColorBrush* call = nullptr;
            context->CreateSolidColorBrush(
                D2D1::ColorF(0.18f, 0.82f, 0.48f, 0.95f * opacity), &call);
            if (call) {
                for (int i = 0; i < 7; ++i) {
                    const float phase = musicPhase * 6.0f + static_cast<float>(i) * 0.8f;
                    const float envelope =
                        0.25f + 0.75f * (0.5f + 0.5f * std::sin(phase));
                    const float half =
                        1.0f + 7.0f * envelope * std::min(1.0f, callLevel);
                    const float x = cursor + 2.0f + static_cast<float>(i) * 4.0f;
                    context->DrawLine(D2D1::Point2F(x, centreY - half),
                                      D2D1::Point2F(x, centreY + half), call, 2.0f);
                }
                call->Release();
            }
        } else {
            context->DrawRoundedRectangle(
                D2D1::RoundedRect(
                    D2D1::RectF(bellX - 5.0f, centreY - 5.5f, bellX + 5.0f,
                                centreY + 4.0f),
                    5.0f, 5.0f),
                ink, 1.6f);
            context->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(bellX, centreY + 6.5f), 1.7f, 1.7f),
                ink);
        }
        if (!callActive && unreadNotifications > 0) {
            wchar_t count[16] = {};
            std::swprintf(count, 16, L"%d", unreadNotifications);
            const RectF countBox = {cursor + 14.0f, box.rect.top + 1.0f,
                                    cursor + statusWidth, box.rect.bottom - 1.0f};
            DrawTextIn(context, niriDateFormat_, count, countBox, ink,
                       DWRITE_TEXT_ALIGNMENT_CENTER,
                       DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }

    if (searchInk) searchInk->Release();
    if (accent) accent->Release();
    if (muted) muted->Release();
    ink->Release();
}

void TextRenderer::DrawNiriNotification(ID2D1DeviceContext2* context, const RoundedBox& box,
                                        const char* line, const uint8_t* iconPixels,
                                        int iconWidth, int iconHeight,
                                        unsigned long long iconRevision, float opacity) {
    if (!context || !line || line[0] == '\0' || opacity <= 0.01f) return;
    IDWriteTextFormat* format = niriMetricFormat_ ? niriMetricFormat_ : niriDateFormat_;
    if (!format) return;

    wchar_t text[512] = {};
    if (MultiByteToWideChar(CP_UTF8, 0, line, -1, text, 512) <= 0) return;

    const float centreY = (box.rect.top + box.rect.bottom) * 0.5f;
    const float iconCentreX = box.rect.left + 34.0f;
    D2D1_COLOR_F halo = island::CurrentTheme().accent;
    halo.a = 0.14f * opacity;
    if (ID2D1SolidColorBrush* brush = nullptr;
        SUCCEEDED(context->CreateSolidColorBrush(halo, &brush)) && brush) {
        context->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(iconCentreX, centreY), 16.0f, 16.0f), brush);
        brush->Release();
    }

    const float iconSide = 24.0f;
    const RectF iconBox = {iconCentreX - iconSide * 0.5f, centreY - iconSide * 0.5f,
                           iconCentreX + iconSide * 0.5f, centreY + iconSide * 0.5f};
    bool drewIcon = false;
    if (ID2D1Bitmap1* icon = NotificationIconBitmap(context, iconPixels, iconWidth,
                                                    iconHeight, iconRevision)) {
        ID2D1Factory* d2dFactory = nullptr;
        context->GetFactory(&d2dFactory);
        ID2D1RoundedRectangleGeometry* clip = nullptr;
        if (d2dFactory) {
            d2dFactory->CreateRoundedRectangleGeometry(
                D2D1::RoundedRect(D2D1::RectF(iconBox.left, iconBox.top, iconBox.right,
                                             iconBox.bottom),
                                  7.0f, 7.0f),
                &clip);
        }
        if (clip) {
            context->PushLayer(
                D2D1::LayerParameters1(D2D1::InfiniteRect(), clip,
                                       D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                nullptr);
        }
        context->DrawBitmap(
            icon,
            D2D1::RectF(iconBox.left, iconBox.top, iconBox.right, iconBox.bottom),
            opacity, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        drewIcon = true;
        if (clip) {
            context->PopLayer();
            clip->Release();
        }
        SafeRelease(&d2dFactory);
    }
    if (!drewIcon) {
        wchar_t initial[2] = {text[0], L'\0'};
        if (ID2D1SolidColorBrush* brush = nullptr;
            SUCCEEDED(context->CreateSolidColorBrush(PillInk(0.88f * opacity), &brush)) &&
            brush) {
            DrawTextIn(context, niriLyricFormat_, initial, iconBox, brush,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            brush->Release();
        }
    }

    if (ID2D1SolidColorBrush* brush = nullptr;
        SUCCEEDED(context->CreateSolidColorBrush(PillInk(0.92f * opacity), &brush)) &&
        brush) {
        const RectF textBox = {iconBox.right + 10.0f, box.rect.top + 1.0f,
                               box.rect.right - 14.0f, box.rect.bottom - 1.0f};
        DrawTextIn(context, format, text, textBox, brush, DWRITE_TEXT_ALIGNMENT_LEADING,
                   DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        brush->Release();
    }
}

void TextRenderer::DrawNiriSearchField(ID2D1DeviceContext2* context, const RoundedBox& box,
                                       float progress, float opacity) {
    if (!context || opacity <= 0.01f) return;
    const island::NiriSearchLayout layout =
        island::NiriSearchLayoutFor(box, progress);
    if (!layout.valid) return;
    const RectF field = layout.field;
    // The open editor is part of the white Niri pill, not a second dark
    // capsule inside it. The native editor paints the text area with the
    // theme's pill colour; this layer owns only the magnifier in the left slot.
    const float radius = (field.bottom - field.top) * 0.5f;
    ID2D1SolidColorBrush* edge = nullptr;
    if (SUCCEEDED(context->CreateSolidColorBrush(
            D2D1::ColorF(0.58f, 0.58f, 0.60f, 0.72f * opacity), &edge)) &&
        edge) {
        const D2D1_ROUNDED_RECT outline = {
            D2D1::RectF(field.left, field.top, field.right, field.bottom),
            radius, radius};
        context->DrawRoundedRectangle(&outline, edge, 1.2f);
        edge->Release();
    }
    ID2D1SolidColorBrush* ink = nullptr;
    if (FAILED(context->CreateSolidColorBrush(PillInk(0.92f * opacity), &ink)) ||
        !ink) {
        return;
    }
    const float centreY = (field.top + field.bottom) * 0.5f;
    const float searchX = field.left + island::kNiriSearchIconCentreDip;
    context->DrawEllipse(
        D2D1::Ellipse(D2D1::Point2F(searchX, centreY - 1.0f), 5.5f, 5.5f), ink, 1.8f);
    context->DrawLine(D2D1::Point2F(searchX + 4.0f, centreY + 3.0f),
                      D2D1::Point2F(searchX + 8.5f, centreY + 7.5f), ink, 1.8f);
    ink->Release();
}

void TextRenderer::DrawNiriSearchResults(ID2D1DeviceContext2* context,
                                         const RoundedBox& island,
                                         const NiriSearchResultsText& text, float reveal,
                                         float opacity) {
    if (!context || opacity <= 0.01f) return;
    const int count = std::clamp(text.count, 0, island::kStartSearchMaxResults);
    const island::NiriSearchResultsLayout layout =
        island::NiriSearchResultsLayoutFor(island, count, text.pending, reveal);
    if (!layout.valid) return;

    FillCardSurface(context, layout.panel, opacity, reveal < 0.999f);
    const auto drawText = [&](IDWriteTextFormat* format, const wchar_t* line,
                              const RectF& box, const D2D1_COLOR_F& colour,
                              DWRITE_TEXT_ALIGNMENT horizontal,
                              DWRITE_PARAGRAPH_ALIGNMENT vertical) {
        if (!format || !line || line[0] == L'\0' || box.right <= box.left) return;
        if (ID2D1SolidColorBrush* brush = BrushFor(context, colour)) {
            DrawTextIn(context, format, line, box, brush, horizontal, vertical);
        }
    };
    context->PushAxisAlignedClip(
        D2D1::RectF(layout.panel.rect.left, layout.panel.rect.top,
                    layout.panel.rect.right, layout.panel.rect.bottom),
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    if (count == 0) {
        const wchar_t* message =
            (text.pending || !text.filesComplete)
                ? L"\u6B63\u5728\u641C\u7D22\u5E94\u7528\u3001\u8BBE\u7F6E\u548C\u6587\u4EF6\u2026"
                : L"\u6CA1\u6709\u627E\u5230\u5339\u914D\u9879";
        drawText(startRowFormat_, message, layout.status, CardInk(0.56f * opacity),
                 DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        context->PopAxisAlignedClip();
        return;
    }

    for (int i = 0; i < count && text.results; ++i) {
        const island::StartSearchResult& result = text.results[i];
        const RectF& row = layout.rows[i];
        const bool active = i == text.selected;
        const bool hovered = i == text.hovered;
        if (active || hovered) {
            const float strength = active ? 0.10f : 0.055f;
            FillRounded(context, row, 9.0f, CardInk(strength * opacity));
        }

        const float iconSide = 22.0f;
        const float iconTop = row.top + (row.bottom - row.top - iconSide) * 0.5f;
        const RectF iconBox = {row.left + 8.0f, iconTop, row.left + 8.0f + iconSide,
                               iconTop + iconSide};
        const uint8_t* png = result.iconPng.empty() ? nullptr : result.iconPng.data();
        size_t pngSize = result.iconPng.size();
        if (result.kind == island::SearchResultKind::Application && text.listIcons &&
            result.sourceIndex >= 0) {
            png = text.listIcons[result.sourceIndex];
            pngSize = text.listIconSizes ? text.listIconSizes[result.sourceIndex] : 0;
        }
        ID2D1Bitmap1* icon = StartBitmapFor(context, &startListBitmaps_[i], png, pngSize);
        if (icon) {
            context->DrawBitmap(
                icon, D2D1::RectF(iconBox.left, iconBox.top, iconBox.right, iconBox.bottom),
                opacity, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        } else {
            FillRounded(context, iconBox, 5.0f, CardInk(0.055f * opacity));
            wchar_t initial[2] = {result.title.empty() ? L'?' : result.title.front(), L'\0'};
            drawText(startCaptionFormat_, initial, iconBox, CardInk(0.68f * opacity),
                     DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        const float subtitleWidth = 88.0f;
        drawText(startRowFormat_, result.title.c_str(),
                 {iconBox.right + 10.0f, row.top, row.right - subtitleWidth - 8.0f,
                  row.bottom},
                 CardInk((active ? 0.92f : 0.76f) * opacity),
                 DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        drawText(startCaptionFormat_, result.subtitle.c_str(),
                 {row.right - subtitleWidth, row.top, row.right - 4.0f, row.bottom},
                 CardInk((active ? 0.58f : 0.40f) * opacity),
                 DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    context->PopAxisAlignedClip();
}

void TextRenderer::DrawNiriActiveApp(ID2D1DeviceContext2* context, const RoundedBox& box,
                                     ID2D1Bitmap1* icon, const wchar_t* title, float opacity) {
    if (!context || opacity <= 0.01f) return;
    FillRounded(context, box.rect, box.radiusX,
                D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.92f * opacity));
    const float centreY = (box.rect.top + box.rect.bottom) * 0.5f;
    const float iconSize = 24.0f;
    const RectF iconBox = {box.rect.left + 8.0f, centreY - iconSize * 0.5f,
                           box.rect.left + 8.0f + iconSize,
                           centreY + iconSize * 0.5f};
    if (ID2D1SolidColorBrush* backing =
            BrushFor(context, PillInk(0.14f * opacity))) {
        context->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F((iconBox.left + iconBox.right) * 0.5f, centreY),
                          iconSize * 0.5f, iconSize * 0.5f),
            backing);
    }
    if (icon) {
        context->DrawBitmap(icon, D2D1::RectF(iconBox.left, iconBox.top, iconBox.right,
                                             iconBox.bottom),
                            opacity, D2D1_INTERPOLATION_MODE_LINEAR);
    } else {
        FillRounded(context, iconBox, 5.0f,
                    D2D1::ColorF(0.18f, 0.78f, 0.48f, 0.92f * opacity));
    }
    ID2D1SolidColorBrush* ink = nullptr;
    context->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.11f, 0.16f, 0.92f * opacity), &ink);
    if (ink && appRowFormat_) {
        DrawTextIn(context, appRowFormat_, title && title[0] ? title : L"桌面",
                   {iconBox.right + 7.0f, box.rect.top, box.rect.right - 8.0f,
                    box.rect.bottom},
                   ink, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (ink) ink->Release();
}

void TextRenderer::DrawCardRail(ID2D1DeviceContext2* context, const RoundedBox& card,
                                int selectedPage, const CardRailText& text, float opacity,
                                bool moving) {
    if (!context || opacity <= 0.01f || !niriMetricFormat_) return;
    FillCardSurface(context, card, opacity, moving);
    const float handleCentre = (card.rect.left + card.rect.right) * 0.5f;
    FillRounded(context,
                {handleCentre - 28.0f, card.rect.top + 8.0f,
                 handleCentre + 28.0f, card.rect.top + 14.0f},
                3.0f, D2D1::ColorF(0.75f, 0.72f, 0.82f, 0.55f * opacity));
    const island::CardRailLayout rail = island::CardRailFor(card);
    if (!rail.valid) return;
    const RectF middleDock = {rail.frame.left + 4.0f, rail.nav[0].top - 6.0f,
                              rail.frame.right - 4.0f,
                              rail.nav[island::kCardRailNavCount - 1].bottom + 6.0f};
    FillRounded(context, middleDock, 16.0f,
                D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.72f * opacity));
    const RectF powerDock = {rail.frame.left + 4.0f, rail.settings.top - 6.0f,
                             rail.frame.right - 4.0f, rail.shutdown.bottom + 6.0f};
    FillRounded(context, powerDock, 16.0f,
                D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.72f * opacity));

    const D2D1_COLOR_F purple = D2D1::ColorF(0.42f, 0.32f, 0.68f, opacity);
    const D2D1_COLOR_F muted = D2D1::ColorF(0.46f, 0.43f, 0.54f, 0.88f * opacity);
    ID2D1SolidColorBrush* purpleBrush = BrushFor(context, purple);
    ID2D1SolidColorBrush* mutedBrush = BrushFor(context, muted);
    if (!purpleBrush || !mutedBrush) return;

    const auto selectedSlot = [selectedPage](int slot) {
        if (selectedPage < 0) return slot == 0;
        if (slot == 1) return selectedPage == static_cast<int>(island::CardTab::DefaultApps);
        if (slot == 2) return selectedPage == static_cast<int>(island::CardTab::TrayApps);
        return slot == 3 && selectedPage == static_cast<int>(island::CardTab::System);
    };
    for (int i = 0; i < island::kCardRailNavCount; ++i) {
        const bool selected = selectedSlot(i);
        if (selected) {
            FillRounded(context, rail.nav[i], 11.0f,
                        D2D1::ColorF(0.42f, 0.32f, 0.68f, 0.98f * opacity));
        }
        ID2D1Brush* mark = selected ? BrushFor(context, D2D1::ColorF(1.0f, 1.0f, 1.0f, opacity))
                                    : purpleBrush;
        const RectF& box = rail.nav[i];
        const float cx = (box.left + box.right) * 0.5f;
        const float cy = (box.top + box.bottom) * 0.5f;
        if (i == 0) {
            context->DrawLine(D2D1::Point2F(cx - 7.0f, cy), D2D1::Point2F(cx, cy - 7.0f),
                              mark, 1.8f);
            context->DrawLine(D2D1::Point2F(cx, cy - 7.0f), D2D1::Point2F(cx + 7.0f, cy),
                              mark, 1.8f);
            context->DrawLine(D2D1::Point2F(cx - 5.0f, cy - 2.0f),
                              D2D1::Point2F(cx - 5.0f, cy + 7.0f), mark, 1.8f);
            context->DrawLine(D2D1::Point2F(cx + 5.0f, cy - 2.0f),
                              D2D1::Point2F(cx + 5.0f, cy + 7.0f), mark, 1.8f);
        } else if (i == 1) {
            constexpr float side = 5.0f;
            for (int q = 0; q < 4; ++q) {
                const float left = cx + (q % 2 ? 1.5f : -7.5f);
                const float top = cy + (q >= 2 ? 1.5f : -7.5f);
                context->DrawRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(left, top, left + side, top + side), 1.2f,
                                      1.2f),
                    mark, 1.4f);
            }
        } else if (i == 2) {
            context->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(cx - 8.0f, cy - 6.0f, cx + 8.0f, cy + 7.0f),
                                  3.0f, 3.0f),
                mark, 1.6f);
            context->DrawLine(D2D1::Point2F(cx - 7.0f, cy - 1.0f),
                              D2D1::Point2F(cx + 7.0f, cy - 1.0f), mark, 1.4f);
        } else {
            // The system page is a set of controls, not a second settings
            // destination. Three slider tracks distinguish it from the gear-like
            // settings mark in the lower dock.
            constexpr float knobX[3] = {-3.5f, 3.0f, -0.5f};
            for (int row = 0; row < 3; ++row) {
                const float y = cy - 6.0f + static_cast<float>(row) * 6.0f;
                context->DrawLine(D2D1::Point2F(cx - 8.0f, y), D2D1::Point2F(cx + 8.0f, y),
                                  mark, 1.4f);
                context->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(cx + knobX[row], y), 2.2f, 2.2f), mark);
            }
        }
    }

    const auto drawPowerButton = [&](const RectF& box, bool restart, bool armed,
                                     ID2D1Brush* brush, float dx) {
        if (armed) {
            FillRounded(context, box, 10.0f,
                        D2D1::ColorF(0.93f, 0.31f, 0.30f, 0.16f * opacity));
        }
        const float cx = (box.left + box.right) * 0.5f;
        const float cy = (box.top + box.bottom) * 0.5f;
        if (restart) {
            context->DrawLine(D2D1::Point2F(cx - 6.0f + dx, cy - 3.0f),
                              D2D1::Point2F(cx - 2.0f + dx, cy - 7.0f), brush, 1.7f);
            context->DrawLine(D2D1::Point2F(cx - 2.0f + dx, cy - 7.0f),
                              D2D1::Point2F(cx + 4.0f + dx, cy - 6.0f), brush, 1.7f);
            context->DrawLine(D2D1::Point2F(cx + 4.0f + dx, cy - 6.0f),
                              D2D1::Point2F(cx + 7.0f + dx, cy - 1.0f), brush, 1.7f);
            context->DrawLine(D2D1::Point2F(cx + 7.0f + dx, cy - 1.0f),
                              D2D1::Point2F(cx + 5.0f + dx, cy + 5.0f), brush, 1.7f);
            context->DrawLine(D2D1::Point2F(cx + 5.0f + dx, cy + 5.0f),
                              D2D1::Point2F(cx - 1.0f + dx, cy + 7.0f), brush, 1.7f);
            context->DrawLine(D2D1::Point2F(cx - 6.0f + dx, cy - 3.0f),
                              D2D1::Point2F(cx - 7.0f + dx, cy - 8.0f), brush, 1.7f);
            context->DrawLine(D2D1::Point2F(cx - 6.0f + dx, cy - 3.0f),
                              D2D1::Point2F(cx - 1.0f + dx, cy - 2.0f), brush, 1.7f);
        } else {
            context->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx + dx, cy), 7.0f, 7.0f),
                                 brush, 1.7f);
            context->DrawLine(D2D1::Point2F(cx + dx, cy - 8.0f),
                              D2D1::Point2F(cx + dx, cy - 1.0f), brush, 1.7f);
        }
    };
    const auto drawSettingsButton = [&](const RectF& box, ID2D1Brush* brush, float dx) {
        const float cx = (box.left + box.right) * 0.5f;
        const float cy = (box.top + box.bottom) * 0.5f;
        context->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx + dx, cy), 4.2f, 4.2f),
                             brush, 1.5f);
        for (int spoke = 0; spoke < 8; ++spoke) {
            const float angle = static_cast<float>(spoke) * 0.7853982f;
            context->DrawLine(
                D2D1::Point2F(cx + dx + std::cos(angle) * 6.0f,
                              cy + std::sin(angle) * 6.0f),
                D2D1::Point2F(cx + dx + std::cos(angle) * 8.0f,
                              cy + std::sin(angle) * 8.0f),
                brush, 1.3f);
        }
    };
    float confirmProgress = std::clamp(text.powerConfirmProgress, 0.0f, 1.0f);
    confirmProgress = confirmProgress * confirmProgress * (3.0f - 2.0f * confirmProgress);
    if (confirmProgress < 0.999f) {
        ID2D1Brush* fading = mutedBrush;
        if (confirmProgress > 0.001f) {
            fading = BrushFor(
                context, D2D1::ColorF(0.46f, 0.43f, 0.54f,
                                     0.88f * (1.0f - confirmProgress) * opacity));
        }
        drawSettingsButton(rail.settings, fading, -5.0f * confirmProgress);
        drawPowerButton(rail.restart, true, text.powerConfirmButton == 2, fading,
                        -5.0f * confirmProgress);
        drawPowerButton(rail.shutdown, false, text.powerConfirmButton == 3, fading,
                        -5.0f * confirmProgress);
    }
    if (confirmProgress > 0.005f &&
        (text.powerConfirmShownButton == 2 || text.powerConfirmShownButton == 3)) {
        RectF confirm = island::CardRailPowerConfirmFor(rail);
        const float centreY = (confirm.top + confirm.bottom) * 0.5f;
        const float halfHeight = (confirm.bottom - confirm.top) * 0.5f * confirmProgress;
        confirm.top = centreY - halfHeight;
        confirm.bottom = centreY + halfHeight;
        FillRounded(context, confirm, 12.0f,
                    D2D1::ColorF(0.93f, 0.31f, 0.30f,
                                 (0.10f + 0.06f * confirmProgress) * opacity));
        if (ID2D1SolidColorBrush* edge =
                BrushFor(context, D2D1::ColorF(0.88f, 0.26f, 0.27f,
                                               0.26f * confirmProgress * opacity))) {
            const D2D1_ROUNDED_RECT outline = {
                D2D1::RectF(confirm.left, confirm.top, confirm.right, confirm.bottom), 12.0f,
                12.0f};
            context->DrawRoundedRectangle(&outline, edge, 1.0f);
        }
        const wchar_t* action = text.powerConfirmShownButton == 2 ? L"\u91CD\u542F"
                                                                  : L"\u5173\u673A";
        const float textOffsetY = (1.0f - confirmProgress) * 4.0f;
        const float rowsTop = centreY - 18.0f + textOffsetY;
        const RectF prompt = {confirm.left, rowsTop, confirm.right, rowsTop + 16.0f};
        const RectF actionBox = {confirm.left, rowsTop + 18.0f, confirm.right,
                                 rowsTop + 38.0f};
        if (ID2D1SolidColorBrush* promptInk =
                BrushFor(context, D2D1::ColorF(0.57f, 0.28f, 0.31f,
                                               0.88f * confirmProgress * opacity))) {
            DrawTextIn(context, niriWeatherQualityFormat_, L"\u518D\u6B21\u70B9\u51FB", prompt,
                       promptInk, DWRITE_TEXT_ALIGNMENT_CENTER,
                       DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (ID2D1SolidColorBrush* actionInk =
                BrushFor(context, D2D1::ColorF(0.78f, 0.18f, 0.21f,
                                               opacity * confirmProgress))) {
            DrawTextIn(context, niriMetricFormat_, action, actionBox, actionInk,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }

    ID2D1Bitmap1* avatar =
        StartBitmapFor(context, &startAvatarBitmap_, text.avatarPng, text.avatarPngSize);
    if (avatar) {
        ID2D1EllipseGeometry* circle = nullptr;
        ID2D1Factory* factory = nullptr;
        context->GetFactory(&factory);
        if (factory) {
            const float cx = (rail.avatar.left + rail.avatar.right) * 0.5f;
            const float cy = (rail.avatar.top + rail.avatar.bottom) * 0.5f;
            const float radius = (rail.avatar.right - rail.avatar.left) * 0.5f;
            factory->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius),
                                           &circle);
            factory->Release();
        }
        if (circle) {
            context->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), circle,
                                                      D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                               nullptr);
            context->DrawBitmap(avatar,
                                D2D1::RectF(rail.avatar.left, rail.avatar.top, rail.avatar.right,
                                            rail.avatar.bottom),
                                opacity, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            context->PopLayer();
            circle->Release();
        }
    } else {
        context->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F((rail.avatar.left + rail.avatar.right) * 0.5f,
                                        (rail.avatar.top + rail.avatar.bottom) * 0.5f),
                          (rail.avatar.right - rail.avatar.left) * 0.5f,
                          (rail.avatar.bottom - rail.avatar.top) * 0.5f),
            BrushFor(context, D2D1::ColorF(0.82f, 0.78f, 0.89f, opacity)));
        wchar_t initial[2] = {
            text.accountName && text.accountName[0] ? text.accountName[0] : L'U', L'\0'};
        DrawTextIn(context, niriMetricFormat_, initial, rail.avatar, purpleBrush,
                   DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
}

void TextRenderer::DrawWeatherCard(ID2D1DeviceContext2* context, const RoundedBox& card,
                                   const WeatherSnapshot& weather, float opacity) {
    if (!context || opacity <= 0.01f || !niriMetricFormat_) return;
    const WeatherCardLayout layout = WeatherCardFor(card);
    const D2D1_COLOR_F lavender = D2D1::ColorF(0.86f, 0.81f, 0.98f, 0.96f * opacity);
    const D2D1_COLOR_F white = D2D1::ColorF(0.96f, 0.95f, 0.98f, 0.98f * opacity);
    FillRounded(context, layout.current, 22.0f, lavender);
    FillRounded(context, layout.metrics, 20.0f, white);
    FillRounded(context, layout.hourly, 20.0f, white);
    FillRounded(context, layout.daily, 20.0f, white);

    ID2D1SolidColorBrush* ink = nullptr;
    ID2D1SolidColorBrush* muted = nullptr;
    context->CreateSolidColorBrush(D2D1::ColorF(0.13f, 0.11f, 0.18f, 0.96f * opacity), &ink);
    context->CreateSolidColorBrush(D2D1::ColorF(0.38f, 0.35f, 0.45f, 0.88f * opacity), &muted);
    if (!ink) return;
    if (!weather.valid) {
        DrawTextIn(context, niriHeroFormat_, L"天气数据加载中",
                   {layout.current.left + 22.0f, layout.current.top,
                    layout.current.right - 22.0f, layout.current.bottom},
                   ink, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        ink->Release();
        if (muted) muted->Release();
        return;
    }

    wchar_t temperature[32] = {};
    std::swprintf(temperature, 32, L"%d°",
                  static_cast<int>(std::lround(weather.temperatureC)));
    DrawTextIn(context, niriHeroFormat_, temperature,
               {layout.current.left + 22.0f, layout.current.top + 4.0f,
                layout.current.left + 214.0f, layout.current.top + 88.0f},
               ink, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    const wchar_t* word = WeatherWordForCode(weather.code);
    DrawTextIn(context, niriMetricFormat_, word ? word : L"未知",
               {layout.current.left + 24.0f, layout.current.top + 92.0f,
                layout.current.left + 220.0f, layout.current.top + 116.0f},
               ink, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    wchar_t apparent[96] = {};
    std::swprintf(apparent, 96, L"体感 %d°", static_cast<int>(std::lround(weather.apparentTemperatureC)));
    DrawTextIn(context, niriMetricFormat_, apparent,
               {layout.current.left + 24.0f, layout.current.top + 118.0f,
                layout.current.left + 250.0f, layout.current.top + 142.0f},
               muted ? muted : ink, DWRITE_TEXT_ALIGNMENT_LEADING,
               DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    wchar_t sun[96] = {};
    std::swprintf(sun, 96, L"日出 %02d:%02d   日落 %02d:%02d", weather.sunriseMinutes / 60,
                  weather.sunriseMinutes % 60, weather.sunsetMinutes / 60,
                  weather.sunsetMinutes % 60);
    DrawTextIn(context, niriMetricFormat_, sun,
               {layout.current.left + 24.0f, layout.current.bottom - 34.0f,
                layout.current.right - 130.0f, layout.current.bottom - 10.0f},
               muted ? muted : ink, DWRITE_TEXT_ALIGNMENT_LEADING,
               DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    DrawWeatherMarkLarge(context,
                         {layout.current.right - 118.0f, layout.current.top + 18.0f,
                          layout.current.right - 22.0f, layout.current.bottom - 18.0f},
                         weather.code, opacity);

    struct Metric {
        const wchar_t* label;
        wchar_t value[48];
    };
    Metric metrics[6] = {
        {L"湿度", {}}, {L"风速", {}}, {L"气压", {}},
        {L"降水概率", {}}, {L"日出", {}}, {L"日落", {}},
    };
    std::swprintf(metrics[0].value, 48, L"%d%%", weather.humidityPercent);
    std::swprintf(metrics[1].value, 48, L"%.1f m/s", weather.windSpeedKph / 3.6);
    std::swprintf(metrics[2].value, 48, L"%d hPa", weather.pressureHpa);
    std::swprintf(metrics[3].value, 48, L"%d%%", weather.precipitationPercent);
    std::swprintf(metrics[4].value, 48, L"%02d:%02d", weather.sunriseMinutes / 60,
                  weather.sunriseMinutes % 60);
    std::swprintf(metrics[5].value, 48, L"%02d:%02d", weather.sunsetMinutes / 60,
                  weather.sunsetMinutes % 60);
    const float metricLeft = layout.metrics.left + 18.0f;
    const float metricTop = layout.metrics.top + 18.0f;
    const float metricWidth = (layout.metrics.right - layout.metrics.left - 36.0f) / 2.0f;
    const float metricHeight = (layout.metrics.bottom - layout.metrics.top - 36.0f) / 3.0f;
    for (int i = 0; i < 6; ++i) {
        const int column = i % 2;
        const int row = i / 2;
        const RectF box = {metricLeft + static_cast<float>(column) * metricWidth,
                           metricTop + static_cast<float>(row) * metricHeight,
                           metricLeft + static_cast<float>(column + 1) * metricWidth,
                           metricTop + static_cast<float>(row + 1) * metricHeight};
        DrawTextIn(context, niriMetricFormat_, metrics[i].label, box,
                   muted ? muted : ink, DWRITE_TEXT_ALIGNMENT_LEADING,
                   DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        DrawTextIn(context, niriMetricFormat_, metrics[i].value,
                   {box.left, box.top + 17.0f, box.right, box.bottom}, ink,
                   DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }

    DrawTextIn(context, niriMetricFormat_, L"每小时",
               {layout.hourly.left + 16.0f, layout.hourly.top + 8.0f,
                layout.hourly.right - 16.0f, layout.hourly.top + 36.0f},
               ink, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    for (int i = 0; i < kWeatherHourlyColumns && i < weather.hourCount; ++i) {
        const RectF& cell = layout.hourlyCells[i];
        if (i == 0) FillRounded(context, cell, 13.0f, D2D1::ColorF(0.88f, 0.85f, 0.98f,
                                                                    0.78f * opacity));
        wchar_t hour[16] = {};
        std::swprintf(hour, 16, L"%02d:00", weather.hours[i].hour);
        DrawTextIn(context, niriMetricFormat_, hour,
                   {cell.left, cell.top + 6.0f, cell.right, cell.top + 28.0f},
                   i == 0 && muted ? muted : ink, DWRITE_TEXT_ALIGNMENT_CENTER,
                   DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        DrawWeatherMarkLarge(
            context, {cell.left + 18.0f, cell.top + 34.0f, cell.right - 18.0f,
                      cell.top + 86.0f},
            weather.hours[i].code, opacity);
        wchar_t value[48] = {};
        std::swprintf(value, 48, L"%d°", static_cast<int>(std::lround(weather.hours[i].temperatureC)));
        DrawTextIn(context, niriMetricFormat_, value,
                   {cell.left, cell.top + 88.0f, cell.right, cell.top + 112.0f},
                   ink, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        std::swprintf(value, 48, L"%d%%", weather.hours[i].precipitationPercent);
        DrawTextIn(context, niriMetricFormat_, value,
                   {cell.left, cell.bottom - 25.0f, cell.right, cell.bottom - 5.0f},
                   muted ? muted : ink, DWRITE_TEXT_ALIGNMENT_CENTER,
                   DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    DrawTextIn(context, niriMetricFormat_, L"每日",
               {layout.daily.left + 16.0f, layout.daily.top + 8.0f,
                layout.daily.right - 16.0f, layout.daily.top + 36.0f},
               ink, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    for (int i = 0; i < weather.dayCount && i < kWeatherDayCount; ++i) {
        const RectF& cell = layout.dailyCells[i];
        const int weekday = DayOfWeekMondayFirst(weather.days[i].year, weather.days[i].month,
                                                 weather.days[i].day);
        DrawTextIn(context, niriMetricFormat_, kWeekdays[(weekday + 1) % 7],
                   {cell.left, cell.top + 4.0f, cell.right, cell.top + 28.0f},
                   ink, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        DrawWeatherMarkLarge(context,
                             {cell.left + 20.0f, cell.top + 34.0f, cell.right - 20.0f,
                              cell.top + 88.0f},
                             weather.days[i].code, opacity);
        wchar_t maximum[32] = {};
        std::swprintf(maximum, 32, L"%d°",
                      static_cast<int>(std::lround(weather.days[i].temperatureMaxC)));
        DrawTextIn(context, niriMetricFormat_, maximum,
                   {cell.left, cell.top + 94.0f, cell.right, cell.top + 116.0f},
                   ink, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        wchar_t minimum[32] = {};
        std::swprintf(minimum, 32, L"%d°",
                      static_cast<int>(std::lround(weather.days[i].temperatureMinC)));
        DrawTextIn(context, niriMetricFormat_, minimum,
                   {cell.left, cell.top + 118.0f, cell.right, cell.top + 140.0f},
                   muted ? muted : ink, DWRITE_TEXT_ALIGNMENT_CENTER,
                   DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    if (muted) muted->Release();
    ink->Release();
}

void TextRenderer::DrawCommandCenter(ID2D1DeviceContext2* context, const RoundedBox& card,
                                     const CommandCenterText& text, float opacity) {
    if (!context || opacity <= 0.01f || !niriMetricFormat_) return;
    const CommandCenterLayout layout =
        CommandCenterFor(card, text.filteredCount, text.scrollFirst);
    const D2D1_COLOR_F white = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f * opacity);
    const D2D1_COLOR_F inkColour = D2D1::ColorF(45.0f / 255.0f, 38.0f / 255.0f,
                                                75.0f / 255.0f, 0.97f * opacity);
    const D2D1_COLOR_F mutedColour = D2D1::ColorF(111.0f / 255.0f, 105.0f / 255.0f,
                                                  130.0f / 255.0f, 0.88f * opacity);
    const D2D1_COLOR_F accentColour = D2D1::ColorF(107.0f / 255.0f, 82.0f / 255.0f,
                                                   174.0f / 255.0f, 0.98f * opacity);
    const D2D1_COLOR_F cardBorder = D2D1::ColorF(235.0f / 255.0f, 230.0f / 255.0f,
                                                  245.0f / 255.0f, 0.70f * opacity);
    const D2D1_COLOR_F mediaBorder = D2D1::ColorF(233.0f / 255.0f, 213.0f / 255.0f,
                                                   255.0f / 255.0f, 0.60f * opacity);
    ID2D1SolidColorBrush* ink = BrushFor(context, inkColour);
    ID2D1SolidColorBrush* muted = BrushFor(context, mutedColour);
    ID2D1SolidColorBrush* accent = BrushFor(context, accentColour);
    if (!ink || !muted || !accent) return;

    const auto strokeRounded = [&](const RectF& rect, float radius, const D2D1_COLOR_F& colour,
                                   float width) {
        if (ID2D1SolidColorBrush* brush = BrushFor(context, colour)) {
            context->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom),
                                  radius, radius),
                brush, width);
        }
    };
    const auto drawText = [&](IDWriteTextFormat* format, const wchar_t* value, const RectF& where,
                              ID2D1Brush* brush, DWRITE_TEXT_ALIGNMENT horizontal,
                              DWRITE_PARAGRAPH_ALIGNMENT vertical) {
        DrawTextIn(context, format, value, where, brush, horizontal, vertical);
    };
    const auto softShadow = [&](const RectF& box, float radius) {
        constexpr float spread[] = {18.0f, 12.0f, 7.0f, 3.0f};
        constexpr float alpha[] = {0.010f, 0.014f, 0.020f, 0.022f};
        for (int i = 0; i < 4; ++i) {
            const float d = spread[i];
            FillRounded(context,
                        {box.left - d, box.top + 2.0f, box.right + d, box.bottom + d + 2.0f},
                        radius + d * 0.35f,
                        D2D1::ColorF(107.0f / 255.0f, 82.0f / 255.0f, 174.0f / 255.0f,
                                     alpha[i] * opacity));
        }
    };

    // The hero clock is a tinted focal card, exactly as in the reference.
    const D2D1_GRADIENT_STOP clockStops[] = {
        {0.0f, D2D1::ColorF(241.0f / 255.0f, 236.0f / 255.0f, 252.0f / 255.0f,
                            0.98f * opacity)},
        {0.52f, D2D1::ColorF(247.0f / 255.0f, 244.0f / 255.0f, 254.0f / 255.0f,
                             0.98f * opacity)},
        {1.0f, D2D1::ColorF(236.0f / 255.0f, 229.0f / 255.0f, 249.0f / 255.0f,
                            0.98f * opacity)},
    };
    ID2D1GradientStopCollection* clockGradientStops = nullptr;
    ID2D1LinearGradientBrush* clockGradient = nullptr;
    if (SUCCEEDED(context->CreateGradientStopCollection(
            clockStops, ARRAYSIZE(clockStops), D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP,
            &clockGradientStops)) &&
        clockGradientStops &&
        SUCCEEDED(context->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(layout.clock.left, layout.clock.top),
                D2D1::Point2F(layout.clock.right, layout.clock.bottom)),
            clockGradientStops, &clockGradient)) &&
        clockGradient) {
        const D2D1_ROUNDED_RECT rounded = {
            D2D1::RectF(layout.clock.left, layout.clock.top, layout.clock.right,
                        layout.clock.bottom),
            24.0f, 24.0f};
        context->FillRoundedRectangle(&rounded, clockGradient);
    }
    SafeRelease(&clockGradient);
    SafeRelease(&clockGradientStops);
    if (ID2D1SolidColorBrush* glow = BrushFor(
            context, D2D1::ColorF(0.42f, 0.32f, 0.68f, 0.07f * opacity))) {
        context->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(layout.clock.right - 6.0f,
                                       layout.clock.bottom + 4.0f),
                          48.0f, 48.0f),
            glow);
    }
    softShadow(layout.weather, 24.0f);
    FillRounded(context, layout.weather, 24.0f, white);
    softShadow(layout.calendar, 16.0f);
    FillRounded(context, layout.calendar, 16.0f, white);
    if (ID2D1SolidColorBrush* glow = BrushFor(
            context, D2D1::ColorF(107.0f / 255.0f, 82.0f / 255.0f, 174.0f / 255.0f,
                                  0.07f * opacity))) {
        context->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(layout.media.right - 4.0f, layout.media.top + 6.0f),
                          64.0f, 64.0f),
            glow);
        context->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(layout.media.left + 10.0f, layout.media.bottom - 6.0f),
                          44.0f, 44.0f),
            glow);
    }
    softShadow(layout.media, 24.0f);
    FillRounded(context, layout.media, 24.0f, white);
    strokeRounded(layout.weather, 24.0f, cardBorder, 1.0f);
    strokeRounded(layout.calendar, 16.0f, cardBorder, 1.0f);
    strokeRounded(layout.media, 24.0f, mediaBorder, 1.0f);

    Refresh();
    DrawTextIn(context, niriCommandTimeFormat_, timeText_,
               {layout.clock.left + 14.0f, layout.clock.top + 8.0f,
                layout.clock.right - 10.0f, layout.clock.top + 64.0f},
               ink, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    wchar_t date[80] = {};
    std::swprintf(date, 80, L"%ls · %u年%u月%u日", weekdayText_,
                  static_cast<unsigned>(time_.wYear), static_cast<unsigned>(time_.wMonth),
                  static_cast<unsigned>(time_.wDay));
    DrawTextIn(context, niriCommandDateFormat_, date,
               {layout.clock.left + 16.0f, layout.clock.top + 62.0f,
                layout.clock.right - 12.0f, layout.clock.top + 82.0f},
               muted ? muted : ink, DWRITE_TEXT_ALIGNMENT_LEADING,
               DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    FillRounded(context, layout.clockChip, layout.clockChip.bottom - layout.clockChip.top,
                D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.90f * opacity));
    strokeRounded(layout.clockChip, (layout.clockChip.bottom - layout.clockChip.top) * 0.5f,
                  D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f * opacity), 1.0f);
    const float clockChipCentreY = (layout.clockChip.top + layout.clockChip.bottom) * 0.5f;
    context->DrawEllipse(
        D2D1::Ellipse(D2D1::Point2F(layout.clockChip.left + 10.0f, clockChipCentreY),
                      3.2f, 3.2f),
        accent, 1.2f);
    context->DrawLine(D2D1::Point2F(layout.clockChip.left + 10.0f, clockChipCentreY),
                      D2D1::Point2F(layout.clockChip.left + 10.0f, clockChipCentreY - 2.0f),
                      accent, 1.0f);
    DrawTextIn(context, niriCommandChipFormat_, L"\u5317\u4EAC\u65F6\u95F4",
               {layout.clockChip.left + 18.0f, layout.clockChip.top,
                layout.clockChip.right - 6.0f, layout.clockChip.bottom},
               accent, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    const WeatherSnapshot* weather = text.weather;
    if (weather && weather->valid) {
        const RectF liveChip = {layout.weather.right - 62.0f, layout.weather.top + 10.0f,
                                layout.weather.right - 10.0f, layout.weather.top + 30.0f};
        FillRounded(context, liveChip, 10.0f,
                    D2D1::ColorF(236.0f / 255.0f, 253.0f / 255.0f, 245.0f / 255.0f,
                                  0.96f * opacity));
        strokeRounded(liveChip, 10.0f,
                      D2D1::ColorF(167.0f / 255.0f, 243.0f / 255.0f, 208.0f / 255.0f,
                                    0.60f * opacity),
                      1.0f);
        wchar_t airQuality[32] = {};
        if (weather->airQualityIndex >= 0) {
            std::swprintf(airQuality, 32, L"%ls %d", AirQualityWord(weather->airQualityIndex),
                          weather->airQualityIndex);
        } else {
            wcscpy_s(airQuality, L"实时");
        }
        DrawTextIn(context, niriWeatherQualityFormat_, airQuality, liveChip,
                   BrushFor(context, D2D1::ColorF(5.0f / 255.0f, 150.0f / 255.0f,
                                                  105.0f / 255.0f, opacity)),
                   DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        wchar_t temperature[32] = {};
        std::swprintf(temperature, 32, L"%d°",
                      static_cast<int>(std::lround(weather->temperatureC)));
        DrawTextIn(context, niriWeatherTempFormat_, temperature,
                   {layout.weather.left + 12.0f, layout.weather.bottom - 50.0f,
                    layout.weather.right - 10.0f, layout.weather.bottom - 24.0f},
                   ink, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        const wchar_t* word = WeatherWordForCode(weather->code);
        DrawTextIn(context, niriWeatherWordFormat_, word ? word : L"未知",
                   {layout.weather.left + 10.0f, layout.weather.bottom - 23.0f,
                    layout.weather.right - 10.0f, layout.weather.bottom - 5.0f},
                   muted ? muted : ink, DWRITE_TEXT_ALIGNMENT_CENTER,
                   DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        const float weatherCentreX = (layout.weather.left + layout.weather.right) * 0.5f;
        const RectF weatherBadge = {weatherCentreX - 20.0f, layout.weather.top + 42.0f,
                                    weatherCentreX + 20.0f, layout.weather.top + 82.0f};
        FillRounded(context, weatherBadge, 16.0f,
                    D2D1::ColorF(253.0f / 255.0f, 230.0f / 255.0f, 138.0f / 255.0f,
                                  0.58f * opacity));
        DrawWeatherMarkLarge(context,
                             {weatherBadge.left + 9.0f, weatherBadge.top + 9.0f,
                              weatherBadge.right - 9.0f, weatherBadge.bottom - 9.0f},
                             weather->code, opacity);
    } else {
        DrawTextIn(context, niriMetricFormat_, L"天气暂无数据", layout.weather,
                   muted ? muted : ink, DWRITE_TEXT_ALIGNMENT_CENTER,
                   DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    FillRounded(context, layout.calendarIcon, 6.0f,
                D2D1::ColorF(245.0f / 255.0f, 243.0f / 255.0f, 250.0f / 255.0f,
                              0.96f * opacity));
    const float calendarIconCx = (layout.calendarIcon.left + layout.calendarIcon.right) * 0.5f;
    const float calendarIconCy = (layout.calendarIcon.top + layout.calendarIcon.bottom) * 0.5f;
    context->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(calendarIconCx - 5.0f, calendarIconCy - 5.0f,
                                     calendarIconCx + 5.0f, calendarIconCy + 5.0f),
                          1.5f, 1.5f),
        accent, 1.0f);
    context->DrawLine(D2D1::Point2F(calendarIconCx - 5.0f, calendarIconCy - 1.5f),
                      D2D1::Point2F(calendarIconCx + 5.0f, calendarIconCy - 1.5f), accent, 1.0f);
    wchar_t monthTitle[64] = {};
    std::swprintf(monthTitle, 64, L"%u年%u月", static_cast<unsigned>(time_.wYear),
                  static_cast<unsigned>(time_.wMonth));
    DrawTextIn(context, niriNotificationTitleFormat_, monthTitle, layout.calendarTitle, ink,
               DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    DrawTextIn(context, niriMetricFormat_, L"‹", layout.calendarPrevious,
               accent ? accent : ink, DWRITE_TEXT_ALIGNMENT_CENTER,
               DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    DrawTextIn(context, niriMetricFormat_, L"›", layout.calendarNext,
               accent ? accent : ink, DWRITE_TEXT_ALIGNMENT_CENTER,
               DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    const wchar_t shortWeekdays[7][3] = {L"一", L"二", L"三", L"四", L"五", L"六", L"日"};
    for (int column = 0; column < 7; ++column) {
        DrawTextIn(context, niriCalendarWeekdayFormat_, shortWeekdays[column],
                   layout.calendarCells[column], muted ? muted : ink,
                   DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    const int first = DayOfWeekMondayFirst(time_.wYear, time_.wMonth, 1);
    const int days = DaysInMonth(time_.wYear, time_.wMonth);
    for (int row = 1; row < kCommandCenterCalendarRows; ++row) {
        for (int column = 0; column < 7; ++column) {
            const int index = (row - 1) * 7 + column;
            const int day = index - first + 1;
            RectF cell = layout.calendarCells[row * 7 + column];
            if (day < 1 || day > days) continue;
            if (day == time_.wDay) {
                FillRounded(context, cell, 9.0f,
                            D2D1::ColorF(107.0f / 255.0f, 82.0f / 255.0f,
                                          174.0f / 255.0f, 0.98f * opacity));
            }
            wchar_t dayText[8] = {};
            std::swprintf(dayText, 8, L"%d", day);
            ID2D1Brush* dayInk =
                day == time_.wDay ? BrushFor(context, ThemeHighlight(0.96f * opacity)) : ink;
            DrawTextIn(context, niriCalendarDayFormat_, dayText, cell,
                       dayInk,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }

    if (text.music && text.music->present) {
        const MusicInfo& music = *text.music;
        ID2D1Bitmap1* artwork =
            StartBitmapFor(context, &commandArtworkBitmap_, music.artwork, music.artworkSize);
        const D2D1_GRADIENT_STOP coverStops[] = {
            {0.0f, D2D1::ColorF(83.0f / 255.0f, 57.0f / 255.0f, 148.0f / 255.0f, opacity)},
            {0.52f, D2D1::ColorF(107.0f / 255.0f, 82.0f / 255.0f, 174.0f / 255.0f, opacity)},
            {1.0f, D2D1::ColorF(213.0f / 255.0f, 199.0f / 255.0f, 238.0f / 255.0f, opacity)},
        };
        ID2D1GradientStopCollection* coverGradientStops = nullptr;
        ID2D1LinearGradientBrush* coverGradient = nullptr;
        if (SUCCEEDED(context->CreateGradientStopCollection(
                coverStops, ARRAYSIZE(coverStops), D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP,
                &coverGradientStops)) &&
            coverGradientStops &&
            SUCCEEDED(context->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(layout.mediaCover.left, layout.mediaCover.bottom),
                    D2D1::Point2F(layout.mediaCover.right, layout.mediaCover.top)),
                coverGradientStops, &coverGradient)) &&
            coverGradient) {
            const D2D1_ROUNDED_RECT rounded = {
                D2D1::RectF(layout.mediaCover.left, layout.mediaCover.top,
                            layout.mediaCover.right, layout.mediaCover.bottom),
                16.0f, 16.0f};
            context->FillRoundedRectangle(&rounded, coverGradient);
        }
        SafeRelease(&coverGradient);
        SafeRelease(&coverGradientStops);

        const RectF artworkBox = {layout.mediaCover.left + 2.0f, layout.mediaCover.top + 2.0f,
                                  layout.mediaCover.right - 2.0f,
                                  layout.mediaCover.bottom - 2.0f};
        if (artwork) {
            ID2D1RoundedRectangleGeometry* clip = nullptr;
            ID2D1Factory* factory = nullptr;
            context->GetFactory(&factory);
            if (factory) {
                factory->CreateRoundedRectangleGeometry(
                    D2D1::RoundedRect(
                        D2D1::RectF(artworkBox.left, artworkBox.top, artworkBox.right,
                                    artworkBox.bottom),
                        14.0f, 14.0f),
                    &clip);
                factory->Release();
            }
            if (clip) {
                context->PushLayer(
                    D2D1::LayerParameters1(D2D1::InfiniteRect(), clip,
                                           D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                    nullptr);
                context->DrawBitmap(
                    artwork,
                    D2D1::RectF(artworkBox.left, artworkBox.top, artworkBox.right,
                                artworkBox.bottom),
                    opacity, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                context->PopLayer();
                clip->Release();
            }
            // A reflection is the BOTTOM edge of the cover mirrored below it and
            // dissolved into the card. The previous code squeezed the whole cover
            // into a 28 DIP strip, which preserved its caption and produced a second,
            // distorted album rather than a reflection.
            const RectF reflection = {
                layout.mediaCover.left, layout.mediaCover.bottom + 1.0f,
                layout.mediaCover.right, layout.mediaCover.bottom + 29.0f};
            const D2D1_SIZE_U artworkPixels = artwork->GetPixelSize();
            if (artworkPixels.height > 0 && artworkPixels.width > 0) {
                const D2D1_RECT_F reflectionSource = {
                    0.0f, static_cast<float>(artworkPixels.height) * 0.68f,
                    static_cast<float>(artworkPixels.width),
                    static_cast<float>(artworkPixels.height)};
                ID2D1RoundedRectangleGeometry* reflectionClip = nullptr;
                ID2D1Factory* reflectionFactory = nullptr;
                context->GetFactory(&reflectionFactory);
                if (reflectionFactory) {
                    reflectionFactory->CreateRoundedRectangleGeometry(
                        D2D1::RoundedRect(
                            D2D1::RectF(reflection.left, reflection.top, reflection.right,
                                        reflection.bottom),
                            16.0f, 16.0f),
                        &reflectionClip);
                    reflectionFactory->Release();
                }
                if (reflectionClip) {
                    context->PushLayer(
                        D2D1::LayerParameters1(D2D1::InfiniteRect(), reflectionClip,
                                               D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                        nullptr);
                    D2D1_MATRIX_3X2_F previousTransform = D2D1::Matrix3x2F::Identity();
                    context->GetTransform(&previousTransform);
                    context->SetTransform(
                        previousTransform *
                        D2D1::Matrix3x2F::Scale(
                            1.0f, -1.0f,
                            D2D1::Point2F(0.0f, reflection.top)));
                    const float height = reflection.bottom - reflection.top;
                    context->DrawBitmap(
                        artwork,
                        D2D1::RectF(reflection.left, reflection.top - height,
                                    reflection.right, reflection.top),
                        0.28f * opacity, D2D1_INTERPOLATION_MODE_LINEAR,
                        reflectionSource);
                    context->SetTransform(previousTransform);
                    context->PopLayer();
                    reflectionClip->Release();
                }

                const D2D1_GRADIENT_STOP reflectionMaskStops[] = {
                    {0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f * opacity)},
                    {0.48f, D2D1::ColorF(0.98f, 0.97f, 0.99f, 0.68f * opacity)},
                    {1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f * opacity)},
                };
                ID2D1GradientStopCollection* reflectionMaskStopsCollection = nullptr;
                ID2D1LinearGradientBrush* reflectionMask = nullptr;
                if (SUCCEEDED(context->CreateGradientStopCollection(
                        reflectionMaskStops, ARRAYSIZE(reflectionMaskStops), D2D1_GAMMA_2_2,
                        D2D1_EXTEND_MODE_CLAMP, &reflectionMaskStopsCollection)) &&
                    reflectionMaskStopsCollection &&
                    SUCCEEDED(context->CreateLinearGradientBrush(
                        D2D1::LinearGradientBrushProperties(
                            D2D1::Point2F(reflection.left, reflection.top),
                            D2D1::Point2F(reflection.left, reflection.bottom)),
                        reflectionMaskStopsCollection, &reflectionMask)) &&
                    reflectionMask) {
                    const D2D1_ROUNDED_RECT rounded = {
                        D2D1::RectF(reflection.left, reflection.top, reflection.right,
                                    reflection.bottom),
                        16.0f, 16.0f};
                    context->FillRoundedRectangle(&rounded, reflectionMask);
                }
                SafeRelease(&reflectionMask);
                SafeRelease(&reflectionMaskStopsCollection);
            }
            const D2D1_GRADIENT_STOP glossStops[] = {
                {0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.32f * opacity)},
                {0.42f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f * opacity)},
                {1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
            };
            ID2D1GradientStopCollection* glossGradientStops = nullptr;
            ID2D1LinearGradientBrush* gloss = nullptr;
            if (SUCCEEDED(context->CreateGradientStopCollection(
                    glossStops, ARRAYSIZE(glossStops), D2D1_GAMMA_2_2,
                    D2D1_EXTEND_MODE_CLAMP, &glossGradientStops)) &&
                glossGradientStops &&
                SUCCEEDED(context->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(
                        D2D1::Point2F(artworkBox.left, artworkBox.top),
                        D2D1::Point2F(artworkBox.left, artworkBox.bottom)),
                    glossGradientStops, &gloss)) &&
                gloss) {
                const D2D1_ROUNDED_RECT rounded = {
                    D2D1::RectF(artworkBox.left, artworkBox.top, artworkBox.right,
                                artworkBox.bottom),
                    14.0f, 14.0f};
                context->FillRoundedRectangle(&rounded, gloss);
            }
            SafeRelease(&gloss);
            SafeRelease(&glossGradientStops);
        } else {
            FillRounded(context, artworkBox, 14.0f,
                        D2D1::ColorF(0.91f, 0.87f, 0.98f, opacity));
            DrawTextIn(context, niriClockFormat_, L"\u266A", artworkBox, accent,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        wchar_t title[384] = {};
        wchar_t artist[256] = {};
        wchar_t album[256] = {};
        MultiByteToWideChar(CP_UTF8, 0, music.title, -1, title, 384);
        MultiByteToWideChar(CP_UTF8, 0, music.artist, -1, artist, 256);
        MultiByteToWideChar(CP_UTF8, 0, music.album, -1, album, 256);
        DrawTextIn(context, niriNotificationTitleFormat_, title, layout.mediaTitle, ink,
                   DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        wchar_t subtitle[512] = {};
        if (artist[0] && album[0]) {
            std::swprintf(subtitle, 512, L"%ls  ·  %ls", artist, album);
        } else {
            std::swprintf(subtitle, 512, L"%ls",
                          artist[0] ? artist
                                    : (album[0] ? album : L"\u672A\u77E5\u827A\u672F\u5BB6"));
        }
        const wchar_t* stateLabel =
            music.state == PlaybackState::Playing ? L"播放中" : L"已暂停";
        const RectF stateChip = {layout.mediaSubtitle.right - 52.0f,
                                 layout.mediaSubtitle.top + 2.0f,
                                 layout.mediaSubtitle.right,
                                 layout.mediaSubtitle.bottom - 1.0f};
        FillRounded(context, stateChip, 9.0f,
                    D2D1::ColorF(0.93f, 0.90f, 0.98f, 0.94f * opacity));
        DrawTextIn(context, niriWeatherQualityFormat_, stateLabel, stateChip, accent,
                   DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        RectF subtitleBox = layout.mediaSubtitle;
        subtitleBox.right = stateChip.left - 8.0f;
        DrawTextIn(context, niriMediaSubtitleFormat_, subtitle, subtitleBox, muted,
                   DWRITE_TEXT_ALIGNMENT_LEADING,
                   DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        D2D1_COLOR_F waveColour = text.musicAccent;
        waveColour.a = 0.95f * opacity;
        if (ID2D1SolidColorBrush* wave = BrushFor(context, waveColour)) {
            for (int i = 0; i < 5; ++i) {
                const float phase = text.musicWavePhase * 5.0f + static_cast<float>(i) * 0.8f;
                const float value =
                    0.25f + 0.75f * std::min(1.0f, text.musicLevelKnown ? text.musicLevel : 0.35f);
                const float half = 2.0f + 9.0f * (0.5f + 0.5f * std::sin(phase)) * value;
                const float x = layout.mediaWave.left + 3.0f + static_cast<float>(i) * 4.2f;
                const float cy = (layout.mediaWave.top + layout.mediaWave.bottom) * 0.5f;
                context->DrawLine(D2D1::Point2F(x, cy - half), D2D1::Point2F(x, cy + half),
                                  wave, 2.0f);
            }
        }

        const auto clockText = [](long long ms, wchar_t* out, size_t capacity) {
            const long long total = ms > 0 ? ms / 1000 : 0;
            std::swprintf(out, capacity, L"%lld:%02lld", total / 60, total % 60);
        };
        wchar_t elapsed[16] = {};
        wchar_t duration[16] = {};
        clockText(music.positionMs, elapsed, 16);
        clockText(music.durationMs, duration, 16);
        DrawTextIn(context, niriMediaTimeFormat_, elapsed, layout.mediaElapsed, muted,
                   DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        DrawTextIn(context, niriMediaTimeFormat_, duration, layout.mediaDuration, muted,
                   DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        FillRounded(context, layout.mediaProgress, 3.0f,
                    D2D1::ColorF(237.0f / 255.0f, 230.0f / 255.0f, 249.0f / 255.0f,
                                  opacity));
        const float fraction =
            music.durationMs > 0
                ? std::clamp(static_cast<float>(music.positionMs) /
                                 static_cast<float>(music.durationMs),
                             0.0f, 1.0f)
                : 0.0f;
        RectF progressFill = layout.mediaProgress;
        progressFill.right = progressFill.left +
                             (progressFill.right - progressFill.left) * fraction;
        FillRounded(context, progressFill, 3.0f, accentColour);
        const float knobX = progressFill.right;
        const float knobY =
            (layout.mediaProgress.top + layout.mediaProgress.bottom) * 0.5f;
        context->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(knobX, knobY), 5.0f, 5.0f),
            BrushFor(context, D2D1::ColorF(1.0f, 1.0f, 1.0f, opacity)));
        context->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, knobY), 5.0f, 5.0f),
                             accent, 2.0f);

        FillRounded(context, layout.mediaPlayPause, 22.0f, accentColour);
        const auto drawStep = [&](const RectF& box, bool forward) {
            const float cx = (box.left + box.right) * 0.5f;
            const float cy = (box.top + box.bottom) * 0.5f;
            const float direction = forward ? 1.0f : -1.0f;
            context->DrawLine(D2D1::Point2F(cx + direction * 6.0f, cy - 7.0f),
                              D2D1::Point2F(cx + direction * 6.0f, cy + 7.0f), muted,
                              1.8f);
            context->DrawLine(D2D1::Point2F(cx - direction * 5.0f, cy - 7.0f),
                              D2D1::Point2F(cx + direction * 2.0f, cy), muted, 1.8f);
            context->DrawLine(D2D1::Point2F(cx + direction * 2.0f, cy),
                              D2D1::Point2F(cx - direction * 5.0f, cy + 7.0f), muted,
                              1.8f);
        };
        drawStep(layout.mediaPrevious, false);
        drawStep(layout.mediaNext, true);
        ID2D1SolidColorBrush* transportInk =
            BrushFor(context, D2D1::ColorF(1.0f, 1.0f, 1.0f, opacity));
        const float playCx = (layout.mediaPlayPause.left + layout.mediaPlayPause.right) * 0.5f;
        const float playCy = (layout.mediaPlayPause.top + layout.mediaPlayPause.bottom) * 0.5f;
        if (music.state == PlaybackState::Playing) {
            context->DrawLine(D2D1::Point2F(playCx - 3.5f, playCy - 6.0f),
                              D2D1::Point2F(playCx - 3.5f, playCy + 6.0f), transportInk,
                              2.6f);
            context->DrawLine(D2D1::Point2F(playCx + 3.5f, playCy - 6.0f),
                              D2D1::Point2F(playCx + 3.5f, playCy + 6.0f), transportInk,
                              2.6f);
        } else {
            context->DrawLine(D2D1::Point2F(playCx - 4.0f, playCy - 7.0f),
                              D2D1::Point2F(playCx + 7.0f, playCy), transportInk, 2.4f);
            context->DrawLine(D2D1::Point2F(playCx + 7.0f, playCy),
                              D2D1::Point2F(playCx - 4.0f, playCy + 7.0f), transportInk,
                              2.4f);
        }
    } else {
        DrawTextIn(context, niriMetricFormat_, L"当前无播放内容", layout.media,
                   muted ? muted : ink, DWRITE_TEXT_ALIGNMENT_CENTER,
                   DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    DrawTextIn(context, niriNotificationHeaderFormat_, L"通知",
               layout.notificationsTitle,
               ink, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    FillRounded(context, layout.notificationCountChip, 12.0f,
                D2D1::ColorF(237.0f / 255.0f, 230.0f / 255.0f, 249.0f / 255.0f,
                              0.96f * opacity));
    wchar_t notificationCount[16] = {};
    std::swprintf(notificationCount, 16, L"%d",
                  text.historyActive ? text.notificationTotal : text.unreadCount);
    DrawTextIn(context, niriNotificationMetaFormat_, notificationCount,
               layout.notificationCountChip,
               accent, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    const float preferenceCx = layout.preferencesButton.left + 10.0f;
    const float preferenceCy =
        (layout.preferencesButton.top + layout.preferencesButton.bottom) * 0.5f;
    for (int line = 0; line < 3; ++line) {
        const float y = preferenceCy - 5.0f + static_cast<float>(line) * 5.0f;
        context->DrawLine(D2D1::Point2F(preferenceCx - 6.0f, y),
                          D2D1::Point2F(preferenceCx + 6.0f, y), muted, 1.2f);
        const float knob = preferenceCx - 2.5f + static_cast<float>(line) * 2.5f;
        context->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knob, y), 1.8f, 1.8f), muted);
    }
    DrawTextIn(context, niriNotificationMetaFormat_, L"偏好设置",
               {layout.preferencesButton.left + 20.0f, layout.preferencesButton.top,
                layout.preferencesButton.right, layout.preferencesButton.bottom},
               muted, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    const auto widen = [](const char* utf8, wchar_t* out, int capacity) {
        if (!out || capacity <= 0) return;
        out[0] = L'\0';
        if (!utf8 || utf8[0] == '\0') return;
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, capacity);
    };
    if (text.notifications && text.visibleCount > 0) {
        for (int i = 0; i < text.visibleCount && i < layout.notificationRowsShown; ++i) {
            const int storeIndex = text.visibleIndices[i];
            const Notification* item = text.notifications->At(storeIndex);
            if (!item) continue;
            const RectF& row = layout.notificationRows[i];
            softShadow(row, 16.0f);
            FillRounded(context, row, 16.0f, white);
            strokeRounded(row, 16.0f, cardBorder, 1.0f);
            wchar_t app[64] = {};
            wchar_t title[kTitleCapacity] = {};
            wchar_t body[kBodyCapacity] = {};
            widen(item->app, app, 64);
            widen(item->title, title, kTitleCapacity);
            widen(item->body, body, kBodyCapacity);
            const RectF& avatar = layout.notificationAvatar[i];
            FillRounded(context, avatar, (avatar.right - avatar.left) * 0.5f,
                        D2D1::ColorF(0.94f, 0.91f, 0.99f, opacity));
            wchar_t initial[2] = {app[0] ? app[0] : L'N', L'\0'};
            DrawTextIn(context, niriNotificationTitleFormat_, initial, avatar, accent,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            DrawTextIn(context, niriNotificationAppFormat_, app,
                       layout.notificationApp[i],
                       muted ? muted : ink, DWRITE_TEXT_ALIGNMENT_LEADING,
                       DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            DrawTextIn(context, niriNotificationTitleFormat_, title,
                       layout.notificationTitle[i],
                       ink, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            DrawTextIn(context, niriNotificationBodyFormat_, body,
                       layout.notificationBody[i],
                       muted ? muted : ink, DWRITE_TEXT_ALIGNMENT_LEADING,
                       DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            const unsigned long long now = GetTickCount64();
            const unsigned long long ageMs =
                now >= item->timestampMs ? now - item->timestampMs : 0;
            wchar_t age[32] = {};
            if (ageMs < 60000ull) {
                wcscpy_s(age, L"刚刚");
            } else if (ageMs < 3600000ull) {
                std::swprintf(age, 32, L"%llu分钟前", ageMs / 60000ull);
            } else if (ageMs < 86400000ull) {
                std::swprintf(age, 32, L"%llu小时前", ageMs / 3600000ull);
            } else {
                std::swprintf(age, 32, L"%llu天前", ageMs / 86400000ull);
            }
            DrawTextIn(context, niriNotificationMetaFormat_, age,
                       layout.notificationAge[i], muted,
                       DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            DrawTextIn(context, niriNotificationTitleFormat_, L"打开",
                       layout.notificationOpen[i],
                       accent, DWRITE_TEXT_ALIGNMENT_LEADING,
                       DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            DrawTextIn(context, niriNotificationMetaFormat_, L"关闭",
                       layout.notificationDismiss[i],
                       muted, DWRITE_TEXT_ALIGNMENT_LEADING,
                       DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    } else {
        DrawTextIn(context, niriMetricFormat_, L"暂无通知",
                   {layout.notifications.left + 14.0f, layout.notifications.top + 58.0f,
                    layout.notifications.right - 14.0f, layout.notifications.bottom - 58.0f},
                   muted ? muted : ink, DWRITE_TEXT_ALIGNMENT_CENTER,
                   DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (layout.notificationScrollVisible) {
        FillRounded(context, layout.notificationScrollTrack, 1.0f,
                    D2D1::ColorF(0.84f, 0.81f, 0.90f, 0.42f * opacity));
        FillRounded(context, layout.notificationScrollThumb, 1.0f,
                    D2D1::ColorF(0.42f, 0.32f, 0.68f, 0.66f * opacity));
    }
    FillRounded(context, layout.historyButton, 16.0f,
                D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.82f * opacity));
    DrawTextIn(context, niriNotificationMetaFormat_,
               text.historyActive ? L"未读" : L"全部",
               layout.historyButton, muted, DWRITE_TEXT_ALIGNMENT_CENTER,
               DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    FillRounded(context, layout.clearButton, 17.0f,
                D2D1::ColorF(0.82f, 0.77f, 0.93f, 0.98f * opacity));
    DrawTextIn(context, niriNotificationTitleFormat_, L"清空", layout.clearButton, ink,
               DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
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
                                 const CallStripText& text, float opacity, bool showTimer) {
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
    //
    // NOT WHILE A GAME HAS THE SCREEN: the strip is then a small call indicator over a full-screen
    // picture, and a clock counting at it is the one thing on it that is not about the call ("玩游戏
    // 全屏 通话 不用显示左边的时间"). The icon and the level meter stay.
    wchar_t timer[32] = {};
    CallTimerText(text.seconds, timer, 32);
    if (showTimer && timer[0] != L'\0' && bodyFormat_) {
        ID2D1SolidColorBrush* green = nullptr;
        // The green iOS uses for a call in progress (0x30D158), as fractions of 255.
        const D2D1_COLOR_F callGreen = D2D1::ColorF(0.188f, 0.820f, 0.345f, 0.98f * opacity);
        if (SUCCEEDED(context->CreateSolidColorBrush(callGreen, &green)) && green) {
            const DWRITE_TEXT_ALIGNMENT previousHorizontal = bodyFormat_->GetTextAlignment();
            const DWRITE_PARAGRAPH_ALIGNMENT previousVertical = bodyFormat_->GetParagraphAlignment();
            bodyFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            bodyFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            const float width = 80.0f;
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
    const float barWidth = 4.0f;
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
        // THE CALL'S OWN GREEN, at a floor of 0.55, because the warm meter that used to be here was
        // unreadable on the light glass the strip is drawn on ("通话时 音波图看不清"). Green matches the timer
        // beside it, so the meter reads as part of the call rather than as a second, unrelated reading.
        const D2D1_COLOR_F colour = D2D1::ColorF(0.188f, 0.820f, 0.345f,
                                                 (0.55f + 0.45f * fraction) * opacity);
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

float TextRenderer::MusicMeterFraction(float peak) {
    // The window the twelve steps are laid out over. -42 dB is quiet-but-audible; 0 dB is full scale.
    //
    // Chosen from a measurement rather than a taste (2026-11-05): the peak this card is handed sat at
    // 0.017..0.115 -- -35 dB to -19 dB -- so the window has to reach down to -42 to give that band
    // anything to move through, and it must reach 0 so a loud passage still fills the meter.
    constexpr float kFloorDb = 42.0f;

    // `!(peak > 0)` rather than `peak <= 0`: it is also how a NaN is refused, and a NaN reaching the
    // quantiser would light a step from undefined arithmetic.
    if (!(peak > 0.0f)) return 0.0f;
    if (peak >= 1.0f) return 1.0f;

    const float level = (20.0f * std::log10(peak) + kFloorDb) / kFloorDb;
    if (level < 0.0f) return 0.0f;
    return level > 1.0f ? 1.0f : level;
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

    // Text colour. Everything below uses these three brushes, so the whole card's contrast moves together.
    // The ALBUM is what is behind the words, not the island: black or white comes from the blurred
    // background's average. PillInk is only the fallback for a track that published no cover.
    const bool albumKnown = text.backgroundLuminance > 0.0f;
    const bool darkAlbum = albumKnown && text.backgroundLuminance < 0.52f;
    const D2D1_COLOR_F primary =
        !albumKnown ? CardInk(opacity)
                    : (darkAlbum ? D2D1::ColorF(0.96f, 0.97f, 1.00f, opacity)
                                 : D2D1::ColorF(0.04f, 0.05f, 0.08f, opacity));
    const D2D1_COLOR_F secondary =
        !albumKnown ? CardInk(0.80f * opacity)
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

    // THE COVER'S OWN COLOUR, behind the top half. The wash above is the album's PICTURE, blurred; this
    // is its COLOUR, pooled where the cover sits and fading out across the card, so a track with a
    // saturated cover reads as that colour rather than as a grey photograph.
    {
        const D2D1_GRADIENT_STOP stops[2] = {
            {0.0f, D2D1::ColorF(accent.r, accent.g, accent.b, 0.16f * opacity)},
            {1.0f, D2D1::ColorF(accent.r, accent.g, accent.b, 0.0f)},
        };
        ID2D1GradientStopCollection* collection = nullptr;
        if (SUCCEEDED(context->CreateGradientStopCollection(stops, 2, &collection)) && collection) {
            ID2D1RadialGradientBrush* glow = nullptr;
            const float centreX = (layout.artwork.left + layout.artwork.right) * 0.5f;
            const float centreY = (layout.artwork.top + layout.artwork.bottom) * 0.5f;
            context->CreateRadialGradientBrush(
                D2D1::RadialGradientBrushProperties(
                    D2D1::Point2F(centreX, centreY), D2D1::Point2F(0.0f, 0.0f),
                    (box.rect.right - box.rect.left) * 0.55f,
                    (box.rect.bottom - box.rect.top) * 0.80f),
                collection, &glow);
            collection->Release();
            if (glow) {
                pushSilhouette();
                context->FillRectangle(
                    D2D1::RectF(box.rect.left, box.rect.top, box.rect.right, box.rect.bottom), glow);
                popSilhouette();
                glow->Release();
            }
        }
    }

    // The glass edge, over the album wash, the thin scrim and the glow.
    if (island::CurrentTheme().lens) {
        const D2D1_ROUNDED_RECT rounded = {
            D2D1::RectF(box.rect.left, box.rect.top, box.rect.right, box.rect.bottom),
            box.radiusX, box.radiusY};
        island::DrawLensRimBand(context, rounded, island::CurrentTheme(), opacity);
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
    // The cover's own shadow first, so the picture sits ON the card rather than in a cut-out.
    {
        const D2D1_ROUNDED_RECT artShadow = {
            D2D1::RectF(layout.artwork.left, layout.artwork.top, layout.artwork.right,
                        layout.artwork.bottom),
            10.0f, 10.0f};
        island::DrawSoftShadow(context, artShadow, 0.35f * opacity, 10.0f);
    }
    if (artwork && artworkWidth > 0 && artworkHeight > 0) {
        ID2D1RoundedRectangleGeometry* artClip = nullptr;
        {
            const D2D1_ROUNDED_RECT rounded = {
                D2D1::RectF(layout.artwork.left, layout.artwork.top, layout.artwork.right,
                            layout.artwork.bottom),
                10.0f, 10.0f};
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

        // --- 3a. the cover's REFLECTION -----------------------------------
        //
        // The bottom ~17 DIP of the cover, mirrored below it and faded to nothing: the one thing that makes
        // a picture sit ON the glass rather than in it. Drawn from the same bitmap, so it can never
        // disagree with the cover above it.
        {
            const float reflectTop = layout.artwork.bottom + 3.0f;
            constexpr float kReflectHeight = 20.0f;
            if (reflectTop + kReflectHeight < layout.progress.top) {
                ID2D1Factory* factory = nullptr;
                context->GetFactory(&factory);
                ID2D1RoundedRectangleGeometry* reflectClip = nullptr;
                if (factory) {
                    const D2D1_ROUNDED_RECT shape = {
                        D2D1::RectF(layout.artwork.left, reflectTop, layout.artwork.right,
                                    reflectTop + kReflectHeight),
                        8.0f, 8.0f};
                    factory->CreateRoundedRectangleGeometry(shape, &reflectClip);
                    factory->Release();
                }
                // The mask: three tenths at the top of the reflection, nothing at its bottom.
                ID2D1GradientStopCollection* stops = nullptr;
                const D2D1_GRADIENT_STOP fade[2] = {
                    {0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.30f * opacity)},
                    {1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
                };
                ID2D1LinearGradientBrush* mask = nullptr;
                if (SUCCEEDED(context->CreateGradientStopCollection(fade, 2, &stops)) && stops) {
                    context->CreateLinearGradientBrush(
                        D2D1::LinearGradientBrushProperties(
                            D2D1::Point2F(0.0f, reflectTop),
                            D2D1::Point2F(0.0f, reflectTop + kReflectHeight)),
                        stops, &mask);
                    stops->Release();
                }
                if (mask && reflectClip) {
                    const D2D1_LAYER_PARAMETERS1 params = D2D1::LayerParameters1(
                        D2D1::InfiniteRect(), reflectClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                        D2D1::IdentityMatrix(), 1.0f, mask, D2D1_LAYER_OPTIONS1_NONE);
                    context->PushLayer(params, nullptr);
                    D2D1_MATRIX_3X2_F previous = D2D1::IdentityMatrix();
                    context->GetTransform(&previous);
                    // Mirror about the line y = reflectTop: y' = 2 * reflectTop - y.
                    context->SetTransform(D2D1::Matrix3x2F::Scale(1.0f, -1.0f) *
                                          D2D1::Matrix3x2F::Translation(0.0f, 2.0f * reflectTop));
                    DrawBitmapRect(context, artwork, layout.artwork, opacity);
                    context->SetTransform(previous);
                    context->PopLayer();
                }
                if (mask) mask->Release();
                if (reflectClip) reflectClip->Release();
            }
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
            10.0f, 10.0f};
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

    // --- 5b. the lyric region, beside the cover --------------------------
    //
    // The user's "这个区域显示歌词": the current line, with the accent sweeping across it as the line is
    // sung, and the next line dimmed under it. Both come from the same sheet as the pill's line, so the
    // card and the bar cannot show different words.
    //
    // AND THE CHANGE IS A CROSS-FADE, not a substitution: the line that just left fades out under the one
    // arriving, which fades in over it, both for the length of the pill's own roll. Without it the words
    // changed between two frames, which is the "没有过渡" this bar has been reported for before.
    const float lyricRoll =
        text.lyricRoll < 0.0f ? 0.0f : (text.lyricRoll > 1.0f ? 1.0f : text.lyricRoll);
    if (text.lyricPrev && text.lyricPrev[0] != '\0' && lyricRoll < 0.999f && musicLyricFormat_ &&
        factory_ && secondaryBrush) {
        wchar_t widePrev[512] = {};
        if (MultiByteToWideChar(CP_UTF8, 0, text.lyricPrev, -1, widePrev, 512) > 0) {
            IDWriteTextLayout* layoutPrev = LayoutFor(
                widePrev, musicLyricFormat_, layout.lyricCurrent.right - layout.lyricCurrent.left,
                layout.lyricCurrent.bottom - layout.lyricCurrent.top);
            if (layoutPrev) {
                secondaryBrush->SetOpacity(1.0f - lyricRoll);
                context->DrawTextLayout(
                    D2D1::Point2F(layout.lyricCurrent.left, layout.lyricCurrent.top), layoutPrev,
                    secondaryBrush);
                secondaryBrush->SetOpacity(1.0f);
            }
        }
    }
    if (text.lyricLine && text.lyricLine[0] != '\0' && musicLyricFormat_ && factory_) {
        wchar_t wideLine[512] = {};
        if (MultiByteToWideChar(CP_UTF8, 0, text.lyricLine, -1, wideLine, 512) > 0) {
            const float width = layout.lyricCurrent.right - layout.lyricCurrent.left;
            IDWriteTextLayout* layoutLine =
                LayoutFor(wideLine, musicLyricFormat_, width,
                          layout.lyricCurrent.bottom - layout.lyricCurrent.top);
            if (layoutLine) {
                DWRITE_TEXT_METRICS metrics = {};
                layoutLine->GetMetrics(&metrics);
                const D2D1_POINT_2F origin =
                    D2D1::Point2F(layout.lyricCurrent.left, layout.lyricCurrent.top);
                // THE UNSUNG WORDS ARE THE DIM ONES and the sung ones the bright ink. The first version
                // painted the sung part in the ACCENT, which on a blue cover is almost the background --
                // "歌词唱完的部分 看不清". The accent is the underline instead, where it can be seen.
                if (secondaryBrush) {
                    secondaryBrush->SetOpacity(lyricRoll);
                    context->DrawTextLayout(origin, layoutLine, secondaryBrush);
                    secondaryBrush->SetOpacity(1.0f);
                }
                const float fraction = text.lyricProgress > 1.0f ? 1.0f : text.lyricProgress;
                const float filled = metrics.widthIncludingTrailingWhitespace * fraction;
                // The line is left-aligned, so the karaoke starts at the rect's left edge.
                const float textLeft = layout.lyricCurrent.left;
                if (primaryBrush && fraction > 0.0f) {
                    context->PushAxisAlignedClip(
                        D2D1::RectF(textLeft, layout.lyricCurrent.top, textLeft + filled,
                                    layout.lyricCurrent.bottom),
                        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                    primaryBrush->SetOpacity(lyricRoll);
                    context->DrawTextLayout(origin, layoutLine, primaryBrush);
                    primaryBrush->SetOpacity(1.0f);
                    context->PopAxisAlignedClip();
                }
                if (accentBrush && fraction > 0.0f) {
                    // The sung part's underline: the accent, where the cover's colour reads.
                    const D2D1_ROUNDED_RECT sung = {
                        D2D1::RectF(textLeft, layout.lyricCurrent.bottom - 2.0f, textLeft + filled,
                                    layout.lyricCurrent.bottom),
                        1.0f, 1.0f};
                    accentBrush->SetOpacity(lyricRoll);
                    context->FillRoundedRectangle(&sung, accentBrush);
                    accentBrush->SetOpacity(1.0f);
                }
                // No Release: the layout cache owns it (see TextRenderer::LayoutFor).
            }
        }
    } else if (secondaryBrush && musicLyricFormat_) {
        DrawTextIn(context, musicLyricFormat_, L"暂无歌词", layout.lyricCurrent, secondaryBrush,
                   DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (text.lyricNext && text.lyricNext[0] != '\0' && musicLyricNextFormat_) {
        wchar_t wideNext[512] = {};
        if (MultiByteToWideChar(CP_UTF8, 0, text.lyricNext, -1, wideNext, 512) > 0) {
            DrawTextIn(context, musicLyricNextFormat_, wideNext, layout.lyricNext, secondaryBrush,
                       DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }

    // --- 5c. the thin level bar, at the card's right edge ----------------
    //
    // A SEGMENTED meter, not a continuous fill: twelve rounded steps up the cover's height. The lit ones
    // take the cover's accent -- the top lit step is drawn at full strength and the ones below it at 0.55,
    // so the bar reads as a level rather than as one bar of paint -- and the unlit ones a faint ink. The
    // reading is the same peak the pill's ribbon moves with, so the card and the bar agree about the
    // SOUND; the STEPS are laid out in decibels (see MusicMeterFraction), because the reading itself is a
    // linear amplitude that lives inside one linear step and the meter then never appears to move.
    if (text.levelKnown && layout.levelBar.right > layout.levelBar.left) {
        constexpr int kSegments = 12;
        constexpr float kGap = 2.0f;
        const float height = layout.levelBar.bottom - layout.levelBar.top;
        const float segment = (height - kGap * (kSegments - 1)) / kSegments;
        const int lit = static_cast<int>(MusicMeterFraction(text.level) * kSegments + 0.5f);
        ID2D1SolidColorBrush* dark = nullptr;
        ID2D1SolidColorBrush* dim = nullptr;
        ID2D1SolidColorBrush* bright = nullptr;
        context->CreateSolidColorBrush(
            D2D1::ColorF(primary.r, primary.g, primary.b, 0.08f * opacity), &dark);
        context->CreateSolidColorBrush(
            D2D1::ColorF(accent.r * 0.7f, accent.g * 0.7f, accent.b * 0.7f, 0.80f * opacity), &dim);
        context->CreateSolidColorBrush(accent, &bright);
        for (int i = 0; i < kSegments; ++i) {
            const float bottom = layout.levelBar.bottom - static_cast<float>(i) * (segment + kGap);
            const D2D1_ROUNDED_RECT step = {
                D2D1::RectF(layout.levelBar.left, bottom - segment, layout.levelBar.right, bottom),
                2.0f, 2.0f};
            ID2D1SolidColorBrush* brush = (i < lit) ? (i == lit - 1 ? bright : dim) : dark;
            if (brush) context->FillRoundedRectangle(&step, brush);
        }
        if (dark) dark->Release();
        if (dim) dim->Release();
        if (bright) bright->Release();
    }

    // --- 5d. the vertical lyric progress, beside the level meter --------
    //
    // Where the song is in the SHEET: the current line's index plus how far through that line it is,
    // over the number of lines. A thin bar beside the meter -- the two read as one instrument panel at
    // the card's right edge -- filled from the top, because this is a reading position, not a level.
    if (layout.lyricProgress.right > layout.lyricProgress.left) {
        const D2D1_ROUNDED_RECT track = {
            D2D1::RectF(layout.lyricProgress.left, layout.lyricProgress.top,
                        layout.lyricProgress.right, layout.lyricProgress.bottom),
            1.5f, 1.5f};
        ID2D1SolidColorBrush* progressTrack = nullptr;
        if (SUCCEEDED(context->CreateSolidColorBrush(
                D2D1::ColorF(primary.r, primary.g, primary.b, 0.08f * opacity), &progressTrack)) &&
            progressTrack) {
            context->FillRoundedRectangle(&track, progressTrack);
            progressTrack->Release();
        }
        if (text.lyricCount > 0 && text.lyricIndex >= 0 && accentBrush) {
            float fraction = (static_cast<float>(text.lyricIndex) + text.lyricProgress) /
                             static_cast<float>(text.lyricCount);
            if (fraction < 0.0f) fraction = 0.0f;
            if (fraction > 1.0f) fraction = 1.0f;
            if (fraction > 0.0f) {
                const float height =
                    (layout.lyricProgress.bottom - layout.lyricProgress.top) * fraction;
                const D2D1_ROUNDED_RECT filled = {
                    D2D1::RectF(layout.lyricProgress.left, layout.lyricProgress.top,
                                layout.lyricProgress.right, layout.lyricProgress.top + height),
                    1.5f, 1.5f};
                context->FillRoundedRectangle(&filled, accentBrush);
            }
        }
    }

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
        const D2D1_COLOR_F trackColour = D2D1::ColorF(primary.r, primary.g, primary.b,
                                                     0.16f * opacity);
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

                // The knob at the head of the fill: the flyout's own handle, which also says where the
                // track is at a glance instead of only by the bar's length. The ring is the card's ink so
                // it stays visible against the accent and the album behind it.
                const float midY = (layout.progress.top + layout.progress.bottom) * 0.5f;
                ID2D1SolidColorBrush* knobRing = nullptr;
                if (SUCCEEDED(context->CreateSolidColorBrush(
                        D2D1::ColorF(primary.r, primary.g, primary.b, 0.90f * opacity),
                        &knobRing)) &&
                    knobRing) {
                    context->FillEllipse(
                        D2D1::Ellipse(D2D1::Point2F(filled, midY), 4.4f, 4.4f), knobRing);
                    knobRing->Release();
                }
                context->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(filled, midY), 3.1f, 3.1f), accentBrush);
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
                D2D1::ColorF(accent.r, accent.g, accent.b, 0.34f * opacity);
            if (SUCCEEDED(context->CreateSolidColorBrush(discColour, &discBrush)) &&
                discBrush) {
                const D2D1_ELLIPSE disc =
                    D2D1::Ellipse(D2D1::Point2F(centreX, centreY), radius, radius);
                context->FillEllipse(&disc, discBrush);
                discBrush->Release();
            }

            if (primaryBrush) {
                if (playing) {
                    const float barWidth = 5.0f;
                    const float barHeight = 18.0f;
                    const float gap = 6.0f;
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
                        const float half = 11.0f;
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

    IDWriteTextLayout* layout = LayoutFor(text, format, 8192.0f, 512.0f);
    if (!layout) return 0.0f;
    DWRITE_TEXT_METRICS metrics = {};
    const HRESULT hr = layout->GetMetrics(&metrics);
    return SUCCEEDED(hr) ? metrics.width : 0.0f;
}

float TextRenderer::TrackLineWidth(const wchar_t* text) const {
    if (!text || !factory_) return 0.0f;
    IDWriteTextFormat* format = pillLineFormat_ ? pillLineFormat_ : appFormat_;
    if (!format) return 0.0f;

    // The same 8192-wide, 512-tall area TextWidth uses: the width of a line is a property of the TEXT and the
    // face, not of the box it will be drawn into -- putting it in a box is what wrapping is for, and wrapping
    // is exactly what this line must never do.
    IDWriteTextLayout* layout = LayoutFor(text, format, 8192.0f, 512.0f);
    if (!layout) return 0.0f;
    DWRITE_TEXT_METRICS metrics = {};
    const HRESULT hr = layout->GetMetrics(&metrics);
    return SUCCEEDED(hr) ? metrics.width : 0.0f;
}

void TextRenderer::DrawLyricLine(ID2D1DeviceContext2* context, const RoundedBox& box,
                                 const char* utf8Line, float scrollDip, float opacity,
                                 const LineRoll& fade, const D2D1_COLOR_F* over) {
    if (!context || !utf8Line || utf8Line[0] == '\0') return;
    DrawTrackLine(context, box, utf8Line, scrollDip, opacity, fade, over);
}

void TextRenderer::DrawTrackLine(ID2D1DeviceContext2* context, const RoundedBox& box,
                                 const char* utf8Line, float scrollDip, float opacity,
                                 const LineRoll& roll, const D2D1_COLOR_F* over) {
    // The pill's own heavier format; the fallback keeps this working if that one could not be created.
    IDWriteTextFormat* format = pillLineFormat_ ? pillLineFormat_ : appFormat_;
    if (!context || !format || !utf8Line || utf8Line[0] == '\0') return;

    wchar_t wide[512] = {};
    if (MultiByteToWideChar(CP_UTF8, 0, utf8Line, -1, wide, 512) <= 0) return;

    // THE LINE'S INK IS DECIDED BY WHAT IS BEHIND IT. Over the music wave the card's ink is wrong twice
    // over: the wave is the COVER's accent -- almost black on one track, a bright pastel on the next --
    // and the lyric sat on it unreadably. When `over` is given, the ink is that colour's contrast: the
    // theme's dark ink on a bright wave, its light ink on a dark one.
    D2D1_COLOR_F ink = TextColour(opacity);
    if (over) {
        const float luma = 0.2126f * over->r + 0.7152f * over->g + 0.0722f * over->b;
        const D2D1_COLOR_F base = (luma > 0.45f) ? CurrentTheme().ink : CurrentTheme().highlight;
        ink = D2D1::ColorF(base.r, base.g, base.b, opacity);
    }
    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(context->CreateSolidColorBrush(ink, &brush)) || !brush) {
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
    const island::Theme& theme = island::CurrentTheme();
    const bool niriLight = std::wcscmp(theme.name, L"Niri") == 0;

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
    const D2D1_COLOR_F hoverColour =
        niriLight ? PillInk(0.14f * opacity)
                  : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f * opacity);
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
            if (niriLight) {
                iconBox.left -= 2.0f;
                iconBox.top -= 2.0f;
                iconBox.right += 2.0f;
                iconBox.bottom += 2.0f;
            }
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
            const float iconStrength =
                niriLight ? 1.0f : (button.minimised ? 0.55f : 1.0f);
            DrawBitmapRect(context, button.icon, iconBox, opacity * appear * iconStrength,
                           niriLight ? D2D1_INTERPOLATION_MODE_LINEAR
                                     : D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
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

        // ONE LAYOUT PER DISTINCT TITLE, not one per icon per frame: the row is drawn sixty times a
        // second and its titles change only when the windows do. The trimming is re-applied to the
        // cached layout because the sign belongs to the call, not to the text.
        IDWriteTextLayout* layout =
            LayoutFor(button.title, format, titleBox.right - titleBox.left, lineHeight);
        if (!layout) {
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
        // No Release: the layout cache owns it (see TextRenderer::LayoutFor).
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
                        IDWriteTextFormat* glyphFormat, IDWriteTextFormat* textFormat,
                        const D2D1_COLOR_F* inkOverride = nullptr) {
    if (!context || !glyphFormat) return;

    ID2D1SolidColorBrush* brush = nullptr;
    // White on an ON tile: the tile's fill is the accent, and the theme's ink is the wrong colour on it
    // (it is dark in the light themes). The override carries the caller's own colour instead.
    const D2D1_COLOR_F ink = inkOverride ? *inkOverride : island::CardInk(0.95f * opacity);
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
    const D2D1_COLOR_F ink = island::CardInk(0.90f * opacity);
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
        // A card is a raised sheet, not another control inside the bar. The
        // painted themes historically gave it no shadow at all, which made the
        // large 520 x 500 surface look pasted onto the wallpaper. The reference
        // uses a wide, soft drop shadow, so cards get a floor even when the pill
        // theme deliberately keeps its own shadow quiet.
        island::DrawSoftShadow(context, rounded, std::max(theme.shadow, 0.34f), 26.0f);
    }
    const D2D1_COLOR_F fillColour = D2D1::ColorF(theme.cardFill.r, theme.cardFill.g, theme.cardFill.b,
                                                theme.cardFill.a * opacity);
    const D2D1_COLOR_F edgeColour = island::ThemeHighlight(theme.cardEdge * opacity);
    // Created here rather than borrowed from the text renderer's brush cache: this function is free
    // rather than a member (the music card and the notification call it before the class is in scope),
    // and it runs once or twice a frame rather than once per icon or per tile.
    if (theme.lens) {
        ID2D1SolidColorBrush* fill = nullptr;
        if (SUCCEEDED(context->CreateSolidColorBrush(fillColour, &fill)) && fill) {
            context->FillRoundedRectangle(&rounded, fill);
            fill->Release();
        }
    } else {
        // The reference's dark acrylic is a shallow vertical gradient: a lighter,
        // blue-grey crown fading into a near-black base. DrawSmokedBase caches the
        // brush, so this is a transform and one fill rather than a new allocation.
        const D2D1_COLOR_F top = D2D1::ColorF(
            std::min(1.0f, theme.cardFill.r + 0.030f),
            std::min(1.0f, theme.cardFill.g + 0.034f),
            std::min(1.0f, theme.cardFill.b + 0.042f), theme.cardFill.a);
        const D2D1_COLOR_F bottom = D2D1::ColorF(
            std::max(0.0f, theme.cardFill.r - 0.012f),
            std::max(0.0f, theme.cardFill.g - 0.012f),
            std::max(0.0f, theme.cardFill.b - 0.014f),
            std::max(0.0f, theme.cardFill.a - 0.015f));
        island::DrawSmokedBase(context, rounded, theme, top, bottom, opacity);
    }

    // THE CRYSTAL CARD (2026-10-18, from the reference the user supplied): the 2% body is already under
    // this. A crisp 1px 40% outline, a facet highlight inside the top edge, a thickness shadow inside the
    // bottom and one diagonal sheen -- the reference's material, drawn at card size. The island's 2.5 DIP
    // rim band and caustic are NOT used here any more: they are the pill's material, and at 320x320 they
    // read as a coloured strip rather than as glass.
    if (theme.lens) {
        island::DrawCardAmbient(context, rounded, opacity, theme);
        island::DrawCardCrystal(context, rounded, opacity);
        return;
    }

    ID2D1SolidColorBrush* edge = nullptr;
    if (SUCCEEDED(context->CreateSolidColorBrush(edgeColour, &edge)) && edge) {
        context->DrawRoundedRectangle(&rounded, edge, 1.0f);
        edge->Release();
    }
    // One short specular line just inside the top edge. It is what separates the
    // reference's glass from a plain translucent rectangle; it stops before either
    // rounded corner so it cannot read as a second frame.
    ID2D1SolidColorBrush* crown = nullptr;
    if (SUCCEEDED(context->CreateSolidColorBrush(island::ThemeHighlight(0.16f * opacity), &crown)) &&
        crown) {
        const float inset = std::min(radius, (rounded.rect.right - rounded.rect.left) * 0.22f);
        context->DrawLine(D2D1::Point2F(rounded.rect.left + inset, rounded.rect.top + 1.2f),
                          D2D1::Point2F(rounded.rect.right - inset, rounded.rect.top + 1.2f),
                          crown, 1.0f);
        crown->Release();
    }
    // THE SAME LIGHTING THE PILL GETS, from the same function: a card's surface and the bar's are the same
    // material at different sizes, and this is where that stops being a claim in a comment.
    island::DrawSurfaceLighting(context, rounded, theme, opacity);
    if (!moving) {
        island::DrawGrain(context, rounded, std::max(theme.grain, 0.012f) * opacity);
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
    if (!context || !panelLabelFormat_ || !panelSubFormat_ || !panelIconFormat_) return;

    // One slider, or two when the display answers for its brightness (see SystemPanelText::hasBrightness).
    const SystemPanelLayout layout = SystemPanelFor(box, text.tileCount,
                                                    text.hasBrightness ? 2 : 1,
                                                    text.pageCount > 1, text.pageIndex,
                                                    text.pageCount);

    // THE INSET SURFACE, from the reference's sliders block: a darker well inside the card, with a
    // hairline of the card's ink. Drawn before the rows, and only when there is a row to hold.
    if (layout.sliderPanel.right > layout.sliderPanel.left &&
        layout.sliderPanel.bottom > layout.sliderPanel.top) {
        constexpr float kPanelRadius = 20.0f;
        const D2D1_ROUNDED_RECT rounded = {
            D2D1::RectF(layout.sliderPanel.left, layout.sliderPanel.top, layout.sliderPanel.right,
                        layout.sliderPanel.bottom),
            kPanelRadius, kPanelRadius};
        if (ID2D1SolidColorBrush* well = BrushFor(context, island::CardInk(0.045f * opacity))) {
            context->FillRoundedRectangle(&rounded, well);
        }
        if (ID2D1SolidColorBrush* edge = BrushFor(context, island::CardInk(0.075f * opacity))) {
            context->DrawRoundedRectangle(&rounded, edge, 1.0f);
        }
        if (ID2D1SolidColorBrush* highlight =
                BrushFor(context, island::ThemeHighlight(0.065f * opacity))) {
            context->DrawLine(
                D2D1::Point2F(layout.sliderPanel.left + kPanelRadius, layout.sliderPanel.top + 1.0f),
                D2D1::Point2F(layout.sliderPanel.right - kPanelRadius, layout.sliderPanel.top + 1.0f),
                highlight, 1.0f);
        }
    }

    // THE TILE'S BLUE, from the reference: #0078d4 at the top left to #0262b0 at the bottom right. Its
    // own brush rather than a solid one because the diagonal is most of what makes the tile read as lit.
    const D2D1_COLOR_F onFillTop = D2D1::ColorF(0.086f, 0.537f, 0.910f, 0.96f * opacity);
    const D2D1_COLOR_F onFillBottom = D2D1::ColorF(0.0078f, 0.3843f, 0.6902f, 0.96f * opacity);
    const auto fillOnTile = [&](const D2D1_ROUNDED_RECT& rounded, float amount) {
        if (amount <= 0.001f) return;
        D2D1_COLOR_F top = onFillTop;
        D2D1_COLOR_F bottom = onFillBottom;
        top.a *= amount;
        bottom.a *= amount;
        const D2D1_GRADIENT_STOP stops[2] = {{0.0f, top}, {1.0f, bottom}};
        ID2D1GradientStopCollection* collection = nullptr;
        if (FAILED(context->CreateGradientStopCollection(stops, 2, &collection)) || !collection) {
            return;
        }
        ID2D1LinearGradientBrush* brush = nullptr;
        const D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES properties = D2D1::LinearGradientBrushProperties(
            D2D1::Point2F(rounded.rect.left, rounded.rect.top),
            D2D1::Point2F(rounded.rect.right, rounded.rect.bottom));
        if (SUCCEEDED(context->CreateLinearGradientBrush(properties, collection, &brush)) && brush) {
            context->FillRoundedRectangle(&rounded, brush);
            brush->Release();
        }
        collection->Release();
    };
    const auto mixColour = [](const D2D1_COLOR_F& from, const D2D1_COLOR_F& to,
                              float amount) {
        const float t = std::clamp(amount, 0.0f, 1.0f);
        return D2D1::ColorF(from.r + (to.r - from.r) * t,
                            from.g + (to.g - from.g) * t,
                            from.b + (to.b - from.b) * t,
                            from.a + (to.a - from.a) * t);
    };

    for (int i = 0; i < layout.tileCount; ++i) {
        const SystemPanelTile& tile = text.tiles[i];
        const RectF& rect = layout.tiles[i];
        if (rect.right <= rect.left || rect.bottom <= rect.top) continue;

        constexpr float kTileRadius = 18.0f;
        const float animatedOn = std::clamp(tile.onAmount, 0.0f, 1.0f);
        const float onAmount = animatedOn > 0.001f ? animatedOn : (tile.on ? 1.0f : 0.0f);
        const float hoverAmount = std::clamp(tile.hoverAmount, 0.0f, 1.0f);
        const float pressAmount = std::clamp(tile.pressAmount, 0.0f, 1.0f);
        const D2D1_ROUNDED_RECT rounded = {
            D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom), kTileRadius, kTileRadius};

        // ON and OFF are different FILLS, which is the whole point of a tile as against a row of text:
        // the state is visible before the label is read. ON takes the reference's blue gradient; OFF is
        // the card's ink at 6%, with a hairline so it still reads as a surface.
        if (onAmount < 0.999f) {
            if (ID2D1SolidColorBrush* fill =
                    BrushFor(context, island::CardInk(0.055f * (1.0f - onAmount) * opacity))) {
                context->FillRoundedRectangle(&rounded, fill);
            }
            if (ID2D1SolidColorBrush* edge =
                    BrushFor(context, island::CardInk(0.09f * (1.0f - onAmount) * opacity))) {
                context->DrawRoundedRectangle(&rounded, edge, 1.0f);
            }
        }
        if (onAmount > 0.001f) {
            fillOnTile(rounded, onAmount);
            if (ID2D1SolidColorBrush* edge =
                    BrushFor(context, D2D1::ColorF(
                                           0.49f, 0.83f, 0.99f, 0.32f * onAmount * opacity))) {
                context->DrawRoundedRectangle(&rounded, edge, 1.0f);
            }
        }
        if (hoverAmount > 0.001f) {
            if (ID2D1SolidColorBrush* wash =
                    BrushFor(context, D2D1::ColorF(
                                           1.0f, 1.0f, 1.0f, 0.065f * hoverAmount * opacity))) {
                context->FillRoundedRectangle(&rounded, wash);
            }
        }
        if (pressAmount > 0.001f) {
            if (ID2D1SolidColorBrush* pressed =
                    BrushFor(context, D2D1::ColorF(
                                           0.0f, 0.0f, 0.0f, 0.09f * pressAmount * opacity))) {
                context->FillRoundedRectangle(&rounded, pressed);
            }
        }

        // The glyph at the tile's TOP LEFT, white on an ON tile and the card's ink on an OFF one -- the
        // theme's ink would be dark on the blue in the light themes.
        const D2D1_COLOR_F iconOn = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f * opacity);
        const D2D1_COLOR_F iconOff = island::CardInk(0.92f * opacity);
        const D2D1_COLOR_F iconInk = mixColour(iconOff, iconOn, onAmount);
        RectF iconBox = layout.tileIcons[i];
        iconBox.top += pressAmount;
        iconBox.bottom += pressAmount;
        DrawIndicatorGlyph(context, tile.icon, iconBox, tile.iconValue, tile.label,
                           opacity, panelIconFormat_, bodyFormat_, &iconInk);

        // The chevron at the tile's TOP RIGHT, for the tiles that open a list (see SystemPanelTile).
        if (tile.chevron && gearGlyphFormat_) {
            const D2D1_COLOR_F chevronColour =
                mixColour(island::CardInk(0.62f * opacity),
                          D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.85f * opacity), onAmount);
            if (ID2D1SolidColorBrush* chevronBrush = BrushFor(context, chevronColour)) {
                constexpr float side = 14.0f;
                const D2D1_RECT_F area = D2D1::RectF(
                    rect.right - side - 16.0f, rect.top + 18.0f + pressAmount,
                    rect.right - 16.0f, rect.top + 18.0f + side + pressAmount);
                const wchar_t chevron[] = {L'\uE76C', L'\0'};   // ChevronRight, Segoe Fluent Icons
                context->DrawTextW(chevron, 1, gearGlyphFormat_, area, chevronBrush,
                                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }

        // The name, then the state under it: both INSIDE the tile's lower left, the reference's
        // hierarchy (13 semibold white over 11 dim).
        if (tile.label && tile.label[0] != L'\0') {
            const D2D1_COLOR_F labelColour =
                mixColour(island::CardInk(0.90f * opacity),
                          D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f * opacity), onAmount);
            RectF labelBox = layout.tileLabels[i];
            labelBox.top += pressAmount;
            labelBox.bottom += pressAmount;
            if (ID2D1SolidColorBrush* labelBrush = BrushFor(context, labelColour)) {
                DrawTextIn(context, panelLabelFormat_, tile.label, labelBox, labelBrush,
                           DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
        }
        if (tile.stateText && tile.stateText[0] != L'\0') {
            const D2D1_COLOR_F subColour =
                mixColour(island::CardInk(0.55f * opacity),
                          D2D1::ColorF(0.86f, 0.94f, 1.0f, 0.86f * opacity), onAmount);
            RectF subLabelBox = layout.tileSubLabels[i];
            subLabelBox.top += pressAmount;
            subLabelBox.bottom += pressAmount;
            if (ID2D1SolidColorBrush* subBrush = BrushFor(context, subColour)) {
                DrawTextIn(context, panelSubFormat_, tile.stateText, subLabelBox,
                           subBrush, DWRITE_TEXT_ALIGNMENT_LEADING,
                           DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
        }
    }

    // THE TWO CONTROL ROWS: a mark and a title at the left of the header, the reading at the right, and
    // a 4 DIP track with a white handle in the row below. Each row carries its OWN mark and its OWN
    // reading -- the speaker with the volume, the sun with the brightness.
    const auto clamped = [](float value) {
        return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    };
    for (int i = 0; i < layout.sliderCount; ++i) {
        const RectF& track = layout.sliders[i];
        if (track.right <= track.left || track.bottom <= track.top) continue;
        const float centreY = (track.top + track.bottom) * 0.5f;
        constexpr float barHeight = 6.0f;
        const float radius = barHeight * 0.5f;

        const bool brightnessRow = i == 1;
        const bool mutedRow = !brightnessRow && text.muted;
        const float fraction = clamped(brightnessRow ? text.brightness : text.volume);

        // The mark and the row's title, at the left of the header.
        const RectF& icon = brightnessRow ? layout.brightnessIcon : layout.speakerIcon;
        DrawIndicatorGlyph(context,
                           brightnessRow ? IndicatorIcon::Brightness : IndicatorIcon::Volume, icon,
                           brightnessRow ? static_cast<int>(fraction * 100.0f + 0.5f)
                                         : (text.muted ? 0
                                                       : static_cast<int>(fraction * 100.0f + 0.5f)),
                           nullptr, opacity, panelIconFormat_, bodyFormat_);
        const wchar_t* rowTitle = brightnessRow ? L"\u663E\u793A\u4EAE\u5EA6" : L"\u97F3\u91CF";
        if (ID2D1SolidColorBrush* labelBrush =
                BrushFor(context, island::CardInk(0.62f * opacity))) {
            DrawTextIn(context, panelSubFormat_, rowTitle, layout.sliderLabels[i], labelBrush,
                       DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        // The reading at the right of the header. The brightness one is the reference's warm orange; the
        // volume one is the card's bright ink. It is ALWAYS shown -- on a settings surface the number is
        // the reading, not a tooltip that appears while the control moves. A muted row reads 0%, the same
        // as its track, so the number and the bar cannot disagree.
        if (volumeFormat_) {
            const float shownFraction = mutedRow ? 0.0f : fraction;
            wchar_t reading[16] = {};
            std::swprintf(reading, 16, L"%d%%",
                          static_cast<int>(shownFraction * 100.0f + 0.5f));
            const bool highlighted =
                brightnessRow ? text.brightnessHighlighted : text.volumeHighlighted;
            const float valueAlpha = (highlighted ? 0.98f : 0.84f) * opacity;
            const D2D1_COLOR_F ink = brightnessRow
                                         ? D2D1::ColorF(0.98f, 0.75f, 0.14f, valueAlpha)
                                         : island::CardInk(valueAlpha);
            if (ID2D1SolidColorBrush* valueBrush = BrushFor(context, ink)) {
                DrawTextIn(context, volumeFormat_, reading, layout.sliderValues[i], valueBrush,
                           DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
        }

        // The track, the filled portion, then the handle: drawn in that order so the fill covers the
        // track and the handle covers the fill.
        if (ID2D1SolidColorBrush* trackBrush =
                BrushFor(context, island::CardInk(0.22f * opacity))) {
            const D2D1_ROUNDED_RECT rounded = {
                D2D1::RectF(track.left, centreY - barHeight * 0.5f, track.right,
                            centreY + barHeight * 0.5f),
                radius, radius};
            context->FillRoundedRectangle(&rounded, trackBrush);
        }

        // Muted draws NO fill even at a non-zero level, so the state is visible in the bar itself and
        // not only in the glyph.
        const float shown = mutedRow ? 0.0f : fraction;
        const float handleX = track.left + (track.right - track.left) * shown;
        if (shown > 0.0f && handleX > track.left + 0.5f) {
            const D2D1_COLOR_F themeAccent = island::CurrentTheme().accent;
            const D2D1_COLOR_F accent =
                D2D1::ColorF(themeAccent.r, themeAccent.g, themeAccent.b, themeAccent.a * opacity);
            if (ID2D1SolidColorBrush* fillBrush = BrushFor(context, accent)) {
                const D2D1_ROUNDED_RECT rounded = {
                    D2D1::RectF(track.left, centreY - barHeight * 0.5f, handleX,
                                centreY + barHeight * 0.5f),
                    radius, radius};
                context->FillRoundedRectangle(&rounded, fillBrush);
            }
        }

        if (ID2D1SolidColorBrush* handleBrush =
                BrushFor(context, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f * opacity))) {
            // The handle is a touch larger while its row is the one being used (see sliderEmphasis).
            const float emphasis = clamped(text.sliderEmphasis[i]);
            const float grown = 8.5f + 2.5f * emphasis;
            const D2D1_ELLIPSE handle =
                D2D1::Ellipse(D2D1::Point2F(handleX, centreY), grown, grown);
            context->FillEllipse(&handle, handleBrush);
        }
    }

    // The output-device button at the right of the volume header: the mark, then the chevron that says it
    // leads somewhere. It is the shell's own arrangement, and it is the door into the 声音输出 page.
    if (layout.sliderCount > 0 && layout.volumeButton.right > layout.volumeButton.left) {
        const float centreY = (layout.volumeButton.top + layout.volumeButton.bottom) * 0.5f;
        const float markSide = 20.0f;   // the mark's own square; see DrawOutputDeviceMark
        const RectF mark = {layout.volumeButton.left, centreY - markSide * 0.5f,
                            layout.volumeButton.left + markSide, centreY + markSide * 0.5f};
        DrawOutputDeviceMark(context, mark, opacity);

        if (gearGlyphFormat_) {
            ID2D1SolidColorBrush* chevronBrush = nullptr;
            const D2D1_COLOR_F ink = island::CardInk(0.72f * opacity);
            if (SUCCEEDED(context->CreateSolidColorBrush(ink, &chevronBrush)) && chevronBrush) {
                constexpr float side = 13.0f;
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

    // THE FOOTER: an inset rounded plate, a two-line power reading at the left, and three round buttons
    // at the right. This is the red-boxed area in the reference; it must read as its own control surface.
    if (layout.footer.right > layout.footer.left && layout.footer.bottom > layout.footer.top) {
        constexpr float kFooterRadius = 20.0f;
        const D2D1_ROUNDED_RECT footerPlate = {
            D2D1::RectF(layout.footer.left, layout.footer.top, layout.footer.right,
                        layout.footer.bottom),
            kFooterRadius, kFooterRadius};
        if (ID2D1SolidColorBrush* plate =
                BrushFor(context, island::CardInk(0.04f * opacity))) {
            context->FillRoundedRectangle(&footerPlate, plate);
        }
        if (ID2D1SolidColorBrush* edge = BrushFor(context, island::CardInk(0.065f * opacity))) {
            context->DrawRoundedRectangle(&footerPlate, edge, 1.0f);
        }
        if (ID2D1SolidColorBrush* highlight =
                BrushFor(context, island::ThemeHighlight(0.08f * opacity))) {
            context->DrawLine(
                D2D1::Point2F(layout.footer.left + kFooterRadius, layout.footer.top + 1.0f),
                D2D1::Point2F(layout.footer.right - kFooterRadius, layout.footer.top + 1.0f),
                highlight, 1.0f);
        }
        if (text.footerHovered == 0 && layout.footerBattery.right > layout.footerBattery.left) {
            const D2D1_ROUNDED_RECT batteryHighlight = {
                D2D1::RectF(layout.footerBattery.left + 2.0f, layout.footerBattery.top + 3.0f,
                            layout.footerBattery.right - 2.0f, layout.footerBattery.bottom - 3.0f),
                14.0f, 14.0f};
            if (ID2D1SolidColorBrush* fill =
                    BrushFor(context, island::ThemeHighlight((text.footerPressed == 0 ? 0.14f : 0.08f) *
                                                             opacity))) {
                context->FillRoundedRectangle(&batteryHighlight, fill);
            }
        }

        // Two lines, matching the reference's status block. A desktop keeps the familiar 100% face but
        // names itself in the secondary line, so the user's requested reading does not masquerade as a
        // detected battery or an active charging animation.
        if (layout.footerText.right > layout.footerText.left) {
            const RectF iconBox = {layout.footerText.left,
                                   (layout.footerText.top + layout.footerSubText.bottom) * 0.5f -
                                       11.0f,
                                   layout.footerText.left + 22.0f,
                                   (layout.footerText.top + layout.footerSubText.bottom) * 0.5f +
                                       11.0f};
            DrawIndicatorGlyph(context, IndicatorIcon::Battery, iconBox,
                               text.batteryPercent >= 0 ? text.batteryPercent : -1,
                               nullptr, opacity, panelIconFormat_, bodyFormat_);

            wchar_t primary[64] = {};
            wchar_t secondary[64] = {};
            if (text.batteryPresent) {
                std::swprintf(primary, 64, L"%d%%  %ls", text.batteryPercent,
                              text.charging ? L"\u5145\u7535\u4E2D" : L"\u7535\u6C60");
                std::swprintf(secondary, 64, L"%ls",
                              text.charging ? L"\u5DF2\u8FDE\u63A5\u7535\u6E90"
                                            : L"\u7535\u6C60\u4F9B\u7535");
            } else {
                std::swprintf(primary, 64, L"100%%  \u7535\u6C60");
                std::swprintf(secondary, 64, L"\u53F0\u5F0F\u673A \u00B7 \u4F7F\u7528\u7535\u6E90");
            }
            RectF primaryBox = layout.footerText;
            primaryBox.left = iconBox.right + 8.0f;
            if (ID2D1SolidColorBrush* ink = BrushFor(context, island::CardInk(0.84f * opacity))) {
                DrawTextIn(context, panelLabelFormat_, primary, primaryBox, ink,
                           DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
            if (ID2D1SolidColorBrush* ink = BrushFor(context, island::CardInk(0.48f * opacity))) {
                DrawTextIn(context, panelSubFormat_, secondary, layout.footerSubText, ink,
                           DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
        }

        // The three circular buttons. Their hit targets are NightLight, FooterMute and Gear; the moon's
        // active state is Night Light, and the speaker's is the same mute reading as the volume row.
        const wchar_t marks[3] = {IndicatorGlyph(IndicatorIcon::Moon, 0),
                                  IndicatorGlyph(IndicatorIcon::Volume, text.muted ? 0 : 1),
                                  IndicatorGlyph(IndicatorIcon::Settings, 0)};
        for (int i = 0; i < 3; ++i) {
            const RectF& button = layout.footerButtons[i];
            if (button.right <= button.left || button.bottom <= button.top) continue;
            const bool active = (i == 0 && text.nightLightOn) || (i == 1 && text.muted);
            const bool hovered = text.footerHovered == i + 1;
            const bool pressed = text.footerPressed == i + 1;
            const float pressOffset = pressed ? 1.0f : 0.0f;
            const D2D1_ELLIPSE circle = D2D1::Ellipse(
                D2D1::Point2F((button.left + button.right) * 0.5f,
                              (button.top + button.bottom) * 0.5f + pressOffset),
                (button.right - button.left) * 0.5f, (button.bottom - button.top) * 0.5f);
            const float fillAlpha = active ? 0.20f : (pressed ? 0.15f : (hovered ? 0.11f : 0.06f));
            if (ID2D1SolidColorBrush* fill = BrushFor(context, island::CardInk(fillAlpha * opacity))) {
                context->FillEllipse(&circle, fill);
            }
            if (ID2D1SolidColorBrush* edge =
                    BrushFor(context, island::CardInk((active ? 0.25f : 0.11f) * opacity))) {
                context->DrawEllipse(&circle, edge, 1.0f);
            }
            if (panelIconFormat_ && marks[i] != L'\0') {
                if (ID2D1SolidColorBrush* mark =
                        BrushFor(context, island::CardInk((active ? 1.0f : 0.78f) * opacity))) {
                    const D2D1_RECT_F area =
                        D2D1::RectF(button.left, button.top + pressOffset, button.right,
                                    button.bottom + pressOffset);
                    const wchar_t glyph = marks[i];
                    context->DrawTextW(&glyph, 1, panelIconFormat_, area, mark,
                                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }
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
            if (SUCCEEDED(context->CreateSolidColorBrush(CardInk(alpha),
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
        const D2D1_COLOR_F arrowInk = CardInk(0.70f * opacity);
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

    const auto brushFor = [&](float alpha, ID2D1SolidColorBrush** brush) {
        *brush = nullptr;
        return SUCCEEDED(context->CreateSolidColorBrush(
                   island::CardInk(alpha * opacity), brush)) &&
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

    ID2D1Factory* d2dFactory = nullptr;
    context->GetFactory(&d2dFactory);

    const auto rounded = [](const RectF& rect, float radius) {
        return D2D1::RoundedRect(D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom), radius,
                                 radius);
    };
    // A filled / outlined rounded surface. BrushFor owns the brush (see its note in the header), so nothing
    // here is released and a wash per item costs a cache lookup rather than a brush.
    const auto fillRect = [&](const RectF& rect, float radius, const D2D1_COLOR_F& colour) {
        if (rect.right <= rect.left || rect.bottom <= rect.top) return;
        if (ID2D1SolidColorBrush* brush = BrushFor(context, colour)) {
            const D2D1_ROUNDED_RECT shape = rounded(rect, radius);
            context->FillRoundedRectangle(&shape, brush);
        }
    };
    const auto strokeRect = [&](const RectF& rect, float radius, const D2D1_COLOR_F& colour,
                                float width, ID2D1StrokeStyle* style = nullptr) {
        if (rect.right <= rect.left || rect.bottom <= rect.top) return;
        if (ID2D1SolidColorBrush* brush = BrushFor(context, colour)) {
            const D2D1_ROUNDED_RECT shape = rounded(rect, radius);
            if (style) {
                context->DrawRoundedRectangle(&shape, brush, width, style);
            } else {
                context->DrawRoundedRectangle(&shape, brush, width);
            }
        }
    };
    const auto drawText = [&](IDWriteTextFormat* format, const wchar_t* line, const RectF& box,
                              const D2D1_COLOR_F& colour, DWRITE_TEXT_ALIGNMENT horizontal,
                              DWRITE_PARAGRAPH_ALIGNMENT vertical) {
        if (!format || !line || line[0] == L'\0' || box.right <= box.left) return;
        if (ID2D1SolidColorBrush* brush = BrushFor(context, colour)) {
            DrawTextIn(context, format, line, box, brush, horizontal, vertical);
        }
    };
    // An image inside a rounded plate. This is what makes a 56 DIP application picture an ICON rather than a
    // square that happens to sit on the card: an opaque PNG would otherwise bring its own square corners.
    const auto drawRoundedBitmap = [&](ID2D1Bitmap1* bitmap, const RectF& box, float radius) {
        if (!bitmap || box.right <= box.left) return;
        ID2D1RoundedRectangleGeometry* clip = nullptr;
        if (d2dFactory) {
            d2dFactory->CreateRoundedRectangleGeometry(rounded(box, radius), &clip);
        }
        if (clip) {
            context->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), clip,
                                                      D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                               nullptr);
        }
        const D2D1_RECT_F target = D2D1::RectF(box.left, box.top, box.right, box.bottom);
        context->DrawBitmap(bitmap, &target, opacity, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        if (clip) {
            context->PopLayer();
            clip->Release();
        }
    };
    // The highlight for one item: its own animated amount when the caller supplied them, and a hard 0/1
    // otherwise (which keeps every other caller -- and the drawings the self-test makes -- unchanged).
    const auto washAt = [&](const float* amounts, int index, int hovered) {
        if (index < 0) return 0.0f;
        if (amounts) return amounts[index];
        return index == hovered ? 1.0f : 0.0f;
    };

    // --- the page row: two adjacent launcher tabs ------------------------------------------------
    {
        const bool allSelected = text.page == 1;
        const bool pinnedSelected = !allSelected;
        const float tabRadius = island::kStartTabHeightDip * 0.5f;
        const D2D1_COLOR_F selectedFill = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.78f * opacity);
        const D2D1_COLOR_F selectedEdge = CardInk(0.075f * opacity);
        const D2D1_COLOR_F quietFill = CardInk(0.035f * opacity);

        if (pinnedSelected) {
            fillRect(layout.pinnedTab, tabRadius, selectedFill);
            strokeRect(layout.pinnedTab, tabRadius, selectedEdge, 1.0f);
        } else if (text.pinnedTabHovered) {
            fillRect(layout.pinnedTab, tabRadius, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.42f * opacity));
        } else {
            fillRect(layout.pinnedTab, tabRadius, quietFill);
        }
        const island::Theme& theme = island::CurrentTheme();
        const float pinnedCentreY = (layout.pinnedTab.top + layout.pinnedTab.bottom) * 0.5f;
        if (ID2D1SolidColorBrush* dot = BrushFor(
                context, D2D1::ColorF(theme.accent.r, theme.accent.g, theme.accent.b,
                                      (pinnedSelected ? 0.98f : 0.46f) * opacity))) {
            context->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(layout.pinnedTab.left + 15.0f, pinnedCentreY),
                              3.2f, 3.2f),
                dot);
        }
        wchar_t pinnedLabel[32] = {};
        std::swprintf(pinnedLabel, 32, L"\u56FA\u5B9A\u5E38\u7528  %d", text.pinCount);
        drawText(startRowFormat_, pinnedLabel,
                 {layout.pinnedTab.left + 22.0f, layout.pinnedTab.top,
                  layout.pinnedTab.right - 8.0f, layout.pinnedTab.bottom},
                 CardInk((pinnedSelected ? 0.86f : 0.56f) * opacity),
                 DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        // The all-applications destination is a peer tab, exactly as in the reference, rather than a
        // lone grid glyph in the far corner.
        const float buttonRadius = (layout.allAppsButton.bottom - layout.allAppsButton.top) * 0.5f;
        if (allSelected) {
            fillRect(layout.allAppsButton, buttonRadius, selectedFill);
            strokeRect(layout.allAppsButton, buttonRadius, selectedEdge, 1.0f);
        } else if (text.allAppsHovered) {
            fillRect(layout.allAppsButton, buttonRadius,
                     D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.42f * opacity));
        } else {
            fillRect(layout.allAppsButton, buttonRadius, quietFill);
        }
        wchar_t allLabel[32] = {};
        std::swprintf(allLabel, 32, L"\u5168\u90E8\u5E94\u7528  %d", text.listCount);
        drawText(startRowFormat_, allLabel, layout.allAppsButton,
                 CardInk((allSelected ? 0.86f : 0.56f) * opacity),
                 DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // --- one application: its picture and its name -------------------------------------------------
    //
    // Shared by the pins and the all-applications grid, because the two are the same tile -- the reference
    // draws the launcher's grid once, and the only difference between "pinned" and "all" is which list the
    // tile came from. `wash` is the highlight: at 1 it also draws the selected ring around the icon.
    const auto drawAppTile = [&](const RectF& cell, const RectF& iconBox, const RectF& labelBox,
                                 ID2D1Bitmap1* bitmap, const wchar_t* name, float wash,
                                 int resultKind = -1) {
        if (wash > 0.01f) {
            fillRect(cell, 12.0f, CardInk(0.055f * wash * opacity));
        }
        // The reference gives every application a quiet plate. It makes transparent icons legible on the
        // lavender card and gives the grid a consistent rhythm without covering the artwork itself.
        const RectF plateShadow = {iconBox.left + 1.0f, iconBox.top + 3.0f,
                                   iconBox.right + 1.0f, iconBox.bottom + 3.0f};
        fillRect(plateShadow, island::kStartAppIconRadiusDip, CardInk(0.028f * opacity));
        fillRect(iconBox, island::kStartAppIconRadiusDip,
                 D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.74f * opacity));
        strokeRect(iconBox, island::kStartAppIconRadiusDip, CardInk(0.065f * opacity), 1.0f);
        if (bitmap) {
            drawRoundedBitmap(bitmap, iconBox, island::kStartAppIconRadiusDip);
        } else {
            // An icon that has not arrived yet holds the exact final geometry with a deterministic
            // monogram. This is deliberately not an empty dot: a failed Shell extraction must still look
            // like an application, and the tile must not jump when the real image arrives.
            if (resultKind == static_cast<int>(island::SearchResultKind::File)) {
                // A file result gets a deliberate document glyph rather than the first Chinese character.
                const float width = iconBox.right - iconBox.left;
                const float height = iconBox.bottom - iconBox.top;
                const RectF page = {iconBox.left + width * 0.27f, iconBox.top + height * 0.18f,
                                    iconBox.right - width * 0.24f, iconBox.bottom - height * 0.18f};
                strokeRect(page, 3.0f, CardInk(0.44f * opacity), 1.2f);
                if (ID2D1SolidColorBrush* line = BrushFor(context, CardInk(0.28f * opacity))) {
                    const float left = page.left + 5.0f;
                    const float right = page.right - 5.0f;
                    for (int row = 0; row < 3; ++row) {
                        const float y = page.top + 8.0f + static_cast<float>(row) * 5.0f;
                        context->DrawLine(D2D1::Point2F(left, y), D2D1::Point2F(right, y), line, 1.0f);
                    }
                }
            } else if (resultKind == static_cast<int>(island::SearchResultKind::Setting)) {
                // Three tiny sliders communicate "settings" without depending on a font glyph.
                if (ID2D1SolidColorBrush* line = BrushFor(context, CardInk(0.48f * opacity))) {
                    const float left = iconBox.left + 11.0f;
                    const float right = iconBox.right - 11.0f;
                    for (int row = 0; row < 3; ++row) {
                        const float y = iconBox.top + 13.0f + static_cast<float>(row) * 9.0f;
                        context->DrawLine(D2D1::Point2F(left, y), D2D1::Point2F(right, y), line, 1.2f);
                        const float knobX = left + (right - left) * (0.28f + 0.23f * row);
                        context->FillEllipse(
                            D2D1::Ellipse(D2D1::Point2F(knobX, y), 3.2f, 3.2f), line);
                    }
                }
            } else {
                wchar_t initial[2] = {name && name[0] ? name[0] : L'?', L'\0'};
                drawText(startRowFormat_, initial, iconBox, CardInk(0.68f * opacity),
                         DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
        }
        // The selected/hover tile: the reference outlines the whole CELL, not the
        // bitmap, so real application artwork keeps its own edge.
        if (wash > 0.5f) {
            strokeRect(cell, 12.0f, CardInk(0.16f * opacity), 1.0f);
        }
        if (startLabelFormat_ && name && name[0] != L'\0') {
            if (ID2D1SolidColorBrush* nameBrush =
                    BrushFor(context, CardInk((0.72f + 0.22f * wash) * opacity))) {
                const D2D1_RECT_F target =
                    D2D1::RectF(labelBox.left, labelBox.top, labelBox.right, labelBox.bottom);
                context->DrawTextW(name, static_cast<UINT32>(std::wcslen(name)), startLabelFormat_,
                                   target, nameBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }
    };

    if (text.page == 1 && text.searchActive) {
        const int count = text.searchResultCount < island::kStartSearchMaxResults
                              ? text.searchResultCount
                              : island::kStartSearchMaxResults;
        island::SearchResultKind kinds[island::kStartSearchMaxResults] = {};
        if (text.searchResults) {
            for (int i = 0; i < count; ++i) kinds[i] = text.searchResults[i].kind;
        }
        const island::StartSearchListLayout results =
            (text.searchResults && count > 0)
                ? island::StartSearchListFor(layout, kinds, count)
                : island::StartSearchListLayout{};

        if (count == 0) {
            const wchar_t* message =
                (text.searchPending || !text.searchFilesComplete)
                    ? L"\u6B63\u5728\u641C\u7D22\u5E94\u7528\u3001\u8BBE\u7F6E\u548C\u6587\u4EF6\u2026"
                    : L"\u6CA1\u6709\u627E\u5230\u5339\u914D\u9879";
            drawText(startRowFormat_, message,
                     {layout.grid.left, layout.grid.top, layout.grid.right,
                      layout.grid.top + 44.0f},
                     CardInk(0.56f * opacity), DWRITE_TEXT_ALIGNMENT_CENTER,
                     DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        static constexpr const wchar_t* kGroupLabels[island::kStartSearchGroupCount] = {
            L"\u5E94\u7528", L"Windows \u8BBE\u7F6E", L"\u6587\u4EF6"};
        for (int group = 0; group < island::kStartSearchGroupCount; ++group) {
            if (!results.valid || results.groupCounts[group] == 0) continue;
            const RectF& header = results.headers[group];
            drawText(startRowFormat_, kGroupLabels[group],
                     {header.left, header.top, header.right - 32.0f, header.bottom},
                     CardInk(0.50f * opacity), DWRITE_TEXT_ALIGNMENT_LEADING,
                     DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            wchar_t groupCount[16] = {};
            std::swprintf(groupCount, 16, L"%d", results.groupCounts[group]);
            drawText(startCaptionFormat_, groupCount,
                     {header.right - 28.0f, header.top, header.right, header.bottom},
                     CardInk(0.34f * opacity), DWRITE_TEXT_ALIGNMENT_TRAILING,
                     DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        for (int i = 0; i < count && results.valid; ++i) {
            const island::StartSearchResult& result = text.searchResults[i];
            const RectF& row = results.rows[i];
            const float hoverWash = washAt(text.rowWashes, i, text.hoveredSearchResult);
            const float wash =
                text.selectedSearchResult == i ? 1.0f : hoverWash * 0.62f;
            if (wash > 0.01f) {
                fillRect(row, 8.0f, CardInk(0.075f * wash * opacity));
                strokeRect(row, 8.0f,
                           CardInk((0.08f + 0.10f * wash) * opacity), 1.0f);
            }

            const float iconSide = 22.0f;
            const float iconTop = row.top + (row.bottom - row.top - iconSide) * 0.5f;
            const RectF iconBox = {row.left + 8.0f, iconTop, row.left + 8.0f + iconSide,
                                   iconTop + iconSide};
            const uint8_t* png = result.iconPng.empty() ? nullptr : result.iconPng.data();
            size_t pngSize = result.iconPng.size();
            if (result.kind == island::SearchResultKind::Application && text.listIcons &&
                result.sourceIndex >= 0) {
                png = text.listIcons[result.sourceIndex];
                pngSize = text.listIconSizes ? text.listIconSizes[result.sourceIndex] : 0;
            }
            ID2D1Bitmap1* icon = StartBitmapFor(context, &startListBitmaps_[i], png, pngSize);
            if (icon) {
                drawRoundedBitmap(icon, iconBox, 5.0f);
            } else if (result.kind == island::SearchResultKind::File) {
                const RectF page = {iconBox.left + 5.0f, iconBox.top + 3.0f,
                                    iconBox.right - 4.0f, iconBox.bottom - 3.0f};
                strokeRect(page, 2.0f, CardInk(0.46f * opacity), 1.0f);
                if (ID2D1SolidColorBrush* line = BrushFor(context, CardInk(0.30f * opacity))) {
                    context->DrawLine(D2D1::Point2F(page.left + 3.0f, page.top + 6.0f),
                                      D2D1::Point2F(page.right - 3.0f, page.top + 6.0f),
                                      line, 1.0f);
                    context->DrawLine(D2D1::Point2F(page.left + 3.0f, page.top + 10.0f),
                                      D2D1::Point2F(page.right - 3.0f, page.top + 10.0f),
                                      line, 1.0f);
                }
            } else if (result.kind == island::SearchResultKind::Setting) {
                if (ID2D1SolidColorBrush* line = BrushFor(context, CardInk(0.48f * opacity))) {
                    for (int lineIndex = 0; lineIndex < 3; ++lineIndex) {
                        const float y = iconBox.top + 6.0f + 5.0f * lineIndex;
                        context->DrawLine(D2D1::Point2F(iconBox.left + 4.0f, y),
                                          D2D1::Point2F(iconBox.right - 4.0f, y), line, 1.0f);
                        const float knobX =
                            iconBox.left + 4.0f +
                            (iconBox.right - iconBox.left - 8.0f) *
                                (0.25f + 0.25f * lineIndex);
                        context->FillEllipse(
                            D2D1::Ellipse(D2D1::Point2F(knobX, y), 1.8f, 1.8f), line);
                    }
                }
            } else {
                fillRect(iconBox, 5.0f, CardInk(0.07f * opacity));
                wchar_t initial[2] = {
                    result.title.empty() ? L'?' : result.title.front(), L'\0'};
                drawText(startRowFormat_, initial, iconBox, CardInk(0.68f * opacity),
                         DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }

            const float subtitleWidth = 150.0f;
            drawText(startRowFormat_, result.title.c_str(),
                     {iconBox.right + 10.0f, row.top, row.right - subtitleWidth - 12.0f,
                      row.bottom},
                     CardInk(0.88f * opacity), DWRITE_TEXT_ALIGNMENT_LEADING,
                     DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            drawText(startCaptionFormat_, result.subtitle.c_str(),
                     {row.right - subtitleWidth - 8.0f, row.top, row.right - 10.0f,
                      row.bottom},
                     CardInk(0.38f * opacity), DWRITE_TEXT_ALIGNMENT_TRAILING,
                     DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    } else if (text.page == 1) {
        // --- THE ALL-APPLICATIONS / SEARCH GRID: the same six-column cells the pins use ---------------
        const int columns = island::kStartColumns;
        if (text.listCount == 0) {
            drawText(startRowFormat_, L"\u6CA1\u6709\u53EF\u663E\u793A\u7684\u5E94\u7528",
                     {layout.grid.left, layout.grid.top, layout.grid.right,
                      layout.grid.top + 44.0f},
                     CardInk(0.56f * opacity), DWRITE_TEXT_ALIGNMENT_CENTER,
                     DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        for (int visible = 0; visible < island::kStartRows * columns; ++visible) {
            const int shownIndex = text.pickerScrollRow * columns + visible;
            if (shownIndex < 0 || shownIndex >= text.listCount) break;
            const int index = text.listOrder ? text.listOrder[shownIndex] : shownIndex;
            const RectF cell = island::StartPinCellFor(layout, visible);
            const RectF iconBox = island::StartPinIconFor(layout, visible);
            const RectF labelBox = island::StartPinLabelFor(layout, visible);
            const float wash = washAt(text.rowWashes, shownIndex, text.hoveredPickerRow);
            const uint8_t* png = text.listIcons ? text.listIcons[index] : nullptr;
            const size_t pngSize = text.listIconSizes ? text.listIconSizes[index] : 0;
            ID2D1Bitmap1* icon =
                StartBitmapFor(context, &startListBitmaps_[visible], png, pngSize);
            drawAppTile(cell, iconBox, labelBox, icon,
                        text.list ? text.list[index].name : L"", wash);
        }
        if (text.listCount > 0) {
            const int totalRows = (text.listCount + columns - 1) / columns;
            const float gridHeight = layout.grid.bottom - layout.grid.top;
            const float visibleFraction = static_cast<float>(island::kStartRows) /
                                          static_cast<float>(totalRows > 0 ? totalRows : 1);
            const float barHeight = std::max(24.0f, gridHeight * visibleFraction);
            const float maxRow =
                static_cast<float>(std::max(1, totalRows - island::kStartRows));
            const float travelled =
                layout.grid.top + (gridHeight - barHeight) *
                                      (static_cast<float>(text.pickerScrollRow) / maxRow);
            fillRect({layout.grid.right + 6.0f, travelled, layout.grid.right + 9.0f,
                      travelled + barHeight},
                     1.5f, CardInk(0.07f * opacity));
        }
    } else {
        // --- the pins, and the add tile at their end -------------------------------------------------
        const int capacity = island::StartPinCapacity();
        const int shown = text.pinCount < capacity ? text.pinCount : capacity;
        for (int i = 0; i < shown; ++i) {
            const RectF cell = island::StartPinCellFor(layout, i);
            const RectF iconBox = island::StartPinIconFor(layout, i);
            const RectF labelBox = island::StartPinLabelFor(layout, i);
            const float wash = washAt(text.pinWashes, i, text.hoveredPin);
            const uint8_t* png =
                (text.pinIcons && i < static_cast<int>(sizeof(startPinBitmaps_) /
                                                       sizeof(startPinBitmaps_[0])))
                    ? text.pinIcons[i]
                    : nullptr;
            const size_t pngSize = text.pinIconSizes ? text.pinIconSizes[i] : 0;
            ID2D1Bitmap1* icon = StartBitmapFor(context, &startPinBitmaps_[i], png, pngSize);
            drawAppTile(cell, iconBox, labelBox, icon, text.pins ? text.pins[i].name : L"", wash);
        }

        // THE ADD TILE, in the cell after the last pin: the reference's dashed "+ 添加快捷键" slot, which
        // is what makes "the upper area is yours to fill" visible without pressing the blank area first. It
        // is a real target (StartPageTarget::AddTile) and opens the same picker the empty grid does.
        if (shown < capacity) {
            const RectF cell = island::StartPinCellFor(layout, shown);
            const RectF iconBox = island::StartPinIconFor(layout, shown);
            const RectF labelBox = island::StartPinLabelFor(layout, shown);
            const float wash = text.addTileHovered ? 1.0f : 0.0f;
            if (wash > 0.01f) {
                fillRect(cell, 16.0f, CardInk(0.055f * wash * opacity));
            }
            fillRect(iconBox, island::kStartAppIconRadiusDip, CardInk(0.04f * opacity));
            // The DASHED outline of the reference's empty slot. The stroke style is created once here; if it
            // cannot be made the slot keeps a solid hairline rather than disappearing.
            ID2D1StrokeStyle* dashed = nullptr;
            if (d2dFactory) {
                const D2D1_STROKE_STYLE_PROPERTIES properties = D2D1::StrokeStyleProperties(
                    D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                    D2D1_LINE_JOIN_ROUND, 1.0f, D2D1_DASH_STYLE_DASH, 0.0f);
                d2dFactory->CreateStrokeStyle(properties, nullptr, 0, &dashed);
            }
            strokeRect(iconBox, island::kStartAppIconRadiusDip,
                       CardInk((0.22f + 0.10f * wash) * opacity), 1.2f, dashed);
            if (dashed) dashed->Release();
            if (ID2D1SolidColorBrush* plus =
                    BrushFor(context, CardInk((0.58f + 0.22f * wash) * opacity))) {
                const float centreX = (iconBox.left + iconBox.right) * 0.5f;
                const float centreY = (iconBox.top + iconBox.bottom) * 0.5f;
                constexpr float kArm = 9.0f;
                context->DrawLine(D2D1::Point2F(centreX - kArm, centreY),
                                  D2D1::Point2F(centreX + kArm, centreY), plus, 1.8f);
                context->DrawLine(D2D1::Point2F(centreX, centreY - kArm),
                                  D2D1::Point2F(centreX, centreY + kArm), plus, 1.8f);
            }
            drawText(startLabelFormat_, L"\u6DFB\u52A0\u5FEB\u6377\u952E", labelBox,
                     CardInk(0.58f * opacity), DWRITE_TEXT_ALIGNMENT_CENTER,
                     DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
    }

    // --- the status foot: a quiet anchor beneath the collection --------------------------------
    if (layout.statusRule.right > layout.statusRule.left) {
        fillRect(layout.statusRule, 0.5f, CardInk(0.065f * opacity));
    }
    {
        const float centreY = (layout.status.top + layout.status.bottom) * 0.5f;
        if (ID2D1SolidColorBrush* dot =
                BrushFor(context, D2D1::ColorF(0.20f, 0.76f, 0.45f, 0.94f * opacity))) {
            context->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(layout.status.left + 4.0f, centreY), 3.0f, 3.0f),
                dot);
        }
        drawText(startCaptionFormat_, L"\u7CFB\u7EDF\u8FD0\u884C\u6B63\u5E38",
                 {layout.status.left + 14.0f, layout.status.top,
                  layout.status.left + 118.0f, layout.status.bottom},
                 CardInk(0.52f * opacity), DWRITE_TEXT_ALIGNMENT_LEADING,
                 DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        if (ID2D1SolidColorBrush* divider = BrushFor(context, CardInk(0.18f * opacity))) {
            context->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(layout.status.left + 132.0f, centreY), 1.4f, 1.4f),
                divider);
        }
        wchar_t detail[48] = {};
        std::swprintf(detail, 48, text.page == 1 ? L"\u5DF2\u52A0\u8F7D %d \u4E2A\u5E94\u7528"
                                                : L"%d \u4E2A\u56FA\u5B9A\u5E38\u7528",
                      text.page == 1 ? text.listCount : text.pinCount);
        drawText(startCaptionFormat_, detail,
                 {layout.status.left + 146.0f, layout.status.top,
                  layout.status.right - 8.0f, layout.status.bottom},
                 CardInk(0.38f * opacity), DWRITE_TEXT_ALIGNMENT_LEADING,
                 DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    if (d2dFactory) d2dFactory->Release();
}

} // namespace island

