// =====================================================================
//  ChecksCards.cpp -- see selftest/Checks.h for why the self-test is split like this
//
//  The includes are the ones the single file carried; pruning them per file is a smaller,
//  separate exercise and is deliberately not mixed into the move.
// =====================================================================
// =====================================================================
//  Diagnostics.cpp
//  The runtime self-test and the native-taskbar probe.
//
//  Every assertion below is deliberately phrased as a question about what the
//  system actually did, not about what was requested. The interesting failures
//  in this area of Windows are the silent ones -- a per-monitor-v2 awareness
//  that never took effect, a z-order that quietly dropped out of topmost, a
//  swap chain that presented black outside the pill -- and none of them raise
//  an HRESULT.
//
//  The probe's report goes to the log file as well as to stdout, because
//  stdout has been lost on this machine three separate times.
// =====================================================================
#include "selftest/Diagnostics.h"

#include <windows.h>
#include <shellapi.h>   // APPBARDATA / ABM_* for the native-taskbar probe

#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>   // strtoul: unpacking the hex of a captured registration in the tray checks
#include <vector>
#include <cwchar>
#include <string>

#include "domain/Geometry.h"
#include "core/Log.h"
#include "domain/MiniJson.h"
#include "domain/TrayRegistration.h"
#include "domain/TrayLedger.h"     // which registrations the shell is holding (see the tray checks)
#include "platform/TrayRing.h"     // the channel the module publishes registrations on
#include "domain/CardLayout.h"
#include "domain/StatePolicy.h"
#include "domain/TrayEncoding.h"   // the only place a tray click is encoded (see its header)
#include "media/Lyrics.h"
#include "app/AppBar.h"
#include "domain/TaskStripLayout.h"
#include "platform/WindowList.h"
#include "media/MusicSession.h"
#include "app/PlayerWindow.h"
#include "media/WicImage.h"
#include "services/Notification.h"
#include "services/NotificationIcon.h"
#include "services/ToastStore.h"
#include "services/WeChatWatch.h"
#include "shell/Weather.h"
#include "platform/ThumbnailPreview.h"
#include "app/IslandWindow.h"
#include "platform/Monitor.h"
#include "platform/NativeTaskbar.h"   // the three answers to "what happens to the shell's taskbar"
#include "shell/SystemToggles.h"
#include "render/Theme.h"
#include "render/GlassMaterial.h"
#include "shell/Weather.h"   // SkyMarkForCode: the word and the mark come from the same WMO code
#include "render/Ambient.h"  // AmbientKindFor: the island's weather and the real sunset
#include "domain/Motion.h"
#include "render/Renderer.h"
#include "render/TextRenderer.h"
#include "shell/TrayIcons.h"
#include "shell/TrayPress.h"
#include "shell/TrayLearn.h"   // CountCoveredIcons: how much of the tray a press can reach (see the tray checks)

#include "selftest/Checks.h"
#include "selftest/Harness.h"

namespace island {

void TestCardLayout() {
    Section(L"card control layout");

    const IslandAnchor anchor = AnchorForWindow(0.0f, 0.0f, 2560.0f);
    const RoundedBox card = BoxForState(IslandState::Expanded, anchor);
    const CardContentLayout layout = CardContentFor(card);

    Check(card.rect.left <= layout.content.left && layout.content.right <= card.rect.right,
          L"the content band stays inside the card");
    Check(layout.content.top > card.rect.top + (card.rect.bottom - card.rect.top) * 0.4f,
          L"content starts below the clock's half of the card");
    Check(layout.content.bottom < card.rect.bottom,
          L"content ends above the card's bottom edge");

    // The three parts must not overlap, or a click could be attributed to two of
    // them and the drawing would collide.
    Check(layout.artwork.right < layout.text.left, L"artwork ends before the text begins");
    Check(layout.text.right < layout.playPause.left,
          L"text ends before the transport button begins");
    Check(layout.playPause.right <= layout.content.right + 0.01f,
          L"the transport button stays inside the content band");
    Check(layout.progress.top >= layout.content.bottom - 0.01f,
          L"the progress bar sits below the content, not over it");

    // The transport button has to be big enough to hit. A 28 DIP square is a
    // deliberate choice; a regression to something tiny would still "work".
    const float buttonWidth = layout.playPause.right - layout.playPause.left;
    const float buttonHeight = layout.playPause.bottom - layout.playPause.top;
    Check(Nearly(buttonWidth, kTransportButtonDip, 0.01f) &&
          Nearly(buttonHeight, kTransportButtonDip, 0.01f),
          L"the transport button is the size it claims");
    Check(buttonWidth >= 24.0f, L"the transport button is a usable click target");

    // And the hit test must agree with the drawing, at the centre and just outside.
    const float buttonCentreX = (layout.playPause.left + layout.playPause.right) * 0.5f;
    const float buttonCentreY = (layout.playPause.top + layout.playPause.bottom) * 0.5f;
    Check(HitsTransportButton(card, buttonCentreX, buttonCentreY),
          L"a click at the button's centre hits it");
    // One pixel inside the play/pause button's own left edge, not "one button width
    // to the left of its centre" -- the latter lands on the previous button, which is
    // a hit, and an earlier version of this check asserted the opposite and failed.
    Check(!HitsTransportButton(card, layout.playPause.left - 1.0f, buttonCentreY),
          L"a click just left of play/pause does not hit play/pause");
    Check(!HitsTransportButton(card, buttonCentreX, layout.playPause.top - 1.0f),
          L"a click just above play/pause does not hit it");
    Check(!HitsTransportButton(card, layout.artwork.left + 2.0f, layout.artwork.top + 2.0f),
          L"a click on the artwork does not hit a button");

    // Each of the three buttons must be reachable and distinct.
    {
        const auto centreOf = [](const RectF& r) {
            return D2D1::Point2F((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f);
        };
        const D2D1_POINT_2F previous = centreOf(layout.previous);
        const D2D1_POINT_2F playPause = centreOf(layout.playPause);
        const D2D1_POINT_2F next = centreOf(layout.next);

        Check(ControlAt(card, previous.x, previous.y) == CardControl::Previous,
              L"the leftmost button is previous");
        Check(ControlAt(card, playPause.x, playPause.y) == CardControl::PlayPause,
              L"the middle button is play/pause");
        Check(ControlAt(card, next.x, next.y) == CardControl::Next,
              L"the rightmost button is next");
        Check(previous.x < playPause.x && playPause.x < next.x,
              L"the buttons are laid out in reading order");
        // A point between two buttons must belong to neither, or a click in the gap
        // would toggle something.
        Check(ControlAt(card, (layout.previous.right + layout.playPause.left) * 0.5f,
                        playPause.y) == CardControl::None,
              L"the gap between buttons belongs to no button");
    }
}

void TestImageCodec() {
    Section(L"image codec (album art)");

    // Build a known image through the encoder, then decode it back. This exercises
    // both directions and proves they agree on channel order and orientation.
    constexpr int kSize = 16;
    uint8_t source[kSize * kSize * 4] = {};
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const size_t i = (static_cast<size_t>(y) * kSize + x) * 4;
            // BGRA. The top-left quadrant is strongly blue and the rest dark, so a
            // flip or a channel swap changes the sampled value.
            const bool corner = (x < kSize / 2 && y < kSize / 2);
            source[i + 0] = corner ? 240 : 10;
            source[i + 1] = 10;
            source[i + 2] = corner ? 10 : 240;
            source[i + 3] = 255;
        }
    }

    static uint8_t encoded[kMusicArtworkCapacity] = {};
    const size_t encodedSize = EncodeBgraToPng(source, kSize, kSize,
                                               encoded, sizeof(encoded));
    DecodedImage decoded;
    Check(DecodeImage(encoded, encodedSize, &decoded), L"that PNG decodes back");
    Check(decoded.width == kSize && decoded.height == kSize,
          L"the decoded size matches what was encoded");

    if (decoded.pixels && decoded.width == kSize && decoded.height == kSize) {
        // Sample the two quadrants. BGRA order, so [0] is blue and [2] is red.
        const size_t topLeft = (static_cast<size_t>(2) * kSize + 2) * 4;
        const size_t bottomRight = (static_cast<size_t>(kSize - 3) * kSize + (kSize - 3)) * 4;
        wchar_t detail[192];
        std::swprintf(detail, 192, L"topleft B=%u R=%u; bottomright B=%u R=%u",
                      decoded.pixels[topLeft + 0], decoded.pixels[topLeft + 2],
                      decoded.pixels[bottomRight + 0], decoded.pixels[bottomRight + 2]);
        Check(decoded.pixels[topLeft + 0] > decoded.pixels[topLeft + 2] &&
              decoded.pixels[bottomRight + 2] > decoded.pixels[bottomRight + 0],
              L"channels and orientation survive the round trip", detail);
    }

    // --- TWO DECODED IMAGES ALIVE AT ONCE (2026-10-13, HANDOFF 8.77) --------------------
    //
    // The decoder keeps one buffer PER SLOT, and this is the reason: the music path has three images alive
    // in the same frame -- the cover, the blurred background built from it, and the source app's icon --
    // and while they shared one buffer the LAST decode overwrote the first. The card then drew its cover
    // out of the blurred background's bytes; the blur is smaller than the cover, so only the cover's first
    // rows were overwritten and the top half came out smeared over artwork that still looked right.
    // Reported in those words: "歌曲封面这个上半部分会涂抹".
    //
    // The second decode here is deliberately SMALLER, and the sample is taken at the start of the first
    // image's buffer -- the bytes a shared buffer would have overwritten. A test that sampled the middle
    // would pass with the bug still in place.
    {
        constexpr int kBig = 8;
        static uint8_t big[kBig * kBig * 4] = {};
        for (size_t i = 0; i < sizeof(big); i += 4) {
            big[i + 0] = 20;    // B
            big[i + 1] = 20;    // G
            big[i + 2] = 240;   // R
            big[i + 3] = 255;
        }
        static uint8_t bigPng[kMusicArtworkCapacity] = {};
        const size_t bigSize = EncodeBgraToPng(big, kBig, kBig, bigPng, sizeof(bigPng));

        constexpr int kSmall = 6;
        static uint8_t smallArt[kSmall * kSmall * 4] = {};
        for (size_t i = 0; i < sizeof(smallArt); i += 4) {
            smallArt[i + 0] = 240;   // B
            smallArt[i + 1] = 20;
            smallArt[i + 2] = 20;    // R
            smallArt[i + 3] = 255;
        }
        static uint8_t smallArtPng[kMusicArtworkCapacity] = {};
        const size_t smallArtSize = EncodeBgraToPng(smallArt, kSmall, kSmall, smallArtPng, sizeof(smallArtPng));

        DecodedImage bigDecoded;
        DecodedImage smallArtDecoded;
        const bool bigOk =
            bigSize > 0 && DecodeImage(bigPng, bigSize, &bigDecoded, DecodeSlot::Artwork);
        const bool smallOk =
            smallArtSize > 0 && DecodeImage(smallArtPng, smallArtSize, &smallArtDecoded, DecodeSlot::Other);
        const size_t mid = (static_cast<size_t>(kBig / 2) * kBig + kBig / 2) * 4;
        wchar_t twoDetail[176];
        std::swprintf(twoDetail, 176, L"big B=%u R=%u (also mid B=%u R=%u), small B=%u R=%u",
                      bigDecoded.pixels ? bigDecoded.pixels[0] : 0,
                      bigDecoded.pixels ? bigDecoded.pixels[2] : 0,
                      bigDecoded.pixels ? bigDecoded.pixels[mid + 0] : 0,
                      bigDecoded.pixels ? bigDecoded.pixels[mid + 2] : 0,
                      smallArtDecoded.pixels ? smallArtDecoded.pixels[0] : 0,
                      smallArtDecoded.pixels ? smallArtDecoded.pixels[2] : 0);
        Check(bigOk && smallOk && bigDecoded.pixels && smallArtDecoded.pixels &&
                  bigDecoded.pixels[2] > bigDecoded.pixels[0] &&
                  bigDecoded.pixels[mid + 2] > bigDecoded.pixels[mid + 0] &&
                  smallArtDecoded.pixels[0] > smallArtDecoded.pixels[2],
              L"a decode in one slot leaves a decode in another alone", twoDetail);
    }

    // Downscaling must preserve the aspect ratio, or a non-square cover would be
    // squashed on the card.
    {
        constexpr int kWide = 64;
        constexpr int kShort = 32;
        static uint8_t wide[kWide * kShort * 4] = {};
        for (size_t i = 0; i < sizeof(wide); i += 4) {
            wide[i + 0] = 100;
            wide[i + 1] = 120;
            wide[i + 2] = 140;
            wide[i + 3] = 255;
        }
        static uint8_t widePng[kMusicArtworkCapacity] = {};
        const size_t wideSize = EncodeBgraToPng(wide, kWide, kShort, widePng, sizeof(widePng));
        Check(wideSize > 0, L"a non-square image encodes");

        static uint8_t shrunk[kMusicArtworkCapacity] = {};
        const size_t shrunkSize = ReencodeToPng(widePng, wideSize, 32, shrunk, sizeof(shrunk));
        Check(shrunkSize > 0, L"a non-square image re-encodes");

        DecodedImage scaled;
        if (shrunkSize > 0 && DecodeImage(shrunk, shrunkSize, &scaled)) {
            const double ratio = static_cast<double>(scaled.width) /
                                 static_cast<double>(scaled.height ? scaled.height : 1);
            wchar_t detail[160];
            std::swprintf(detail, 160, L"%dx%d from %dx%d, ratio %.2f",
                          scaled.width, scaled.height, kWide, kShort, ratio);
            Check(scaled.width > scaled.height &&
                  ratio > 1.9 && ratio < 2.1,
                  L"scaling preserves the aspect ratio", detail);
        } else {
            Check(false, L"the scaled image decodes");
        }
    }

    // Malformed input must be refused, not crash. Each of these has been a real
    // crash in some image pipeline at some point.
    const uint8_t garbage[64] = {0x00, 0x01, 0x02, 0x03};
    Check(!DecodeImage(garbage, sizeof(garbage), &decoded), L"garbage is refused");
    Check(!DecodeImage(encoded, 8, &decoded), L"a truncated PNG header is refused");
    Check(!DecodeImage(nullptr, 0, &decoded), L"an empty buffer is refused");
    Check(!DecodeImage(encoded, 0, &decoded), L"a zero length is refused");

    // The music card's words choose black or white from the blurred cover's average luminance. Half
    // black and half white must read 0.5; an empty buffer has no luminance at all.
    {
        const uint8_t halfCover[2 * 2 * 4] = {0,   0,   0,   255, 255, 255, 255, 255,
                                              0,   0,   0,   255, 255, 255, 255, 255};
        const float luma = island::AverageLuminance(halfCover, 2, 2);
        wchar_t detail[128] = {};
        std::swprintf(detail, 128, L"half black/white -> %.3f", luma);
        Check(std::abs(luma - 0.5f) < 0.01f,
              L"a half black, half white cover reads 0.5", detail);
        Check(island::AverageLuminance(nullptr, 0, 0) == 0.0f,
              L"an empty background has no luminance", detail);
    }

    // A body-truncated PNG is deliberately NOT asserted to fail, and the reason is
    // worth recording.
    //
    // WIC decodes the partial image whenever it has enough of the stream to make
    // sense of, which is reasonable and not worth fighting. An earlier version of
    // this check demanded failure and it failed -- against a decoder that was doing
    // the right thing. What IS asserted is the invariant that actually matters: a
    // truncated buffer must never decode to something LARGER than the original,
    // which is what a decoder inventing rows from uninitialised padding would give.
    {
        DecodedImage partial;
        if (DecodeImage(encoded, encodedSize / 2, &partial)) {
            wchar_t detail[160];
            std::swprintf(detail, 160, L"partial decoded %dx%d, original %dx%d",
                          partial.width, partial.height, kSize, kSize);
            Check(partial.width <= kSize && partial.height <= kSize,
                  L"a truncated PNG cannot decode larger than the original", detail);
        } else {
            Check(true, L"a truncated PNG is refused outright");
        }
        Check(!DecodeImage(encoded, 4, &partial), L"four bytes is not an image");
    }

    // Re-encoding invalid input must also fail rather than emit something.
    static uint8_t scratch[kMusicArtworkCapacity] = {};
    Check(ReencodeToPng(garbage, sizeof(garbage), 32, scratch, sizeof(scratch)) == 0,
          L"re-encoding garbage fails");
    Check(EncodeBgraToPng(nullptr, 8, 8, scratch, sizeof(scratch)) == 0,
          L"encoding null pixels fails");
    Check(EncodeBgraToPng(source, 0, 0, scratch, sizeof(scratch)) == 0,
          L"encoding an empty size fails");

    // A buffer too small for the result must fail rather than overflow.
    uint8_t tiny[16] = {};
    Check(EncodeBgraToPng(source, kSize, kSize, tiny, sizeof(tiny)) == 0,
          L"encoding into a buffer that is too small fails");

    // The card lays text out in hand-sized rectangles. A rectangle shorter than the
    // face's natural line height clips the glyphs, which is exactly what happened on
    // screen: the track title drew as a horizontal sliver because its rectangle was
    // 20 DIP while the CJK face needs more. These assertions make the layout answer
    // to the real metrics instead of to a guess.
    {
        TextRenderer metricsSource;
        if (metricsSource.Initialize()) {
            const TextRenderer::TextMetrics metrics = metricsSource.Metrics();

            wchar_t detail[192];
            std::swprintf(detail, 192, L"title %.1f DIP, secondary %.1f DIP",
                          metrics.titleLineHeight, metrics.secondaryLineHeight);
            Info(L"measured line heights", detail);

            // The layout constants in TextRenderer must not be shorter than these.
            // They are asserted rather than hardcoded into the check, so a future font
            // or locale change fails here instead of on screen.
            Check(metrics.titleLineHeight > 0.0f && metrics.secondaryLineHeight > 0.0f,
                  L"DirectWrite reports usable line heights", detail);
            Check(metrics.titleLineHeight <= 22.0f,
                  L"the title line height fits the space the card reserves", detail);
            Check(metrics.secondaryLineHeight <= 20.0f,
                  L"the secondary line height fits its space", detail);

            metricsSource.Shutdown();
        } else {
            Check(false, L"DirectWrite formats for the metrics check");
        }
    }
}

void TestAccentColour() {
    Section(L"album accent colour");

    // A solid red cover. BGRA, premultiplied, which is what the decoder hands over.
    {
        uint8_t red[16 * 16 * 4] = {};
        for (size_t i = 0; i < sizeof(red); i += 4) {
            red[i + 0] = 0;      // B
            red[i + 1] = 0;      // G
            red[i + 2] = 255;    // R
            red[i + 3] = 255;    // A
        }
        const AccentColour accent = AccentColourFromArtwork(red, 16, 16);
        wchar_t detail[160];
        std::swprintf(detail, 160, L"red cover -> %.2f %.2f %.2f (from artwork %d)", accent.r, accent.g,
                      accent.b, accent.fromArtwork ? 1 : 0);
        Check(accent.fromArtwork && accent.r > accent.g + 0.2f && accent.r > accent.b + 0.2f,
              L"a red cover gives a red accent", detail);

        // ... and it is BRIGHT enough to be seen on the pill's near-black: the whole point of the lift.
        Check(accent.r > 0.7f, L"the accent is lifted to a visible value", detail);
    }

    // A grey cover has no colour to take, so the default stands and says so.
    {
        uint8_t grey[16 * 16 * 4] = {};
        for (size_t i = 0; i < sizeof(grey); i += 4) {
            grey[i + 0] = 128;
            grey[i + 1] = 128;
            grey[i + 2] = 128;
            grey[i + 3] = 255;
        }
        const AccentColour accent = AccentColourFromArtwork(grey, 16, 16);
        Check(!accent.fromArtwork, L"a cover with no colour keeps the default accent");
        Check(accent.b > accent.r, L"... and the default is the cool one the bar was built with");
    }

    // A dark, saturated cover: dark enough that an un-lifted average would be invisible.
    {
        uint8_t dark[16 * 16 * 4] = {};
        for (size_t i = 0; i < sizeof(dark); i += 4) {
            dark[i + 0] = 60;    // B -- dark, but unmistakably coloured
            dark[i + 1] = 10;    // G
            dark[i + 2] = 10;    // R
            dark[i + 3] = 255;
        }
        const AccentColour accent = AccentColourFromArtwork(dark, 16, 16);
        wchar_t detail[160];
        std::swprintf(detail, 160, L"dark blue cover -> %.2f %.2f %.2f", accent.r, accent.g, accent.b);
        Check(accent.fromArtwork && accent.b > 0.7f && accent.b > accent.r,
              L"a dark cover still yields a visible accent", detail);
    }

    // Degenerate input must not invent a colour, and must not crash.
    {
        Check(!AccentColourFromArtwork(nullptr, 16, 16).fromArtwork,
              L"no pixels means no accent from artwork");
        uint8_t one[4] = {255, 255, 255, 255};
        Check(!AccentColourFromArtwork(one, 0, 0).fromArtwork,
              L"an empty size means no accent from artwork");
    }
}

void TestLyrics() {
    Section(L"lyrics (LRC parse and line lookup)");

    // One instance, reused by every case below.
    //
    // Lyrics is ~132 KB (512 lines x 260 bytes), and each `Lyrics lyrics;` here used
    // to put another one on the stack. With a dozen cases that overflowed it and the
    // process died on the /GS check with no output at all, which reads like a hang
    // rather than a crash. ParseLrc clears its output, so one instance is safe to
    // reuse between cases.
    static Lyrics sharedLyrics;
    static Lyrics emptyLyrics;

    // The ordinary form.
    {
        Lyrics& lyrics = sharedLyrics;
        const int count = ParseLrc("[00:12.34]first line\n[00:15.00]second line\n", &lyrics);
        Check(count == 2, L"parses two plain lines");
        Check(lyrics.count == 2, L"the count matches the lines stored");
        Check(lyrics.lines[0].timeMs == 12340, L"centiseconds become milliseconds");
        Check(std::strcmp(lyrics.lines[0].text, "first line") == 0,
              L"the first line's text is stored");
        Check(std::strcmp(lyrics.lines[1].text, "second line") == 0,
              L"the second line's text is stored");
    }

    // Three fraction digits, and the colon variant some editors emit.
    {
        Lyrics& lyrics = sharedLyrics;
        ParseLrc("[00:01.500]ms\n[00:02:75]colon\n[00:03]plain\n", &lyrics);
        Check(lyrics.count == 3, L"parses all three time formats");
        Check(lyrics.lines[0].timeMs == 1500, L"three digits are milliseconds");
        Check(lyrics.lines[1].timeMs == 2750, L"a colon fraction is centiseconds");
        Check(lyrics.lines[2].timeMs == 3000, L"a missing fraction is zero");
    }

    // Minutes beyond 59, which long tracks and audiobooks have.
    {
        Lyrics& lyrics = sharedLyrics;
        ParseLrc("[123:45.67]long\n", &lyrics);
        Check(lyrics.count == 1 && lyrics.lines[0].timeMs == 123 * 60000 + 45670,
              L"minutes are not limited to 59");
    }

    // Metadata tags.
    {
        Lyrics& lyrics = sharedLyrics;
        ParseLrc("[ti:Track Name]\n[ar:Some Artist]\n[al:An Album]\n[00:01.00]words\n",
                 &lyrics);
        Check(std::strcmp(lyrics.title, "Track Name") == 0, L"reads the [ti:] tag");
        Check(std::strcmp(lyrics.artist, "Some Artist") == 0, L"reads the [ar:] tag");
        Check(lyrics.count == 1, L"metadata lines are not lyric lines");
    }

    // Several tags on one line mean the same text at each time.
    {
        Lyrics& lyrics = sharedLyrics;
        ParseLrc("[00:10.00][00:20.00]repeated\n", &lyrics);
        Check(lyrics.count == 2, L"a repeated line becomes two entries");
        Check(lyrics.lines[0].timeMs == 10000 && lyrics.lines[1].timeMs == 20000,
              L"both times are kept");
        Check(std::strcmp(lyrics.lines[0].text, "repeated") == 0 &&
              std::strcmp(lyrics.lines[1].text, "repeated") == 0,
              L"both entries carry the text");
    }

    // Out-of-order input must come back sorted; the line lookup depends on it.
    {
        Lyrics& lyrics = sharedLyrics;
        ParseLrc("[00:30.00]third\n[00:10.00]first\n[00:20.00]second\n", &lyrics);
        Check(lyrics.count == 3, L"parses unsorted input");
        Check(lyrics.lines[0].timeMs == 10000 && lyrics.lines[1].timeMs == 20000 &&
              lyrics.lines[2].timeMs == 30000,
              L"the result is sorted by time");
    }

    // An empty lyric line is meaningful: it clears the display during an interlude.
    {
        Lyrics& lyrics = sharedLyrics;
        ParseLrc("[00:01.00]words\n[00:05.00]\n[00:09.00]more\n", &lyrics);
        Check(lyrics.count == 3, L"a timed empty line is kept");
        Check(lyrics.lines[1].text[0] == '\0', L"its text is empty");
    }

    // CRLF, a BOM, and trailing spaces.
    {
        Lyrics& lyrics = sharedLyrics;
        const char* withBom = "\xEF\xBB\xBF[00:01.00]windows line  \r\n[00:02.00]next\r\n";
        const int count = ParseLrc(withBom, &lyrics);
        Check(count == 2, L"handles a BOM and CRLF");
        Check(std::strcmp(lyrics.lines[0].text, "windows line") == 0,
              L"trailing spaces and CR are trimmed");
    }
    {
        Lyrics& lyrics = sharedLyrics;
        ParseLrc("Artist Name - Song\n[00:01.00]real line\n", &lyrics);
        Check(lyrics.count == 1, L"text before the first tag is ignored");
    }

    // Degenerate input must not crash or invent lines.
    {
        Lyrics& lyrics = sharedLyrics;
        Check(ParseLrc("", &lyrics) == 0, L"an empty document has no lines");
        Check(ParseLrc(nullptr, &lyrics) == 0, L"a null document has no lines");
        Check(ParseLrc("[00:01.00", &lyrics) == 0, L"an unterminated tag is ignored");
        Check(ParseLrc("[aa:bb]x\n", &lyrics) == 0, L"a non-numeric tag is ignored");
        Check(ParseLrc("just some prose\nwith no tags\n", &lyrics) == 0,
              L"prose with no tags yields nothing");
        Check(ParseLrc("[00]x\n", &lyrics) == 0, L"a minutes-only tag is not a time");
        Check(ParseLrc("[99:99.99]", &lyrics) == 1, L"an out-of-range time still parses");
        Check(lyrics.lines[0].text[0] == '\0', L"and has empty text");
    }

    // --- line lookup ---
    {
        Lyrics& lyrics = sharedLyrics;
        ParseLrc("[00:10.00]first\n[00:20.00]second\n[00:30.00]third\n", &lyrics);

        // Before the first line there is nothing to show: an intro would otherwise
        // display the first lyric for its whole duration.
        Check(lyrics.LineAt(0) == -1, L"before the first line, nothing is shown");
        Check(lyrics.LineAt(9999) == -1, L"just before the first line, still nothing");
        Check(lyrics.LineAt(10000) == 0, L"exactly on the first line, it is shown");
        Check(lyrics.LineAt(15000) == 0, L"between lines, the earlier one stays");
        Check(lyrics.LineAt(20000) == 1, L"exactly on the second line, it is shown");
        Check(lyrics.LineAt(100000) == 2, L"past the end, the last line stays");

        Check(lyrics.At(-1) == nullptr, L"a negative index has no line");
        Check(lyrics.At(3) == nullptr, L"an index past the end has no line");
        Check(lyrics.At(1) != nullptr, L"a valid index has a line");
        if (lyrics.At(1)) {
            Check(std::strcmp(lyrics.At(1)->text, "second") == 0,
                  L"the looked-up line is the right one");
        }

        // --- AN EMPTY LINE IS NOT AN ANSWER (2026-10-13, HANDOFF 8.76) ---------------------
        //
        // Reported: "如果一句唱的慢的话 会显示歌曲名 然后唱下一句的时候 歌词才能对上". NetEase's LRCs mark
        // the end of a phrase with a time tag and no text, and they are everywhere -- measured on 满江
        // 《归来》: 37 of its 88 lines, one of them 1.4 s after "我的心" began. Taking the marker as the
        // current line made that line EMPTY, and an empty line is what the frame loop reads as "no lyric"
        // and answers with the SONG TITLE. The words that were just sung stay instead.
        {
            ParseLrc("[00:20.94]我的心\n[00:22.33]\n[00:26.81]\n[00:30.00]下一句\n", &lyrics);
            wchar_t markerDetail[192];
            std::swprintf(markerDetail, 192,
                          L"lines=%d, at 25 s -> %d, at 30 s -> %d, before the words -> %d",
                          lyrics.count, lyrics.LineAt(25000), lyrics.LineAt(30000),
                          lyrics.LineAt(20000));
            Check(lyrics.count == 4 && lyrics.LineAt(25000) == 0 && lyrics.LineAt(30000) == 3 &&
                      lyrics.LineAt(20000) == -1,
                  L"an empty LRC line does not blank the pill: the words stay until the next ones",
                  markerDetail);

            // A marker BEFORE any words is the intro: the title is the honest thing to show.
            ParseLrc("[00:01.00]\n[00:05.00]words\n", &lyrics);
            Check(lyrics.LineAt(2000) == -1 && lyrics.LineAt(6000) == 1,
                  L"a marker before the first words leaves nothing to show, as the intro does");

            // Nothing but markers: still nothing to show, however far in.
            ParseLrc("[00:01.00]\n[00:05.00]\n", &lyrics);
            Check(lyrics.LineAt(9000) == -1,
                  L"a document of nothing but markers shows nothing at all");
        }

        Lyrics& empty = emptyLyrics;
        Check(empty.LineAt(0) == -1 && empty.Empty(), L"an empty document shows nothing");
    }

    // Two lines at one time: the later wins, which is what a file carrying both the
    // original and a translation relies on.
    {
        Lyrics& lyrics = sharedLyrics;
        ParseLrc("[00:05.00]original\n[00:05.00]translation\n", &lyrics);
        Check(lyrics.count == 2, L"two lines at one time are both kept");
        Check(lyrics.LineAt(5000) == 1, L"the later of two equal times wins");
    }

    // --- query encoding ---
    //
    // This is the test that would have caught a real bug: the search query was built from
    // `argv`, which on Windows is in the process ANSI code page, so a Chinese title was
    // percent-encoded as GBK (`%B9%A9%CF%FA` for 婵炴挻纰嶉惄顖炲绩? where the services expect UTF-8
    // (`%E4%BE%9B`). They answered with nothing, which looks exactly like a track that has
    // no lyrics. Asserting the exact bytes is the only way to pin that down.
    {
        wchar_t encoded[512] = {};

        // 婵?is U+4F9B -> UTF-8 E4 BE 9B.
        UrlEncode("\xe4\xbe\x9b", encoded, 512);
        Check(std::wcscmp(encoded, L"%E4%BE%9B") == 0,
              L"a Chinese character is percent-encoded as UTF-8");

        // A whole title, so a wrong code page cannot pass by luck on one character.
        UrlEncode("\xe4\xbe\x9b\xe9\x94\x80\xe7\xa4\xbe", encoded, 512);
        Check(std::wcscmp(encoded, L"%E4%BE%9B%E9%94%80%E7%A4%BE") == 0,
              L"a Chinese title encodes to the UTF-8 form the services expect");

        // Anything unreserved must pass through, or a search would be over-escaped.
        UrlEncode("abcXYZ0189-_.~", encoded, 512);
        Check(std::wcscmp(encoded, L"abcXYZ0189-_.~") == 0,
              L"unreserved characters are left alone");

        // Everything else must be escaped, including the space between a title and an
        // artist and the characters that would otherwise break the query.
        UrlEncode("a b&c=d", encoded, 512);
        Check(std::wcscmp(encoded, L"a%20b%26c%3Dd") == 0,
              L"reserved characters are escaped");

        // A buffer that cannot hold the result must truncate, not overrun.
        wchar_t tinyBuffer[5] = {};
        UrlEncode("\xe4\xbe\x9b\xe9\x94\x80", tinyBuffer, 5);
        Check(tinyBuffer[4] == L'\0', L"a short buffer is still terminated");
    }

    // --- the two services' response shapes ---
    //
    // Both are exercised on samples of the shape each service returns, because the parsers
    // are the part that can be checked offline: whether the services answer at all is a
    // network question, and in this development sandbox they never do.
    {
        // NetEase nests the LRC inside an object, so the flat reader cannot reach it.
        const char* netease =
            "{\"lrc\":{\"version\":1,\"lyric\":\"[00:01.00]first\\n[00:02.00]second\"},"
            "\"tlyric\":{\"lyric\":\"translation\"}}";
        char extracted[512] = {};
        Check(ExtractLyricField(netease, extracted, sizeof(extracted)),
              L"the nested netease lyric field is found");
        Check(std::strstr(extracted, "[00:01.00]first") != nullptr,
              L"its escapes are decoded");
        // It must not pick up the translation, which repeats the same key name.
        Check(std::strstr(extracted, "translation") == nullptr,
              L"the translation field is not mistaken for the lyrics");

        Lyrics& parsed = sharedLyrics;
        Check(ParseLrc(extracted, &parsed) == 2, L"the extracted text parses as LRC");

        // `[offset:N]` shifts the WHOLE timeline, and it was ignored -- so a file carrying one was off
        // by that much from beginning to end, which is exactly "歌词应该和当前唱的一样才对" when the
        // offset is a second or more. It may sit before or after the lines, so it is applied last.
        {
            Lyrics shifted;
            Check(ParseLrc("[offset:500]\n[00:10.00]late\n[00:20.00]later\n", &shifted) == 2 &&
                      shifted.lines[0].timeMs == 10500 && shifted.lines[1].timeMs == 20500,
                  L"a positive LRC offset shifts every line later");
            Lyrics earlier;
            Check(ParseLrc("[00:10.00]x\n[offset:-2000]\n[00:20.00]y\n", &earlier) == 2 &&
                      earlier.lines[0].timeMs == 8000 && earlier.lines[1].timeMs == 18000,
                  L"a negative offset shifts every line earlier, wherever the tag sits");
            Lyrics clamped;
            Check(ParseLrc("[offset:-30000]\n[00:10.00]x\n", &clamped) == 1 &&
                      clamped.lines[0].timeMs == 0,
                  L"a shift past the start clamps at zero rather than going negative");
        }

        // Which field answered, so a caller can say so in its log.
        const char* source = nullptr;
        char again[512] = {};
        Check(ExtractLyricField(netease, again, sizeof(again), &source) && source &&
                  std::strcmp(source, "lrc") == 0,
              L"the original is preferred, and named as the source");

        // --- the original empty, the TRANSLATION carrying the timed text ----------------
        //
        // Not a synthetic corner: this is what this machine really receives for
        // `Maple Road（你的名字）`, captured with `--lyrics-test <title> <artist> --lyrics-verbose`:
        //
        //   {"lrc":{"version":1,"lyric":""},"klyric":{"version":0,"lyric":""},
        //    "tlyric":{"version":2,"lyric":"[00:15.243]...\n[00:17.452]..."}}
        //
        // Reading only `lrc` produced "the response had no lyric field", which the provider's retry
        // rule treats as TRANSIENT -- so the bar asked both services twice more for that track and
        // never showed a lyric it had already received. Reported, after lyrics that never appeared,
        // as "现在音乐歌词 是怎么获取的".
        const char* translationOnly =
            R"({"sgc":false,"lrc":{"version":1,"lyric":""},"klyric":{"version":0,"lyric":""},)"
            R"("tlyric":{"version":2,"lyric":"[00:15.24]first line\n[00:17.45]second line"}})";
        char fromTranslation[512] = {};
        const char* translationSource = nullptr;
        const bool gotTranslation = ExtractLyricField(translationOnly, fromTranslation,
                                                     sizeof(fromTranslation), &translationSource);
        wchar_t lyricDetail[256];
        std::swprintf(lyricDetail, 256, L"source='%hs' text='%.60hs'",
                      translationSource ? translationSource : "(none)", fromTranslation);
        Check(gotTranslation && translationSource &&
                  std::strcmp(translationSource, "tlyric") == 0,
              L"an empty original falls through to the translation", lyricDetail);
        Check(gotTranslation && std::strstr(fromTranslation, "first line") != nullptr,
              L"the translation's escapes are decoded like the original's", lyricDetail);
        Lyrics& fromTranslationParsed = emptyLyrics;
        Check(ParseLrc(fromTranslation, &fromTranslationParsed) == 2,
              L"the translation parses as LRC", lyricDetail);

        // An instrumental: every field empty. This is the PERMANENT "no lyrics" answer, and it has to
        // stay distinguishable from the transient one, or the retry budget is spent on songs that will
        // never have words.
        const char* instrumental =
            R"({"lrc":{"version":1,"lyric":""},"klyric":{"version":0,"lyric":""},)"
            R"("tlyric":{"version":2,"lyric":""}})";
        char nothing[64] = {};
        Check(!ExtractLyricField(instrumental, nothing, sizeof(nothing)),
              L"a track with every lyric field empty reports no lyrics");

        // The karaoke field carries per-character timing inside the text, which our LRC reader would
        // show verbatim ("ka<00:01.5>ra"), so it is deliberately not read even when it is the only
        // field with anything in it.
        const char* karaokeOnly =
            R"({"lrc":{"lyric":""},"klyric":{"lyric":"[00:01.00]ka<00:01.50>ra"}})";
        Check(!ExtractLyricField(karaokeOnly, nothing, sizeof(nothing)),
              L"the karaoke field is not read as the lyric");

        // QQ answers the flat shape when it is asked not to base64 the text.
        const char* qqJson = R"({"retcode":0,"code":0,"lyric":"[00:01.00]qq line","trans":"x"})";
        char qqJsonLrc[512] = {};
        const char* qqSource = nullptr;
        Check(ExtractLyricField(qqJson, qqJsonLrc, sizeof(qqJsonLrc), &qqSource) &&
                  std::strstr(qqJsonLrc, "qq line") != nullptr && qqSource &&
                  std::strcmp(qqSource, "lyric") == 0,
              L"the flat shape is read, and named as the source");

        // QQ serves XML with the LRC inside an attribute and its newlines entity-encoded.
        const char* qqAttribute =
            "<result lyric=\"[00:03.00]line one&#10;[00:04.50]line two&#10;\">";
        char qqExtracted[512] = {};
        Check(ExtractQqXmlLyric(qqAttribute, qqExtracted, sizeof(qqExtracted)),
              L"the qq xml lyric attribute is found");
        Check(std::strstr(qqExtracted, "line one\n") != nullptr,
              L"&#10; becomes a real newline");
        Check(std::strstr(qqExtracted, "&#10;") == nullptr, L"no entity is left behind");

        Lyrics& qqParsed = emptyLyrics;
        Check(ParseLrc(qqExtracted, &qqParsed) == 2, L"the qq lyric parses as two lines");
        Check(qqParsed.count == 2 && qqParsed.lines[1].timeMs == 4500,
              L"and its times are right");

        // Entity decoding covers the others an LRC can contain.
        char entities[256] = {};
        DecodeXmlEntities("a &lt;b&gt; &quot;c&quot; &apos;d&apos; &amp;e", entities,
                          sizeof(entities));
        Check(std::strcmp(entities, "a <b> \"c\" 'd' &e") == 0,
              L"the other xml entities decode too");

        // Degenerate input must not crash or invent anything.
        char scratch[64] = {};
        Check(!ExtractQqXmlLyric("no attribute here", scratch, sizeof(scratch)),
              L"a qq response without the attribute yields nothing");
        Check(!ExtractQqXmlLyric("lyric=\"", scratch, sizeof(scratch)),
              L"an unterminated qq attribute yields nothing");
        Check(!ExtractLyricField("{\"lyric\":{}}", scratch, sizeof(scratch)),
              L"a nested object under lyric is not mistaken for text");

        // --- the identifier lookups: the track's OWN id, at the object's own level -----------
        //
        // Every case below is the shape the services really answer with (captured with
        // `--lyrics-test <title> <artist> --lyrics-verbose`), because the reader that came before
        // this one passed tests built on made-up documents and then failed on the real thing twice:
        //
        //   * NetEase's song object nests an album and an artist that each carry an `id`, and this
        //     response puts `"id":0` -- an empty artist's -- BEFORE the song's own id, so "the first
        //     `id` after `songs`" returned 0 and the caller reported "the search found no matching
        //     track" over a response with the song in it;
        //   * QQ's response carries three `list` arrays and the first two are EMPTY, so the mid was
        //     read from the wrong array, and the service refused it with `retcode 1101`.
        static const char* const kSongsPath[] = {"songs"};
        long long id = 0;
        wchar_t idDetail[160];
        id = 0;
        const bool plainId = FindFlatIntegerInArray(R"({"songs":[{"id":1,"name":"x"}]})",
                                                   kSongsPath, 1, "id", &id);
        std::swprintf(idDetail, 160, L"found=%d id=%lld", plainId ? 1 : 0, id);
        Check(plainId && id == 1, L"an id in the first object is read at all", idDetail);

        id = 0;
        const bool ownId = FindFlatIntegerInArray(
            R"({"result":{"songs":[{"album":{"id":255723258},"artists":[{"id":0}],"id":3433406446}]}})",
            kSongsPath, 1, "id", &id);
        std::swprintf(idDetail, 160, L"found=%d id=%lld (the album's is 255723258, an artist's 0)",
                      ownId ? 1 : 0, id);
        Check(ownId && id == 3433406446, L"the song's own id is read, not a nested one", idDetail);
        Check(FindFlatIntegerInArray(R"({"songs":[{"id":-7}]})", kSongsPath, 1, "id", &id) &&
                  id == -7,
              L"a negative id is read as a number");
        Check(FindFlatIntegerInArray(R"({"songs":[{"id":"42"}]})", kSongsPath, 1, "id", &id) &&
                  id == 42,
              L"a quoted id is read as a number");
        Check(!FindFlatIntegerInArray(R"({"songs":[{"ids":[1]}]})", kSongsPath, 1, "id", &id),
              L"a key that only appears as a prefix does not match");
        Check(!FindFlatIntegerInArray(R"({"result":{"songs":[]}})", kSongsPath, 1, "id", &id),
              L"an empty result list has no id");

        // ... and the track's LENGTH, read out of the same response (2026-10-13, HANDOFF 8.75).
        //
        // It is what the card prints and what its bar fills against when the session publishes no
        // timeline -- NetEase Cloud Music's case, where `EndTime` stays 00:00 for the whole song. The
        // document below is the real response for `偷心 糯米Nomi` (captured with curl), shortened to the
        // fields this reader touches; `duration` sits beside the song's own `id`, after the album's and
        // an artist's nested objects, which is why it is read by path and not by "the first one".
        {
            long long duration = 0;
            wchar_t durationDetail[160];
            // The delimiter is a word, not the default: the body itself contains `)"` inside a song
            // title ("偷心 (女生版)") and a bare `R"(...)"` would have ended the literal in the middle
            // of the JSON -- which it did, and the compiler's complaint was "newline in a constant".
            const bool read = FindFlatIntegerInArray(
                R"doc({"result":{"songs":[{"album":{"id":255723258},"artists":[{"id":0}],)doc"
                R"doc("duration":243714,"id":2116892727,"name":"偷心 (女生版)"}]}})doc",
                kSongsPath, 1, "duration", &duration);
            std::swprintf(durationDetail, 160, L"found=%d duration=%lld", read ? 1 : 0, duration);
            Check(read && duration == 243714,
                  L"the track's length is read from the same search response as its id",
                  durationDetail);

            duration = 123;
            Check(!FindFlatIntegerInArray(R"({"songs":[{"id":1}]})", kSongsPath, 1, "duration",
                                          &duration) &&
                      duration == 123,
                  L"a response that does not state a length leaves the caller's value alone");

            // QQ states it in SECONDS (`interval`), in the same object as the mid.
            long long seconds = 0;
            static const char* const kQqSongListPath[] = {"song", "list"};
            Check(FindFlatIntegerInArray(
                      R"doc({"req_0":{"data":{"body":{"song":{"list":[{"mid":"0039MnYb0qxYhV",)doc"
                      R"doc("interval":243}]}}}}})doc",
                      kQqSongListPath, 2, "interval", &seconds) &&
                      seconds == 243,
                  L"QQ's length is read, in seconds, from the same list as its mid");
        }

        char mid[64] = {};
        static const char* const kSongListPath[] = {"song", "list"};
        Check(FindFlatStringInArray(
                  R"({"req_0":{"data":{"body":{"album":{"list":[]},"singer":{"list":[]},)"
                  R"("song":{"list":[{"mid":"0039MnYb0qxYhV","file":{"mid":"NESTED"}}]}}}}})",
                  kSongListPath, 2, "mid", mid, sizeof(mid)) &&
                  std::strcmp(mid, "0039MnYb0qxYhV") == 0,
              L"the song's own mid is read from the right list, not a nested one");
        Check(!FindFlatStringInArray(R"({"song":{"list":[]}})", kSongListPath, 2, "mid", mid,
                                     sizeof(mid)),
              L"an empty list has no mid");
        Check(!FindFlatStringInArray(R"({"song":{"list":[{"middle":"x"}]}})", kSongListPath, 2,
                                     "mid", mid, sizeof(mid)),
              L"a longer key containing the name does not match");
        // A value that happens to look like a key must not be mistaken for one.
        Check(!FindFlatStringInArray(R"({"song":{"list":[{"name":"\"mid\":\"FAKE\""}]}})",
                                     kSongListPath, 2, "mid", mid, sizeof(mid)),
              L"a key inside a string value is not read as a key");
    }

    // More lines than the array holds must be dropped, not written past the end.
    //
    // Both the document size and the entry count are derived from kMaxLyricLines.
    // A hardcoded 550 lines overflowed the buffer and tripped the /GS stack check,
    // because this frame also holds a Lyrics by value -- deriving them keeps the two
    // in step if the limit ever changes.
    {
        Lyrics& lyrics = sharedLyrics;
        static char document[96 * 1024] = {};
        const int wanted = kMaxLyricLines + 50;
        size_t used = 0;
        int writtenLines = 0;
        for (int i = 0; i < wanted; ++i) {
            const int written = std::snprintf(document + used, sizeof(document) - used,
                                              "[%02d:%02d.00]line\n", i / 60, i % 60);
            if (written <= 0) break;
            used += static_cast<size_t>(written);
            ++writtenLines;
        }
        Check(writtenLines == wanted, L"the test document holds every line");

        const int count = ParseLrc(document, &lyrics);
        Check(count == kMaxLyricLines, L"excess lines are dropped at the limit");
        Check(lyrics.count == kMaxLyricLines, L"the stored count respects the limit");
        // The last accepted entry must be intact: that is what shows the drop
        // happened at the limit rather than something being overwritten.
        Check(lyrics.lines[kMaxLyricLines - 1].timeMs ==
                  static_cast<long long>(kMaxLyricLines - 1) * 1000,
              L"the last accepted line is intact");
    }
}

void TestMusicCardLayout() {
    Section(L"music card layout");

    const IslandAnchor anchor = AnchorForWindow(0.0f, 0.0f, 2560.0f);
    const RoundedBox card = BoxForState(IslandState::Expanded, anchor);
    const MusicCardLayout layout = MusicCardFor(card);
    const RectF& cardRect = card.rect;

    const auto insideCard = [&](const RectF& r) {
        return r.left >= cardRect.left - 0.01f && r.right <= cardRect.right + 0.01f &&
               r.top >= cardRect.top - 0.01f && r.bottom <= cardRect.bottom + 0.01f;
    };

    Check(insideCard(layout.appRow), L"the app row is inside the card");
    Check(insideCard(layout.artwork), L"the artwork is inside the card");
    Check(insideCard(layout.title) && insideCard(layout.subtitle),
          L"the title and subtitle are inside the card");
    Check(insideCard(layout.progress), L"the progress bar is inside the card");
    Check(insideCard(layout.previous) && insideCard(layout.playPause) &&
              insideCard(layout.next),
          L"the controls are inside the card");

    // The artwork must not touch the card's edge: the card has padding.
    Check(layout.artwork.left > cardRect.left + 4.0f,
          L"the artwork is inset from the card's left edge");
    Check(layout.artwork.top > layout.appRow.top, L"the artwork is below the app row");
    Check(layout.artwork.right < layout.title.left, L"the artwork ends before the title");

    // The text column must start to the right of the cover, and must not run past the
    // card's right padding.
    Check(layout.title.left >= layout.artwork.right,
          L"the title starts to the right of the cover");
    Check(layout.title.right <= cardRect.right - 4.0f,
          L"the title stays inside the right padding");
    Check(layout.subtitle.top >= layout.title.bottom - 0.01f,
          L"the subtitle is below the title");

    // The vertical order is the reference image's, and it is what broke before: the
    // time row used to overlap the cover.
    {
        wchar_t cardDbg[256] = {};
        std::swprintf(cardDbg, 256,
                      L"h %.0f: card y [%.1f,%.1f] artwork y [%.1f,%.1f] elapsed y [%.1f,%.1f] "
                      L"progress y [%.1f,%.1f] controls y [%.1f,%.1f]",
                      cardRect.bottom - cardRect.top, cardRect.top, cardRect.bottom,
                      layout.artwork.top, layout.artwork.bottom, layout.elapsed.top,
                      layout.elapsed.bottom, layout.progress.top, layout.progress.bottom,
                      layout.controls.top, layout.controls.bottom);
        Info(L"music card vertical", cardDbg);
    }
    Check(layout.artwork.bottom <= layout.elapsed.top + 0.01f,
          L"the artwork ends before the time row");
    // The times are ON the bar's line now -- one row, "03:30 --- 03:49" -- so they straddle it rather
    // than sitting above it. What matters is that the two are centred on the same line and that the
    // row clears the controls.
    Check(Nearly((layout.elapsed.top + layout.elapsed.bottom) * 0.5f,
                 (layout.progress.top + layout.progress.bottom) * 0.5f, 0.5f),
          L"the times are centred on the progress bar's own line");
    Check(layout.elapsed.bottom <= layout.controls.top + 0.01f,
          L"the time row clears the controls");
    Check(layout.progress.bottom <= layout.controls.top + 0.01f,
          L"the bar sits above the controls");

    // The elapsed and total labels must not run into each other, or one number would
    // be drawn over the other.
    Check(layout.elapsed.right < layout.remaining.left,
          L"the two time labels do not overlap");

    // The three controls must be ordered, evenly spaced and not overlapping. A gap of
    // zero would merge two hit targets into one.
    Check(layout.previous.right < layout.playPause.left, L"a gap before play/pause");
    Check(layout.playPause.right < layout.next.left, L"a gap after play/pause");
    Check(layout.previous.left < layout.playPause.left &&
              layout.playPause.left < layout.next.left,
          L"the controls are in reading order");

    const float playWidth = layout.playPause.right - layout.playPause.left;
    const float playHeight = layout.playPause.bottom - layout.playPause.top;
    Check(Nearly(playWidth, kMusicPlaySizeDip, 0.01f) &&
              Nearly(playHeight, kMusicPlaySizeDip, 0.01f),
          L"the play/pause target is the size it claims");
    Check(playWidth >= 28.0f, L"the play/pause target is comfortable to click");

    // Every control must be reachable, and the gaps must belong to nothing.
    {
        const auto centreOf = [](const RectF& r) {
            return D2D1::Point2F((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f);
        };
        const D2D1_POINT_2F previous = centreOf(layout.previous);
        const D2D1_POINT_2F playPause = centreOf(layout.playPause);
        const D2D1_POINT_2F next = centreOf(layout.next);

        Check(MusicControlAt(card, previous.x, previous.y) == CardControl::Previous,
              L"a click on the left control is previous");
        Check(MusicControlAt(card, playPause.x, playPause.y) == CardControl::PlayPause,
              L"a click on the middle control is play/pause");
        Check(MusicControlAt(card, next.x, next.y) == CardControl::Next,
              L"a click on the right control is next");
        Check(MusicControlAt(card, (layout.previous.right + layout.playPause.left) * 0.5f,
                             playPause.y) == CardControl::None,
              L"a click in a gap belongs to no control");
        Check(MusicControlAt(card, layout.artwork.left + 4.0f, layout.artwork.top + 4.0f) ==
                  CardControl::None,
              L"a click on the cover is not a control");
    }

    // --- the card's top-right corner is NOT a control (the volume control was removed) ---
    //
    // It held a speaker that opened a percentage and a 64 DIP bar, and the whole group went when the user
    // asked for the speaker to be removed ("把音乐卡片里右上角的图标去掉吧"). What is asserted here is the
    // shape of that decision: the corner is empty, no control claims it, and the source name -- which used to
    // stop short of the volume readout -- is now given the full row.
    {
        // Inside the row's own padding, at its right-hand end: exactly where the speaker used to be.
        const float cornerX = layout.appRow.right - 6.0f;
        const float rowCentre = (layout.appRow.top + layout.appRow.bottom) * 0.5f;
        Check(MusicControlAt(card, cornerX, rowCentre) == CardControl::None,
              L"the card's top-right corner is not a transport button");
        // ... and the corner is inside the row the source occupies, i.e. there is no second strip up there.
        Check(cornerX >= layout.appRow.left && cornerX <= layout.appRow.right &&
                  rowCentre >= layout.appRow.top && rowCentre <= layout.appRow.bottom,
              L"... and it is part of the source row");
        Check(Nearly(layout.appText.right, layout.appRow.right, 0.01f),
              L"the source name now has the whole row");
        Check(layout.appText.left < layout.appText.right,
              L"the source name has room left to draw in");
    }

    // --- the source application's icon ---
    //
    // The icon comes from an executable named by the session's app id, and the app id is not
    // always usable: a packaged app publishes `package!App`, which names no file at all. Getting
    // this wrong means loading some unrelated executable's icon, so the refusals are asserted too.
    {
        wchar_t path[MAX_PATH] = {};

        Check(!ExecutableFromAppId(L"Microsoft.ZuneMusic_8wekyb3d8bbwe!Microsoft.ZuneMusic", path,
                                   MAX_PATH),
              L"a packaged app id yields no executable");
        Check(path[0] == L'\0', L"a rejected app id leaves the path empty");
        Check(!ExecutableFromAppId(L"QQMusic", path, MAX_PATH),
              L"an id that is not an executable name is rejected");
        Check(!ExecutableFromAppId(L"C:\\nope\\nothing-here.exe", path, MAX_PATH),
              L"a path that does not exist is rejected");
        Check(!ExecutableFromAppId(nullptr, path, MAX_PATH), L"a null app id is rejected");

        // A real executable must resolve, so the loader is exercised and not only its refusals.
        // This binary is the one path guaranteed to exist.
        wchar_t self[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, self, MAX_PATH)) {
            Check(ExecutableFromAppId(self, path, MAX_PATH),
                  L"this executable's own path resolves");
            Check(_wcsicmp(path, self) == 0, L"and resolves to itself");

            uint8_t icon[island::kMusicArtworkCapacity] = {};
            Check(FetchAppIconForTest(L"C:\\nope\\nothing-here.exe", icon, sizeof(icon)) == 0,
                  L"a missing executable yields no icon");
            Check(FetchAppIconForTest(self, icon, sizeof(icon)) > 0,
                  L"this executable yields an icon");
        }
    }

    // --- a session's score, which decides which of several is shown ---
    //
    // A browser playing a video used to win simply by being the current session, so the card
    // showed a video instead of the music. These are the comparisons that decide it.
    {
        Check(SessionScoreForTest(L"QQMusic.exe", true) >
                  SessionScoreForTest(L"chrome.exe", true),
              L"playing music outranks a playing browser");
        Check(SessionScoreForTest(L"QQMusic.exe", false) >
                  SessionScoreForTest(L"chrome.exe", false),
              L"paused music outranks a paused browser");
        Check(SessionScoreForTest(L"QQMusic.exe", true) >
                  SessionScoreForTest(L"QQMusic.exe", false),
              L"a playing session outranks the same session paused");
        Check(SessionScoreForTest(L"cloudmusic.exe", true) >
                  SessionScoreForTest(L"msedge.exe", true),
              L"NetEase outranks Edge");
        Check(SessionScoreForTest(L"someUnknownPlayer.exe", true) >
                  SessionScoreForTest(L"firefox.exe", true),
              L"an unknown app is treated as music, not as a browser");
        // Case must not matter: the shell reports ids in whatever case the app chose.
        Check(SessionScoreForTest(L"QQMUSIC.EXE", true) ==
                  SessionScoreForTest(L"qqmusic.exe", true),
              L"the app name comparison ignores case");

        // --- the name a user knows the player by (2026-10-13, HANDOFF 8.78) -----------------------
        //
        // The card's top-left corner shows the executable's ProductName, and for the players that state
        // their own name in Chinese that is exactly right -- QQ音乐. It is not right for one of them:
        // measured with VerQueryValueW, cloudmusic.exe carries ONE version translation (0804/04B0,
        // Simplified Chinese) and the ProductName inside it is the English "NetEase Cloud Music", so
        // there is no Chinese name in the file at all. Reported as "网易云音乐的名字应该改成中文的".
        {
            wchar_t detail[192];
            const wchar_t* netease = KnownPlayerDisplayName(L"cloudmusic.exe");
            const wchar_t* neteaseUpper =
                KnownPlayerDisplayName(L"C:\\PROGRAM FILES\\CloudMusic\\CloudMusic.EXE");
            std::swprintf(detail, 192, L"bare -> '%ls', path and case -> '%ls'",
                          netease ? netease : L"(null)", neteaseUpper ? neteaseUpper : L"(null)");
            Check(netease && std::wcscmp(netease, L"网易云音乐") == 0 && neteaseUpper &&
                      std::wcscmp(neteaseUpper, L"网易云音乐") == 0,
                  L"NetEase is named in Chinese, by bare name and by full path", detail);

            // The table is a CORRECTION, not a translation of everything: a player whose own name is
            // already the right one is deliberately absent, so the version resource keeps answering.
            Check(KnownPlayerDisplayName(L"QQMusic.exe") == nullptr &&
                      KnownPlayerDisplayName(L"chrome.exe") == nullptr &&
                      KnownPlayerDisplayName(L"island_bar.exe") == nullptr &&
                      KnownPlayerDisplayName(nullptr) == nullptr &&
                      KnownPlayerDisplayName(L"") == nullptr,
                  L"players that name themselves are left to their own answer");
        }

        // --- the player that never publishes a session (2026-10-13, HANDOFF 8.73) -----------------
        //
        // Such a player's own window title is the only thing it says out loud, so the title has to be
        // read conservatively: the pill would otherwise announce a page name as a song, and the lyric
        // lookup would come back empty while the island claimed something was playing.
        {
            wchar_t detail[224];
            const wchar_t* const playerNames[] = {
                L"cloudmusic.exe", L"CloudMusic.exe", L"QQMusic.exe",
                L"C:\\SOFTWARE\\CloudMusic\\CloudMusic\\cloudmusic.exe"};
            int accepted = 0;
            for (const wchar_t* name : playerNames) {
                if (IsKnownPlayerExecutable(name)) ++accepted;
            }
            std::swprintf(detail, 224, L"%d of 4 accepted", accepted);
            Check(accepted == 4, L"a known player is recognised by bare name, by case and by full path",
                  detail);

            const wchar_t* const notPlayerNames[] = {L"chrome.exe", L"msedge.exe", L"explorer.exe",
                                                     L"island_bar.exe", L""};
            int refused = 0;
            for (const wchar_t* name : notPlayerNames) {
                if (!IsKnownPlayerExecutable(name)) ++refused;
            }
            std::swprintf(detail, 224, L"%d of 5 refused", refused);
            Check(refused == 5,
                  L"a browser, the shell, the bar itself and an empty name are not players", detail);
        }

        {
            wchar_t detail[224];
            // Each case carries the reason it is the way it is, because the refused ones are the cases
            // that would put "网易云音乐" on the pill as if it were a song.
            struct TitleCase {
                const wchar_t* window;
                const wchar_t* title;   // nullptr = refused
                const wchar_t* artist;
            };
            const TitleCase titleCases[] = {
                // The measured one: what NetEase's window says while a track is up.
                {L"长大以后你是否无忧 - 宋扬/不劉", L"长大以后你是否无忧", L"宋扬/不劉"},
                {L"如愿 - 葱香科学家（王悠然）", L"如愿", L"葱香科学家（王悠然）"},
                {L"I Am You - Kim Taylor", L"I Am You", L"Kim Taylor"},
                {L"  Song  -  Artist  ", L"Song", L"Artist"},
                // From the RIGHT: a song's own title may contain a dash, the artist part does not.
                {L"A - B - C", L"A - B", L"C"},
                // No track up: the window is titled with the application's own name.
                {L"网易云音乐", nullptr, nullptr},
                {L"QQ音乐", nullptr, nullptr},
                {L"网易云音乐 - 每日推荐", nullptr, nullptr},
                {L"Spotify Free", nullptr, nullptr},
                // Half a title is not a title.
                {L"Song - ", nullptr, nullptr},
                {L" - Artist", nullptr, nullptr},
                {L"", nullptr, nullptr},
            };
            int wrong = 0;
            const wchar_t* firstWrong = L"";
            for (const TitleCase& c : titleCases) {
                char title[kPlayerWindowTitleCapacity] = {};
                char artist[kPlayerWindowArtistCapacity] = {};
                const bool parsed = ParsePlayerWindowTitle(c.window, title, sizeof(title), artist,
                                                           sizeof(artist));
                bool ok = false;
                if (c.title == nullptr) {
                    ok = !parsed;
                } else {
                    wchar_t wideTitle[kPlayerWindowTitleCapacity] = {};
                    wchar_t wideArtist[kPlayerWindowArtistCapacity] = {};
                    MultiByteToWideChar(CP_UTF8, 0, title, -1, wideTitle, kPlayerWindowTitleCapacity);
                    MultiByteToWideChar(CP_UTF8, 0, artist, -1, wideArtist,
                                        kPlayerWindowArtistCapacity);
                    ok = parsed && std::wcscmp(wideTitle, c.title) == 0 &&
                         std::wcscmp(wideArtist, c.artist) == 0;
                }
                if (!ok) {
                    if (wrong == 0) firstWrong = c.window;
                    ++wrong;
                }
            }
            std::swprintf(detail, 224, L"%d of %d wrong, first: '%ls'", wrong,
                          static_cast<int>(sizeof(titleCases) / sizeof(titleCases[0])), firstWrong);
            Check(wrong == 0, L"a player window title is read as (song, artist), or refused", detail);
        }

        // --- how long the worker waits, and the SHORT wait after a skip (2026-10-12, HANDOFF 8.38) ---
        //
        // Reported as "切歌的时候点击完 需要等个2-3秒才能切", and the log says where the two and a half seconds
        // went: the click was accepted in 163 ms, and the bar learned about the new track 1.65 s later, at the
        // next scheduled poll. These are the numbers that make the next few polls 250 ms apart instead.
        {
            const unsigned long long skipAt = 1000000ull;
            wchar_t chaseDetail[224];
            std::swprintf(chaseDetail, 224,
                          L"quiet %llu, just after a skip %llu, 5 s later %llu, 7 s later %llu, "
                          L"after a track change %llu",
                          NextPollIntervalMs(skipAt, 0, 0),
                          NextPollIntervalMs(skipAt + 50, skipAt, 0),
                          NextPollIntervalMs(skipAt + 5000, skipAt, 0),
                          NextPollIntervalMs(skipAt + 7000, skipAt, 0),
                          NextPollIntervalMs(skipAt + 50, 0, skipAt));
            Check(NextPollIntervalMs(skipAt, 0, 0) == kPollIntervalMs,
                  L"with nothing happening, the quiet interval", chaseDetail);
            Check(NextPollIntervalMs(skipAt + 50, skipAt, 0) == kSkipPollIntervalMs,
                  L"right after a skip, the short one", chaseDetail);
            Check(NextPollIntervalMs(skipAt + 5000, skipAt, 0) == kSkipPollIntervalMs,
                  L"... and still short five seconds later, because the player is still catching up",
                  chaseDetail);
            Check(NextPollIntervalMs(skipAt + 7000, skipAt, 0) == kPollIntervalMs,
                  L"... and back to the plain interval once the chase window is over", chaseDetail);
            Check(NextPollIntervalMs(skipAt + 50, 0, skipAt) == kSkipPollIntervalMs,
                  L"a track change chased the same way, even with no request from this bar",
                  chaseDetail);

            // ... and the cover is ASKED FOR AGAIN while it is missing, which is the bug behind the user's
            // "关闭后再打开就能查到": the player had published the thumbnail, and the bar had stopped asking
            // because the track had not changed.
            {
                wchar_t coverDetail[224];
                std::swprintf(coverDetail, 224,
                              L"new track %d, missing+1 s %d, missing+0.5 s %d, present+1 s %d, "
                              L"missing+20 s %d",
                              ShouldFetchArtwork(true, false, 5000, 0) ? 1 : 0,
                              ShouldFetchArtwork(false, true, kArtworkRetryMs, 1000) ? 1 : 0,
                              ShouldFetchArtwork(false, true, 500, 1000) ? 1 : 0,
                              ShouldFetchArtwork(false, false, 5000, 1000) ? 1 : 0,
                              ShouldFetchArtwork(false, true, 5000, kArtworkRetryWindowMs) ? 1 : 0);
                Check(ShouldFetchArtwork(true, true, 0, 0),
                      L"a new track always asks for its cover", coverDetail);
                Check(ShouldFetchArtwork(false, true, kArtworkRetryMs, 1000),
                      L"... and a track whose cover has not arrived is asked again", coverDetail);
                Check(!ShouldFetchArtwork(false, true, 500, 1000),
                      L"... but not more than once a second", coverDetail);
                Check(!ShouldFetchArtwork(false, false, 5000, 1000),
                      L"a track that has a cover is not asked at all", coverDetail);
                // THE CASE THAT WAS STILL BROKEN: a player that publishes the thumbnail LATE -- or, as here,
                // more than a minute in. The previous rule stopped asking after kArtworkRetryWindowMs, so
                // 《蚂蚁 (Live)》 showed the placeholder for its whole length while the session was carrying
                // the cover all along ("封面老是查询不到").
                Check(ShouldFetchArtwork(false, true, kArtworkRetrySlowMs, 60000),
                      L"a cover that arrives a minute late is still picked up", coverDetail);
                Check(!ShouldFetchArtwork(false, true, kArtworkRetrySlowMs - 1, 60000),
                      L"... but the late asking is slower than the early one", coverDetail);
            }
        }
    }

    // --- image codec: does a re-encoded image keep its transparency? ---
    //
    // This is the defect that put a WHITE SQUARE on the card where the source application's icon
    // belongs. The PNG encoder converted everything to 24bppBGR -- correct and smaller for opaque
    // album art, and wrong for an icon, whose pixels are mostly transparent. Stripped of alpha,
    // that transparent background became opaque black, which over the card's light wash read as a
    // blank white tile. Two things are asserted here because the encoder got both of them wrong.
    {
        const int side = 8;
        uint8_t source[side * side * 4] = {};
        // Fully transparent, with a fully opaque red square in the middle. Premultiplied, so the
        // transparent pixels are all zero and the opaque red is (0, 0, 255, 255) in BGRA.
        for (int y = 2; y < 6; ++y) {
            for (int x = 2; x < 6; ++x) {
                uint8_t* p = source + (static_cast<size_t>(y) * side + x) * 4;
                p[0] = 0;
                p[1] = 0;
                p[2] = 255;
                p[3] = 255;
            }
        }

        uint8_t png[64 * 1024] = {};
        const size_t pngSize = EncodeBgraToPng(source, side, side, png, sizeof(png));
        Check(pngSize > 0, L"an icon-like image encodes to a PNG");

        // The length must be the real one. Reporting the buffer's capacity instead made
        // `--icon-dump` write an 881-byte PNG as a 32768-byte file with 32 KB of garbage after the
        // IEND chunk, so both the exactness and the end marker are checked.
        Check(pngSize < sizeof(png) / 2,
              L"the encoded length is the real length, not the buffer capacity");
        Check(pngSize >= 4 && png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G',
              L"the encoded bytes start with the PNG signature");
        bool sawIend = false;
        for (size_t i = 0; i + 4 <= pngSize; ++i) {
            if (png[i] == 'I' && png[i + 1] == 'E' && png[i + 2] == 'N' && png[i + 3] == 'D') {
                sawIend = true;
                break;
            }
        }
        Check(sawIend, L"the encoded bytes contain the IEND end marker");

        // And the transparency must survive a decode, which is the part that was broken.
        DecodedImage roundTrip;
        Check(DecodeImage(png, pngSize, &roundTrip), L"the re-encoded image decodes again");
        if (roundTrip.pixels && roundTrip.width == side && roundTrip.height == side) {
            const auto alphaAt = [&](int x, int y) {
                return roundTrip.pixels[(static_cast<size_t>(y) * side + x) * 4 + 3];
            };
            wchar_t detail[160] = {};
            std::swprintf(detail, 160, L"corner alpha %u, centre alpha %u",
                          static_cast<unsigned>(alphaAt(0, 0)),
                          static_cast<unsigned>(alphaAt(4, 4)));
            Check(alphaAt(0, 0) < 16,
                  L"a transparent corner stays transparent through a re-encode", detail);
            Check(alphaAt(4, 4) > 240, L"an opaque centre stays opaque through a re-encode");
        }
    }

    // --- the AppBar: the mapping the shell depends on ---
    //
    // Registering as an AppBar is what makes the desktop give up the strip, and so what makes the
    // island a taskbar rather than a floating decoration. The shell calls cannot be exercised
    // without a desktop that grants the request -- a lower-integrity process is refused outright
    // -- but the mapping onto the shell's own constants is pure and is checked here.
    {
        Check(DockEdgeToAppBarEdge(DockEdge::Top) == ABE_TOP, L"the top edge maps to ABE_TOP");
        Check(DockEdgeToAppBarEdge(DockEdge::Bottom) == ABE_BOTTOM,
              L"the bottom edge maps to ABE_BOTTOM");
        Check(DockEdgeToAppBarEdge(DockEdge::Left) == ABE_LEFT,
              L"the left edge maps to ABE_LEFT");
        Check(DockEdgeToAppBarEdge(DockEdge::Right) == ABE_RIGHT,
              L"the right edge maps to ABE_RIGHT");

        // None is not a valid edge for the shell, and returning 0 would fail with a meaningless
        // error. It maps to ABE_TOP so the call is at least well-formed.
        Check(DockEdgeToAppBarEdge(DockEdge::None) == ABE_TOP,
              L"an undocked bar still produces a valid edge constant");

        // Every result needs a name, because the name is what goes in the log and the whole point
        // of distinguishing them is that the reader can tell them apart.
        const AppBarResult results[] = {AppBarResult::Registered, AppBarResult::NoWindow,
                                       AppBarResult::CallbackRefused,
                                       AppBarResult::NotRegistered,
                                       AppBarResult::SpaceNotGranted};
        bool allNamed = true;
        for (AppBarResult result : results) {
            const wchar_t* name = AppBarResultName(result);
            if (!name || name[0] == L'\0' || std::wcscmp(name, L"?") == 0) allNamed = false;
        }
        Check(allNamed, L"every AppBar outcome has a distinct name for the log");

        // The callback message must be in the WM_APP range, which is what the documentation
        // requires and what keeps it from colliding with a system message.
        const UINT callback = AppBar::CallbackMessage();
        Check(callback >= WM_APP, L"the AppBar callback message is in the WM_APP range");
        // And it must be stable: a second registration of the same message for the same window
        // is refused, so a program that re-registers on every display change would silently stop
        // receiving callbacks.
        Check(AppBar::CallbackMessage() == callback,
              L"the callback message is the same on every call");
    }

    // --- which windows earn a button on the bar ---
    //
    // This is the read side of being a taskbar, and the decision is the hard part: EnumWindows
    // returns every top-level window, including hundreds that must never be listed. The rules are
    // pure so they can be checked here -- the development sandbox has no application windows at
    // all, so nothing that needs a real window to exist can be tested in it.
    {
        // A plain, tidy, visible application window: the case that must be listed.
        WindowTraits normal;
        normal.hasTitle = true;
        normal.visible = true;
        normal.hasSize = true;
        Check(ShouldListWindow(normal), L"a normal visible titled window is listed");

        // The most important rejection. Suspended UWP applications keep a top-level window that
        // IsWindowVisible calls visible and DWM cloaks; a bar that lists them is full of ghosts.
        WindowTraits cloaked = normal;
        cloaked.cloaked = true;
        Check(!ShouldListWindow(cloaked), L"a cloaked window is not listed even though it is visible");

        // Invisible, and it must be rejected on that alone.
        WindowTraits hidden = normal;
        hidden.visible = false;
        Check(!ShouldListWindow(hidden), L"an invisible window is not listed");

        // A visible window with no title that has not declared itself an application.
        WindowTraits untitled = normal;
        untitled.hasTitle = false;
        Check(!ShouldListWindow(untitled), L"an untitled window is not listed");

        // ...but WS_EX_APPWINDOW is a deliberate statement and overrides that.
        WindowTraits untitledApp = untitled;
        untitledApp.appWindow = true;
        Check(ShouldListWindow(untitledApp), L"an untitled window that claims APPWINDOW is listed");

        // An owned window is a dialog or palette belonging to another window.
        WindowTraits owned = normal;
        owned.hasOwner = true;
        Check(!ShouldListWindow(owned), L"a window owned by another window is not listed");

        // Tool windows are floating palettes.
        WindowTraits tool = normal;
        tool.toolWindow = true;
        Check(!ShouldListWindow(tool), L"a tool window is not listed");

        // WS_EX_APPWINDOW is how a window opts back in from either of those.
        WindowTraits ownedApp = owned;
        ownedApp.appWindow = true;
        Check(ShouldListWindow(ownedApp), L"an owned window that claims APPWINDOW is listed");

        // The shell's own furniture: visible, titled, owned by Explorer. Progman IS the desktop,
        // so listing it puts a button on the bar that focuses the wallpaper.
        WindowTraits shell = normal;
        shell.isShell = true;
        Check(!ShouldListWindow(shell), L"the shell's own windows are never listed");

        // The bar must not list its own windows. Its window is topmost and visible, so every
        // other rule would keep it.
        WindowTraits self = normal;
        self.isSelf = true;
        Check(!ShouldListWindow(self), L"this process's own windows are never listed");

        // A zero-extent window cannot be seen or clicked.
        WindowTraits empty = normal;
        empty.hasSize = false;
        Check(!ShouldListWindow(empty), L"a window with no extent is not listed");

        // Order of precedence: being cloaked outranks claiming APPWINDOW, because a cloaked
        // window is invisible no matter what its styles say.
        WindowTraits cloakedApp = cloaked;
        cloakedApp.appWindow = true;
        Check(!ShouldListWindow(cloakedApp), L"cloaked beats an APPWINDOW claim");

        // --- choosing what to bring forward ---
        TaskEntry entries[4] = {};
        entries[0].handle = reinterpret_cast<HWND>(static_cast<INT_PTR>(0x1000));
        entries[1].handle = reinterpret_cast<HWND>(static_cast<INT_PTR>(0x2000));
        entries[2].handle = reinterpret_cast<HWND>(static_cast<INT_PTR>(0x3000));
        entries[1].iconic = true;

        // The first window that is neither the current one nor minimised wins.
        Check(PickNextWindow(entries, 3, entries[0].handle) == 1 ||
                  PickNextWindow(entries, 3, entries[0].handle) == 2,
              L"the next window skips the current one");
        Check(PickNextWindow(entries, 3, entries[1].handle) == 0,
              L"a minimised entry is not chosen while a restored one exists");
        Check(PickNextWindow(entries, 0, nullptr) == -1, L"an empty list chooses nothing");
        Check(PickNextWindow(nullptr, 3, nullptr) == -1, L"a null list chooses nothing");

        // Every entry minimised or current: something is still returned rather than nothing, so a
        // click can never be a silent no-op.
        TaskEntry onlyMinimised[1] = {};
        onlyMinimised[0].handle = reinterpret_cast<HWND>(static_cast<INT_PTR>(0x4000));
        onlyMinimised[0].iconic = true;
        Check(PickNextWindow(onlyMinimised, 1, nullptr) == 0,
              L"a single minimised entry is still chosen");
        Check(PickNextWindow(onlyMinimised, 1, onlyMinimised[0].handle) == 0,
              L"the current window is chosen when it is the only one");
    }

    // --- the island and the row of application icons beside it ---
    //
    // The icons are a permanent part of the bar, not a hover feature: opening an application adds an
    // icon next to the island and it stays there. Hover only widens the buttons so each can say what
    // it is. These assertions are the ones that keep that true, and the first one is the point of
    // the whole design -- an island that moved every time an application opened would be worse than
    // no island.
    {
        const float monitorWidth = 2560.0f;
        const RoundedBox island = IslandBoxFor(monitorWidth, kCollapsedWidthDip,
                                               kCollapsedHeightDip);

        // The island is centred, and stays centred no matter how many icons there are: its position
        // is a function of the monitor width alone.
        const float islandCentre = (island.rect.left + island.rect.right) * 0.5f;
        Check(Nearly(islandCentre, monitorWidth * 0.5f, 0.01f),
              L"the island is centred on the monitor");
        Check(Nearly(island.rect.top, kPillTopMarginDip, 0.01f),
              L"the island sits at the top margin");

        // The row starts to the right of the island.
        const TaskIconRow row = TaskRowFor(island, 4);
        Check(row.valid, L"a row of four icons has a position");
        Check(row.left >= island.rect.right,
              L"the icon row starts to the right of the island, never under it");
        Check(Nearly(row.left, island.rect.right + kTaskIconPadDip, 0.01f),
              L"the row is separated from the island by the padding");
        Check(TaskRowFor(island, 0).valid == false, L"no icons means no row");

        // At rest each button is a square, and they are laid out left to right without overlapping.
        // -1 means no button is widened.
        {
            const RectF first = TaskIconButtonAt(island, 0, 4, -1, 0.0f);
            const RectF second = TaskIconButtonAt(island, 1, 4, -1, 0.0f);
            Check(Nearly(first.right - first.left, kTaskIconButtonDip, 0.01f),
                  L"a resting icon button is square");
            Check(second.left >= first.right, L"resting icon buttons do not overlap");
            Check(Nearly(second.left, first.right + kTaskIconGapDip, 0.01f),
                  L"resting buttons are separated by the gap");
        }

        // The icon inside a button is square and smaller than it.
        {
            const RectF icon = TaskIconAt(island, 0, 4);
            Check(Nearly(icon.right - icon.left, kTaskIconSizeDip, 0.01f) &&
                      Nearly(icon.bottom - icon.top, kTaskIconSizeDip, 0.01f),
                  L"the icon is square");
            Check(icon.left >= TaskIconButtonAt(island, 0, 4, -1, 0.0f).left &&
                      icon.right <= TaskIconButtonAt(island, 0, 4, -1, 0.0f).right,
                  L"the icon stays inside its button");
        }

        // The pointed-at button grows, and the row makes room for it.
        //
        // Two amounts are animated: `hover` is how far the pointed-at button has grown, `shift` is
        // how far the buttons before it have pushed it along. The properties below are the ones that
        // matter, and the second was learned the hard way:
        //
        //  * the ICON never moves, so the thing being pointed at does not slide away;
        //  * the button's own left edge DOES move, but only as the row opens -- and while `shift` is
        //    zero nothing moves at all. An earlier version pushed the neighbours aside in a single
        //    frame, which shuffled the row and closed a loop: the choice of which button the pointer
        //    was on was made from a layout that the choice itself had changed, so the second icon
        //    could never be reached. Animating the shift is what makes the movement smooth, and
        //    deciding on the RESTING layout (TaskIconUnderPointer) is what breaks the loop.
        {
            const RectF restButton = TaskIconButtonAt(island, 0, 4, -1, 0.0f);
            const RectF grownButton = TaskIconButtonAt(island, 0, 4, 0, 1.0f);
            Check(Nearly(grownButton.right - grownButton.left, kTaskWideButtonDip, 0.01f),
                  L"a fully grown button is the wide size");
            Check(grownButton.right > restButton.right,
                  L"a grown button is wider than a resting one");

            // The icon does not move, whichever way the row opens.
            const RectF restIcon = TaskIconAt(island, 0, 4);
            Check(restIcon.left >= restButton.left && restIcon.right <= grownButton.right,
                  L"the icon stays inside its button while it grows");

            // With the shift at zero the row has NOT moved: every button keeps the left edge it has
            // at rest. This is the state the pointer's position is judged against, so it has to be
            // genuinely static -- and it is the property that breaks the loop, because the choice of
            // which button the pointer is on cannot depend on the growth that choice is about to cause.
            //
        // Nothing is open at amount 0, and everything is exactly where the resting layout puts it.
        //
        // This is the geometry the pointer's position is judged against, so it has to be genuinely
        // independent of the animation: the choice of which button the pointer is on cannot depend on
        // the growth that choice is about to cause. Stated explicitly here because the resting case
        // once emerged from arithmetic by accident, and stopped being resting when the arithmetic
        // changed -- a unit test caught it, which is the only reason it did not reach the screen.
        {
            bool anyMoved = false;
            for (int i = 0; i < 4; ++i) {
                const RectF resting = TaskIconButtonAt(island, i, 4, -1);
                const RectF notYetOpen = TaskIconButtonAt(island, i, 4, 0, 0.0f);
                if (!Nearly(resting.left, notYetOpen.left, 0.01f) ||
                    !Nearly(resting.right - resting.left, notYetOpen.right - notYetOpen.left, 0.01f)) {
                    anyMoved = true;
                }
            }
            Check(!anyMoved, L"at zero growth every button is exactly where it rests");
        }

        // A button after the growing one starts after it ENDS -- at every point of the animation.
        //
        // This is the property that was violated, twice reported and twice misdiagnosed. The open
        // button drew itself using one animated value while the buttons after it were pushed using
        // another; the two agreed at rest and disagreed in between, and while the push lagged, the
        // open button's right edge ran past the next button's left edge and painted over it. The row
        // looked like it had not made room, because in the sense that matters it had not.
        //
        // Checked across the WHOLE transition rather than at its ends, because the ends are exactly
        // where the two values happened to agree. Checking only the resting and settled states is what
        // let this survive two rounds of diagnosis.
        {
            bool overlaps = false;
            bool leavesGap = false;
            float worst = 0.0f;
            for (int step = 0; step <= 40; ++step) {
                const float amount = static_cast<float>(step) / 40.0f;
                const RectF opened = TaskIconButtonAt(island, 0, 4, 0, amount);
                const RectF next = TaskIconButtonAt(island, 1, 4, 0, amount);

                if (opened.right > next.left + 0.01f) {
                    overlaps = true;
                    const float over = opened.right - next.left;
                    if (over > worst) worst = over;
                }
                if (next.left > opened.right + kTaskIconGapDip + 0.01f) leavesGap = true;
            }
            wchar_t detail[160];
            std::swprintf(detail, 160, L"the worst overlap across the transition was %.2f DIP", worst);
            Check(!overlaps, L"an opening button never draws over the button after it", detail);
            Check(!leavesGap, L"an opening button never leaves a hole before the next one", detail);

            // The spacing is the same open and closed: one gap, always.
            const RectF opened = TaskIconButtonAt(island, 0, 4, 0, 1.0f);
            const RectF next = TaskIconButtonAt(island, 1, 4, 0, 1.0f);
            Check(Nearly(next.left, opened.right + kTaskIconGapDip, 0.01f),
                  L"an open button is followed by the same gap as a resting one", detail);

            // And the next button keeps its own size: only the pointed-at one changes width.
            Check(Nearly(next.right - next.left, kTaskIconButtonDip, 0.01f),
                  L"a button that is pushed along keeps its own size");
        }

        // Half way through, the movement is half way: continuous, which is what makes it read as one
        // object opening rather than as a jump.
        {
            const RectF half = TaskIconButtonAt(island, 0, 4, 0, 0.5f);
            const float halfWidth = half.right - half.left;
            Check(halfWidth > kTaskIconButtonDip && halfWidth < kTaskWideButtonDip,
                  L"a half-open button is between the two widths");

            const RectF nextHalf = TaskIconButtonAt(island, 1, 4, 0, 0.5f);
            const RectF nextRest = TaskIconButtonAt(island, 1, 4, -1);
            const float halfPush = nextHalf.left - nextRest.left;
            Check(halfPush > 0.01f && halfPush < (kTaskWideButtonDip - kTaskIconButtonDip),
                  L"a half-open button has pushed the next one half way");
        }
        }

        // No title unless THIS button is the growing one.
        {
            Check(TaskTitleAt(island, 0, 4, -1, 0.0f).right <= TaskTitleAt(island, 0, 4, -1, 0.0f).left,
                  L"a resting button has no title slot");
            Check(TaskTitleAt(island, 1, 4, 0, 1.0f).right <= TaskTitleAt(island, 1, 4, 0, 1.0f).left,
                  L"only the growing button has a title slot");
            const RectF title = TaskTitleAt(island, 0, 4, 0, 1.0f);
            Check(title.right > title.left, L"a grown button has a title slot");
            Check(title.left >= TaskIconAt(island, 0, 4).right,
                  L"the title starts after the icon");
        }

        // Whichever button grows, only IT changes size.
        //
        // The others may be pushed along, but their width is their own. A button that changed size
        // because a neighbour was pointed at would make the row twitch as the pointer travelled.
        {
            for (int hovered = 0; hovered < 4; ++hovered) {
                for (int i = 0; i < 4; ++i) {
                    if (i == hovered) continue;
                    const RectF resting = TaskIconButtonAt(island, i, 4, -1, 0.0f);
                    const RectF whileOtherGrows = TaskIconButtonAt(island, i, 4, hovered, 1.0f);
                    Check(Nearly(resting.right - resting.left,
                                 whileOtherGrows.right - whileOtherGrows.left, 0.01f),
                          L"only the pointed-at button changes size");
                }
            }
        }

        // The row OPENING, end to end, at real screen coordinates.
        //
        // The assertions above check the layout in isolation; this one walks the case the user
        // actually performs -- the pointer arriving at the end of a row that starts at the centre of a
        // 2560 DIP monitor -- and checks that the next button has made room by the time the first one
        // is fully open. Without it, a layout that computed the shift but never received it would
        // pass every other check here.
        {
            const float monitorWide = 2560.0f;
            const RoundedBox wideIsland =
                IslandBoxFor(monitorWide, kCollapsedWidthDip, kCollapsedHeightDip);
            const int shown = 5;

            // Before: the second button at its resting place.
            const RectF secondRest = TaskIconButtonAt(wideIsland, 1, shown, -1, 0.0f);

            // After: the first fully open, which is the state the frame loop animates towards.
            const RectF secondAfter = TaskIconButtonAt(wideIsland, 1, shown, 0, 1.0f);
            const float room = secondAfter.left - secondRest.left;
            const float growth = kTaskWideButtonDip - kTaskIconButtonDip;

            wchar_t detail[192];
            std::swprintf(detail, 192,
                          L"second button moved %.1f DIP, the first grew %.1f DIP", room, growth);
            Check(Nearly(room, growth, 0.01f),
                  L"the row has made room by the time the first button is fully open", detail);

            // And the first button's own right edge does not overrun where the second one now starts.
            const RectF firstOpen = TaskIconButtonAt(wideIsland, 0, shown, 0, 1.0f);
            Check(firstOpen.right <= secondAfter.left,
                  L"an open button does not overlap the button after it", detail);
        }

        // The row makes room, and the pointer's icon does not slip away from it.
        //
        // This is the pair of properties that were wrong together. Opening a button pushes the ones
        // after it right, so a hit test that consulted only the resting layout would decide the
        // pointer had left the row -- closing the button being pointed at, and (because "not on an
        // icon" is what lets the island open) expanding the island instead.
        {
            const float monitorWide = 2560.0f;
            const RoundedBox wideIsland =
                IslandBoxFor(monitorWide, kCollapsedWidthDip, kCollapsedHeightDip);
            const int shown = 5;

            const RectF secondRest = TaskIconButtonAt(wideIsland, 1, shown, -1, 0.0f);
            const RectF secondOpen = TaskIconButtonAt(wideIsland, 1, shown, 0, 1.0f);
            const float centreY = (secondRest.top + secondRest.bottom) * 0.5f;

            // A pointer on the second button, at rest, picks the second button.
            const float onSecond = (secondRest.left + secondRest.right) * 0.5f;
            Check(TaskIconUnderPointer(wideIsland, shown, onSecond, centreY) == 1,
                  L"the pointer on the second button chooses the second button");

            // Now the first button has opened and pushed the second one right. The pointer has not
            // moved, so it may well land in the GAP that has opened up between the grown first button
            // and the second one -- which is over no resting button at all. It is still on the row,
            // though, so the second button stays open rather than being abandoned.
            //
            // The gap is where this matters: a point inside a resting button is found either way, so
            // testing one of those proves nothing. This point is deliberately inside the gap.
            const float inTheGap = secondRest.left - 2.0f;
            Check(TaskIconUnderPointer(wideIsland, shown, inTheGap, centreY) == -1,
                  L"a point in the gap is over no resting button");
            Check(TaskRowContains(wideIsland, shown, inTheGap, centreY),
                  L"but that point is still on the row");

            const int kept = TaskIconToKeepOpen(wideIsland, shown, 1, inTheGap, centreY);
            wchar_t detail[160];
            std::swprintf(detail, 160, L"kept %d for a point %.0f DIP into the gap at %.0f", kept,
                          secondRest.left - inTheGap, inTheGap);
            Check(kept == 1,
                  L"the open button is not abandoned when the row moves under the pointer", detail);

            // And the pointer that really does leave the row gives up the icon, so the island may
            // open again.
            Check(TaskIconToKeepOpen(wideIsland, shown, 1, wideIsland.rect.left, centreY) == -1,
                  L"a pointer back on the island keeps no icon open");

            // ... AND THE HOVER ROW MUST BE THE ONE THAT IS DRAWN. With a notice strip up the bar's own
            // capsule is 420 DIP, not 220, and the icon row is anchored to ITS right edge -- so a point
            // that is the second icon on the pill's row is the first (or a gap) on the strip's row.
            // The window used to hover-test the pill's row while the renderer drew the strip's, which
            // is the "有时候鼠标放上去没有展开".
            {
                const RoundedBox stripIsland =
                    IslandBoxFor(monitorWide, kNoticeStripWidthDip, kCollapsedHeightDip);
                const int onPillRow = TaskIconToKeepOpen(wideIsland, shown, -1, onSecond, centreY);
                const int onDrawnRow = TaskIconToKeepOpen(stripIsland, shown, -1, onSecond, centreY);
                std::swprintf(detail, 160,
                              L"one point: icon %d on the pill's row, %d on the strip's", onPillRow,
                              onDrawnRow);
                Check(onPillRow == 1 && onDrawnRow != 1,
                      L"the hover row is the resting pill the row is drawn from", detail);
            }
        }

        // The title only gets drawn once its box is wide enough to read.
        //
        // A title in a narrow box is trimmed to a single ellipsis, so the button appears to contain
        // "..." and nothing else. The gate used to be a fraction of the growth; it is now the room the
        // text actually has, which is the thing that decides whether an ellipsis appears.
        {
            const RectF fullBox = TaskTitleAt(island, 0, 4, 0, 1.0f);
            const float full = fullBox.right - fullBox.left;
            wchar_t detail[192];
            std::swprintf(detail, 192, L"a fully open title has %.0f DIP of room, minimum %.0f",
                          full, kTaskTitleMinWidthDip);
            Check(full >= kTaskTitleMinWidthDip,
                  L"a fully open button leaves the title enough room to read", detail);

            // The old gate's value, measured rather than assumed.
            //
            // This assertion exists because the first version of this check CLAIMED 0.55 gave only
            // 24 DIP and used that to justify the change. The measurement said 45 DIP, so the claim
            // was wrong and the assertion was rewritten to state the truth. A justification that is
            // never measured is how a wrong explanation gets into the record.
            const RectF halfBox = TaskTitleAt(island, 0, 4, 0, 0.55f);
            const float atOldGate = halfBox.right - halfBox.left;
            std::swprintf(detail, 192, L"at 55%% growth the box is %.0f DIP", atOldGate);
            Check(atOldGate >= 0.0f,
                  L"the box has a size at the old gate, so the gate was not the cause", detail);

            // Once the box has any width it only grows, so a title cannot appear and then vanish
            // mid-transition and flicker.
            bool monotonic = true;
            float previous = 0.0f;
            for (int step = 0; step <= 20; ++step) {
                const float amount = static_cast<float>(step) / 20.0f;
                const RectF box = TaskTitleAt(island, 0, 4, 0, amount);
                const float width = box.right - box.left;
                if (width < 0.0f) continue;   // still narrower than the icon: no box yet
                if (width < previous - 0.01f) monotonic = false;
                previous = width;
            }
            Check(monotonic, L"the title box only ever grows while the button opens", detail);
        }

        // THE SPRING THE BUTTON OPENS ON, which replaced a linear ramp.
        //
        // The ramp (1/0.16 of the way per second, clamped at the ends) is smooth and it settles nowhere:
        // it stops dead. The spring's whole point is the tail, and the whole risk is that a spring
        // integrated with the frame's own dt can ring forever or diverge -- this machine's frame times have
        // been measured as high as 139 ms. So what is asserted is the shape a spring has to have, and the
        // stability that sub-stepping buys.
        {
            wchar_t springDetail[192];
            float value = 0.0f;
            float velocity = 0.0f;
            float worst = 0.0f;
            int frames = 0;
            while (RowOpenStep(&value, &velocity, 1.0f, 1.0f / 60.0f) && frames < 600) {
                ++frames;
                if (value > worst) worst = value;
            }
            std::swprintf(springDetail, 192, L"settled at %.4f (after %d frames), peak %.4f", value,
                          frames, worst);
            Check(std::fabs(value - 1.0f) < 0.001f && frames < 45,
                  L"the row's opening reaches the button's full width and settles inside 0.75 s",
                  springDetail);
            Check(worst > 1.0f && worst < 1.06f,
                  L"... and overshoots by less than a twentieth: a settle, not a bounce", springDetail);

            // The same, driven with the worst frame time this machine has produced.
            float coarse = 0.0f;
            float coarseVelocity = 0.0f;
            float coarseWorst = 0.0f;
            for (int i = 0; i < 20; ++i) {
                RowOpenStep(&coarse, &coarseVelocity, 1.0f, 0.139f);
                if (coarse > coarseWorst) coarseWorst = coarse;
            }
            std::swprintf(springDetail, 192, L"after 20 frames of 139 ms: %.4f, peak %.4f", coarse,
                          coarseWorst);
            Check(std::fabs(coarse - 1.0f) < 0.01f && coarseWorst < 1.1f,
                  L"and it is stable at a 139 ms frame, which is what the sub-stepping is for",
                  springDetail);

            // A wash is a REACTION -- the pointer caused it -- and it has to be over sooner than the
            // row's own opening, or the button feels like it is lagging behind the hand.
            float wash = 0.0f;
            float washVelocity = 0.0f;
            int washFrames = 0;
            while (ControlWashStep(&wash, &washVelocity, 1.0f, 1.0f / 60.0f) && washFrames < 600) {
                ++washFrames;
            }
            std::swprintf(springDetail, 192, L"wash %d frames, row %d frames (60 Hz)", washFrames,
                          frames);
            Check(std::fabs(wash - 1.0f) < 0.001f && washFrames < frames,
                  L"a hover wash settles sooner than the row's opening", springDetail);
        }

        // A row where MANY buttons are mid-transition at once, which is the live case.
        //
        // Attempting to reproduce the failure with two non-zero amounts did not work: the old code
        // gives no overlap for any pair, because it made room for whichever button had the larger
        // amount. The live row has more than two buttons part-way through at the same time -- the
        // pointer travels, and several are opening or closing at various rates -- so the test has to
        // model that, not a hand-picked pair.
        //
        // What is asserted is the invariant the layout is supposed to guarantee for ANY widths: each
        // button starts exactly one gap after the previous one ends. Overlap is then impossible by
        // construction, which is the property that should have been written from the beginning.
        {
            bool broken = false;
            float worst = 0.0f;
            unsigned seed = 12345u;

            for (int trial = 0; trial < 200; ++trial) {
                float amounts[8] = {};
                for (int i = 0; i < 8; ++i) {
                    // A cheap deterministic spread over 0..1, including runs of equal values and
                    // exact 0 and 1, so both ends and the middle are covered.
                    seed = seed * 1103515245u + 12345u;
                    amounts[i] = static_cast<float>((seed >> 16) % 21) / 20.0f;
                }

                for (int i = 0; i + 1 < 8; ++i) {
                    const RectF left = TaskIconButtonAtAmounts(island, i, 8, amounts);
                    const RectF right = TaskIconButtonAtAmounts(island, i + 1, 8, amounts);
                    const float gap = right.left - left.right;
                    if (gap < kTaskIconGapDip - 0.01f) {
                        broken = true;
                        if (kTaskIconGapDip - gap > worst) worst = kTaskIconGapDip - gap;
                    }
                }
            }
            wchar_t detail2[192];
            std::swprintf(detail2, 192,
                          L"200 randomised rows, worst shortfall in the gap: %.2f DIP", worst);
            Check(!broken,
                  L"every button starts exactly one gap after the previous one, however many are "
                  L"mid-transition",
                  detail2);
        }

        // The icon travels with ITS OWN button.
        //
        // This is the assertion for "the background moves but the icons do not", which was on screen
        // while every assertion here passed. The icon was being placed from the button's RESTING
        // rectangle while the button itself was placed from the animated one, so once the row made room
        // the button slid and left its icon behind. Nothing above could see it, because nothing above
        // ever compared an icon's position against its button's position.
        //
        // Checked over the same randomised rows: whenever a button is somewhere other than its resting
        // place, its icon must have moved with it by exactly the same amount.
        {
            bool detached = false;
            float worst = 0.0f;
            unsigned seed2 = 999u;

            for (int trial = 0; trial < 200; ++trial) {
                float amounts[8] = {};
                for (int i = 0; i < 8; ++i) {
                    seed2 = seed2 * 1103515245u + 12345u;
                    amounts[i] = static_cast<float>((seed2 >> 16) % 21) / 20.0f;
                }

                for (int i = 0; i < 8; ++i) {
                    const RectF button = TaskIconButtonAtAmounts(island, i, 8, amounts);
                    const RectF resting = TaskIconButtonAt(island, i, 8, -1);
                    const RectF icon = TaskIconInBox(button);

                    // The icon's offset inside its button is the same however the row has moved.
                    const float movedWith = (icon.left - button.left) - (kTaskIconButtonDip -
                                                                        kTaskIconSizeDip) * 0.5f;
                    if (movedWith > 0.01f || movedWith < -0.01f) {
                        detached = true;
                        if (movedWith > worst) worst = movedWith;
                    }
                    (void)resting;
                }
            }
            wchar_t detail3[192];
            std::swprintf(detail3, 192, L"worst drift of an icon inside its own button: %.3f DIP",
                          worst);
            Check(!detached, L"every icon sits at a fixed offset inside its own button", detail3);
        }

        // A click on an icon must select the window that icon stands for.
        //
        // This is the join between two things that are easy to get out of step: the row's geometry,
        // which says which icon was clicked, and the window list, which says which window an icon
        // index refers to. Both are indexes into the same list and nothing else enforces it -- the
        // frame loop fills button i from At(i), and the click handler activates At(icon).
        //
        // Exercised against a real WindowList, because an empty one would agree with anything.
        {
            island::WindowList live;
            live.Refresh();
            const int listed = live.Count();

            wchar_t detail[192];
            std::swprintf(detail, 192, L"the live desktop has %d windows listed", listed);
            Check(listed > 0, L"the window list found something to point at", detail);

            if (listed > 0) {
                const RoundedBox wideIsland =
                    IslandBoxFor(monitorWidth, kCollapsedWidthDip, kCollapsedHeightDip);

                // For each shown icon, the middle of its resting button must come back as that icon.
                const int shown = island::TaskIconsThatFit(monitorWidth, listed);
                bool everyIconAnswers = true;
                int wrong = -1;
                for (int i = 0; i < shown; ++i) {
                    const RectF button = TaskIconButtonAt(wideIsland, i, shown, -1);
                    const float midX = (button.left + button.right) * 0.5f;
                    const float midY = (button.top + button.bottom) * 0.5f;
                    if (TaskIconUnderPointer(wideIsland, shown, midX, midY) != i) {
                        everyIconAnswers = false;
                        wrong = i;
                    }
                }
                std::swprintf(detail, 192, L"%d icons shown, first disagreement at index %d", shown,
                              wrong);
                Check(everyIconAnswers,
                      L"the centre of every shown icon reports that same icon", detail);

                // A point past the last icon is not an icon, which is what stops a click to the right
                // of the row from activating a window that has no icon on screen.
                const RectF last = TaskIconButtonAt(wideIsland, shown - 1, shown, -1);
                Check(TaskIconUnderPointer(wideIsland, shown, last.right + 20.0f,
                                           (last.top + last.bottom) * 0.5f) == -1,
                      L"a point past the row activates nothing", detail);

                // And the handle the handler would activate is the one that window list holds.
                const island::TaskEntry entry = live.At(0);
                Check(entry.handle != nullptr || listed == 0,
                      L"a listed window has a handle to activate", detail);
            }
        }

        // A refresh must not move the buttons.
        //
        // EnumWindows returns windows in Z order, and activating a window changes its Z order -- so
        // using that order directly meant clicking a button moved that button to the front. The report
        // was "the icon jumped to the first position". The order is now remembered across refreshes:
        // existing windows keep their place, new ones append.
        //
        // Refreshed repeatedly against the live desktop, because that is what the bar does every 500 ms
        // and a single refresh cannot show a reorder.
        {
            island::WindowList stable;
            stable.Refresh();
            const int firstCount = stable.Count();

            HWND before[island::kMaxTaskWindows] = {};
            for (int i = 0; i < firstCount; ++i) before[i] = stable.At(i).handle;

            bool moved = false;
            int where = -1;
            for (int pass = 0; pass < 5; ++pass) {
                stable.Refresh();
                const int now = stable.Count();
                for (int i = 0; i < firstCount && i < now; ++i) {
                    if (stable.At(i).handle != before[i]) {
                        moved = true;
                        if (where < 0) where = i;
                    }
                }
            }

            wchar_t detail4[192];
            std::swprintf(detail4, 192, L"%d buttons tracked over 5 refreshes, first move at %d",
                          firstCount, where);
            Check(!moved, L"a refresh does not reorder the buttons", detail4);
        }

        // The taskbar button is a toggle, and the toggle is decided by asking who is in front.
        //
        // Only the SAFE parts are asserted here. Actually minimising a window during a self-test would
        // put away something the person running it was using, and actually raising one would steal
        // their focus -- so the decision inputs and the refusal paths are checked, and the gesture
        // itself is left to a real click.
        {
            island::WindowList live;
            live.Refresh();

            // A null window is refused rather than acted on, for both gestures.
            Check(!live.Minimize(nullptr), L"minimising no window is refused, not attempted");
            Check(!live.Activate(nullptr), L"activating no window is refused, not attempted");

            // A handle that is not a window is refused too.
            const HWND bogus = reinterpret_cast<HWND>(static_cast<INT_PTR>(0x1234));
            Check(!live.Minimize(bogus), L"minimising a dead handle is refused");
            Check(!live.Activate(bogus), L"activating a dead handle is refused");

            // The toggle reads the LIVE foreground, not the list's cached one: the cache is refreshed
            // twice a second, and the whole question is which window is in front at the click.
            const HWND front = GetForegroundWindow();
            Check(live.ForegroundHandle() == front ||
                      live.ForegroundHandle() == nullptr,
                  L"the cached foreground is the live one, or nothing yet");
        }

        // The strip the bar reserves when it claims the top edge has to cover the whole chrome.
        //
        // The value is asserted because claiming the strip is refused outright on the development
        // machine, so this arithmetic can never be checked by running it.
        //
        // The reason it is now stated as its own constant is a mistake worth recording: the reservation
        // was written as margin + COLLAPSED height, and kCollapsedHeightDip happens to equal
        // kPillHeightDip (both 36), so it evaluated to the right number by coincidence. Reading it, the
        // expression looks like it reserves the height of the collapsed pill and not the chrome -- and
        // the next person to change the collapsed pill's size would have silently changed how much of
        // the screen the bar claims. The assertion below pins the value so that cannot happen quietly.
        {
            wchar_t detail5[192];
            std::swprintf(detail5, 192, L"reserved %.0f DIP = margin %.0f + pill %.0f",
                          kReservedBarHeightDip, kPillTopMarginDip, kPillHeightDip);
            Check(Nearly(kReservedBarHeightDip, kPillTopMarginDip + kPillHeightDip, 0.01f),
                  L"the reserved strip covers the pill and its top margin", detail5);

            // The value is pinned, because the original expression got it right by coincidence:
            // kCollapsedHeightDip equals kPillHeightDip (both 36), so "margin + collapsed" happened to
            // equal "margin + pill". Reading it, the expression claims to reserve the collapsed pill's
            // height rather than the chrome's -- and the next person to change the collapsed pill would
            // have silently changed how much of the screen the bar takes.
            Check(kReservedBarHeightDip == kPillTopMarginDip + kPillHeightDip,
                  L"the reserved strip is exactly the margin plus the pill", detail5);
        }

        // The strip rectangle depends ONLY on the monitor, never on the work area.
        //
        // This is the assertion for the failure that pushed the user's whole desktop down: the bar
        // claimed 42 px off the top, the work area moved to 42, the shell sent ABN_POSCHANGED, the bar
        // re-claimed -- and computed its rectangle from the work area it had just changed, so it asked
        // for 42 px starting at 42. Then 84, then 126, until the reservation was off the bottom of the
        // screen and every maximized window was pushed down with it.
        //
        // The old landing check could not catch it, because it compared each edge to its OWN previous
        // value: every step took another legitimate-looking thickness. Stability against the monitor is
        // the property that was missing, so it is the property asserted.
        {
            const RECT monitor = {0, 0, 2560, 1440};
            const LONG bar = 42;

            const RECT first = AppBarStripRect(monitor, DockEdge::Top, bar);
            Check(first.top == 0 && first.bottom == bar && first.left == 0 && first.right == 2560,
                  L"a top bar's strip is one thickness off the monitor's top");

            // The work area after that reservation, which is what the old code fed back in.
            const RECT shrunkWork = {0, bar, 2560, 1440 - 32};

            // Asking again must produce exactly the same rectangle. Feeding the shrunk work area in
            // would have produced {0, 42, 2560, 84} -- the second step of the walk.
            const RECT again = AppBarStripRect(monitor, DockEdge::Top, bar);
            wchar_t detail6[192];
            std::swprintf(detail6, 192,
                          L"asked twice: %ld,%ld %ldx%ld then %ld,%ld %ldx%ld (work area top is %ld)",
                          first.left, first.top, first.right - first.left, first.bottom - first.top,
                          again.left, again.top, again.right - again.left, again.bottom - again.top,
                          shrunkWork.top);
            Check(again.top == first.top && again.bottom == first.bottom &&
                      again.left == first.left && again.right == first.right,
                  L"the strip rectangle does not move when asked again after the work area shrank",
                  detail6);

            // And the walk itself, were it to happen, is detectable: the second step is a different
            // rectangle from the first, which is what the old code would have produced.
            const RECT walked = AppBarStripRect(shrunkWork, DockEdge::Top, bar);
            Check(walked.top != first.top,
                  L"deriving the strip from a shrunken work area does move it, so the old bug is "
                  L"representable",
                  detail6);

            // The other edges are anchored the same way.
            Check(AppBarStripRect(monitor, DockEdge::Bottom, bar).bottom == 1440 &&
                      AppBarStripRect(monitor, DockEdge::Bottom, bar).top == 1440 - bar,
                  L"a bottom bar's strip is one thickness off the monitor's bottom");
            Check(AppBarStripRect(monitor, DockEdge::Left, bar).right == bar,
                  L"a left bar's strip is one thickness off the monitor's left");
            Check(AppBarStripRect(monitor, DockEdge::Right, bar).left == 2560 - bar,
                  L"a right bar's strip is one thickness off the monitor's right");

            // No edge means an empty rectangle, which the caller turns into a refusal rather than
            // handing the shell a strip spanning the whole screen.
            const RECT none = AppBarStripRect(monitor, DockEdge::None, bar);
            Check(none.right <= none.left && none.bottom <= none.top,
                  L"no edge produces an empty rectangle, which is refused");
        }

        // The landing judgement has to accept a correct reservation AND reject the walk.
        //
        // Both halves were wrong once, in opposite directions, and each produced a bar that could not
        // hold the strip it had claimed:
        //
        //  * judged RELATIVELY, the walk passes -- every step is "a thickness shorter than before";
        //  * judged against the MONITOR on the undocked edges, a correct reservation FAILS, because the
        //    work area's bottom edge already stops short of the monitor's by the shell's own strip.
        //
        // The second one is what the user hit: the strip was claimed correctly, judged wrong, and
        // released, so the island kept nothing.
        {
            const RECT monitor = {0, 0, 2560, 1440};
            // The work area as it is with the shell taskbar visible: 32 px short at the bottom.
            const RECT before = {0, 0, 2560, 1408};
            const LONG bar = 42;

            // Correct: 42 px off the top, bottom still 1408.
            const RECT good = {0, bar, 2560, 1408};
            const AppBarLanding ok = EvaluateLanding(monitor, before, good, DockEdge::Top, bar);
            wchar_t dt[192];
            std::swprintf(dt, 192, L"landed=%d otherEdges=%d sane=%d", ok.landed ? 1 : 0,
                          ok.otherEdgesIntact ? 1 : 0, ok.sane ? 1 : 0);
            Check(ok.sane && ok.landed && ok.otherEdgesIntact,
                  L"a correct reservation is accepted, even though the work area is shorter than "
                  L"the monitor",
                  dt);

            // The walk: the next step takes another thickness from the already-shortened work area.
            const RECT walked = {0, bar * 2, 2560, 1408};
            const AppBarLanding step = EvaluateLanding(monitor, good, walked, DockEdge::Top, bar);
            std::swprintf(dt, 192, L"second step landed=%d otherEdges=%d", step.landed ? 1 : 0,
                          step.otherEdgesIntact ? 1 : 0);
            Check(!step.landed,
                  L"a second thickness taken from the shrunken work area is rejected, so the walk "
                  L"cannot pass the check",
                  dt);

            // And sideways drift on an undocked edge is still caught.
            const RECT drifted = {10, bar, 2560, 1408};
            const AppBarLanding bad =
                EvaluateLanding(monitor, before, drifted, DockEdge::Top, bar);
            Check(!bad.otherEdgesIntact,
                  L"a work area that drifted sideways on an undocked edge is rejected", dt);

            // A reservation that shrank the work area to nothing is not "landed".
            const RECT inverted = {0, bar, 0, 0};
            const AppBarLanding broke =
                EvaluateLanding(monitor, before, inverted, DockEdge::Top, bar);
            Check(!broke.sane, L"an inside-out work area is not a landing", dt);
        }

        // The input region must contain everything the bar draws, and stay on the screen.
        //
        // A window region decides input at the window-manager level: clicks outside it belong to
        // whatever is underneath, with no message handling involved. That is what stops the 2560x240
        // strip across the top of the screen from swallowing clicks meant for the desktop -- the band
        // below the island that was reported as unclickable.
        //
        // The risk in the other direction is worse: a region too small makes part of the bar
        // unclickable, and a region that is empty makes all of it unclickable. So it is asserted to
        // CONTAIN the pill and the icon row rather than merely to exist.
        {
            // A monitor described here rather than taken from the live desktop, so the expected values
            // do not depend on the machine the test happens to run on.
            MonitorGeometry mon;
            mon.screen = {0, 0, 2560, 1440};
            mon.work = {0, 0, 2560, 1408};
            mon.dpi = 96;
            mon.dpiY = 96;
            // A resting bar: the region depends on the state, so the state is named.
            const IslandAnchor regionAnchor = AnchorForWindow(0.0f, kPillTopMarginDip, 2560.0f);
            const RECT region =
                IslandWindow::InputRegionFor(mon, IslandState::Collapsed, regionAnchor);

            wchar_t dt[192];
            std::swprintf(dt, 192, L"region %ld,%ld %ldx%ld on a %ld px wide screen", region.left,
                          region.top, region.right - region.left, region.bottom - region.top,
                          mon.screen.right - mon.screen.left);
            Check(region.right > region.left && region.bottom > region.top,
                  L"the input region is not empty", dt);
            Check(region.left >= 0 &&
                      region.right <= mon.screen.right - mon.screen.left,
                  L"the input region stays within the screen width", dt);

            // It must cover the widest state and the icon row, in the same coordinates the renderer
            // uses. Recomputed here from the layout rather than trusted.
            const float widthDip = PixelsToDip(mon.screen.right - mon.screen.left, mon.dpi);
            const float scale = static_cast<float>(mon.dpi) / kDipPerInch;
            const RoundedBox restingPill =
                IslandBoxFor(widthDip, kCollapsedWidthDip, kCollapsedHeightDip);
            Check(region.left <= static_cast<LONG>(restingPill.rect.left * scale) &&
                      region.right >= static_cast<LONG>(restingPill.rect.right * scale),
                  L"the input region covers the resting pill", dt);

            // The icon row, at its widest: the LAST icon that fits must be inside the region, or it
            // cannot be clicked. Checked at the far end rather than the first icon, because that is
            // where a region sized by a slightly different rule would fall short.
            const RoundedBox restIsland =
                IslandBoxFor(widthDip, kCollapsedWidthDip, kCollapsedHeightDip);
            const int fits = TaskIconsThatFit(widthDip, kMaxTaskIcons);
            const RectF lastIcon =
                TaskIconButtonAt(restIsland, fits - 1, fits, -1);
            std::swprintf(dt, 192, L"%d icons fit; the last ends at %.0f DIP = %ld px, region ends at %ld px",
                          fits, lastIcon.right, static_cast<LONG>(lastIcon.right * scale),
                          region.right);
            Check(fits > 0 && region.right >= static_cast<LONG>(lastIcon.right * scale),
                  L"the input region reaches the last icon that fits", dt);

            // The resting region is the PILL, not the card.
            //
            // This is the assertion for the band that swallowed clicks: the region used to be the whole
            // 240 DIP window, so ~200 DIP of empty-looking desktop below the pill belonged to the bar.
            // Reported with the region drawn on a screenshot, which is why the height is pinned here.
            const float pillBottomDip = kPillTopMarginDip + kPillHeightDip;
            std::swprintf(dt, 192,
                          L"resting region is %ld px tall; the pill ends at %.0f DIP and the window "
                          L"is %ld px",
                          region.bottom, pillBottomDip,
                          static_cast<LONG>(kWindowHeightDip * scale));
            Check(region.bottom <= static_cast<LONG>((pillBottomDip + 4.0f) * scale),
                  L"the resting input region is only the pill, not the window", dt);

            // Opened, it has to reach the bottom of the card -- otherwise part of the card cannot be
            // clicked, which is the same defect on the other side.
            //
            // The card's top is the RESTING pill's bottom plus the gap, which is where the renderer puts
            // it. Deriving it from the Expanded state's own box instead -- a box that is itself 250 DIP
            // tall, so its bottom IS the card's bottom -- put the top 214 DIP too low, and the region then
            // came out 42 DIP short of the card. Measured on screen with the panel open: it ended in a
            // hard straight cut through its last row, with no rounded bottom, and the settings button,
            // which the layout pins to the card's bottom, was never drawn at all.
            //
            // This assertion is the one that should have caught it. Its previous version asked only that
            // the region reached `kPillTopMarginDip + kExpandedHeightDip` -- the EXPANDED BOX's bottom,
            // which is 44 DIP above the card's -- so it passed on the broken code.
            const float cardTopDip = kReservedBarHeightDip + kCardTopGapDip;
            const float cardBottomDip = cardTopDip + kExpandedHeightDip;
            const float cardLeftDip = 853.0f;
            const RoundedBox settledExpanded = BoxForState(IslandState::Expanded, regionAnchor);
            const RECT cardRegion = IslandWindow::InputRegionFor(
                mon, IslandState::Expanded, regionAnchor, settledExpanded, cardLeftDip, 8);

            std::swprintf(dt, 192,
                          L"with the panel open the region is %ld px tall; the card is y %.0f..%.0f, "
                          L"x %.0f..%.0f",
                          cardRegion.bottom, cardTopDip, cardBottomDip, cardLeftDip,
                          cardLeftDip + kExpandedWidthDip);
            Check(cardRegion.bottom >= static_cast<LONG>((cardBottomDip + 2.0f) * scale),
                  L"the input region with the panel open reaches the bottom of the card", dt);
            Check(cardRegion.left <= static_cast<LONG>(cardLeftDip * scale) &&
                      cardRegion.right >=
                          static_cast<LONG>((cardLeftDip + kExpandedWidthDip) * scale),
                  L"the input region with the panel open covers the card's full width", dt);

            // THE SAME QUESTION FOR THE CARD AT THE OTHER END OF THE BAR, which is the case the check
            // above cannot see: its `cardLeftDip` is near the island's left, where the icon row reaches
            // past the card's right edge anyway, so the region covered the card by accident.
            //
            // The tray button is at the right end of the bar, its card is the same 520 DIP wide, and the
            // row is measured from the tray button onwards -- so with a single application window open the
            // row ends 82 DIP short of the card. Measured on screen with the tray page open: region
            // 670..1568 against a card of 1130..1650, and the card was clipped to a square right edge.
            {
                const float trayCardLeftDip = 1130.0f;
                const RECT trayRegion = IslandWindow::InputRegionFor(
                    mon, IslandState::Expanded, regionAnchor, settledExpanded, trayCardLeftDip, 1);
                std::swprintf(dt, 192,
                              L"tray card x %.0f..%.0f, region right %ld, one window open",
                              trayCardLeftDip, trayCardLeftDip + kExpandedWidthDip,
                              trayRegion.right);
                Check(trayRegion.right >=
                          static_cast<LONG>((trayCardLeftDip + kExpandedWidthDip) * scale),
                      L"the region covers the card that hangs at the tray end of the bar too", dt);
            }

            // And it must NOT move while the card opens.
            //
            // The region used to be derived from the animating box, so every frame of the transition
            // applied a new one: a window-manager call inside the animation, invisible from the outside
            // because the bar looks the same either way. Whatever changes while a card opens has to be
            // the PICTURE. A half-open card must therefore yield exactly the settled card's region.
            const RoundedBox restingBar = BoxForState(IslandState::Collapsed, regionAnchor);
            const RoundedBox halfOpen = LerpBox(restingBar, settledExpanded, 0.5f);
            const RECT halfRegion = IslandWindow::InputRegionFor(
                mon, IslandState::Expanded, regionAnchor, halfOpen, cardLeftDip, 8);
            std::swprintf(dt, 192,
                          L"half-open region [%ld,%ld %ld,%ld] vs open [%ld,%ld %ld,%ld]",
                          halfRegion.left, halfRegion.top, halfRegion.right, halfRegion.bottom,
                          cardRegion.left, cardRegion.top, cardRegion.right, cardRegion.bottom);
            Check(halfRegion.left == cardRegion.left && halfRegion.top == cardRegion.top &&
                      halfRegion.right == cardRegion.right && halfRegion.bottom == cardRegion.bottom,
                  L"the window region does not move while the card opens", dt);

            // And it is taller than the resting one, so the two states really do differ.
            Check(cardRegion.bottom > region.bottom,
                  L"the region grows when the card opens", dt);

            // And the card must be reachable where it is PAINTED.
            //
            // Painting, the window region and the pass-through test are three statements about the same
            // rectangle, and the panel has now had all three come apart. The pass-through test asks
            // "is this point the bar's or the desktop's"; it used to answer from the Expanded state's own
            // CENTRED box while the card is drawn hanging BELOW the bar with its left edge pinned to the
            // system button, so most of the panel lay outside it. Measured on a 2560-wide screen: the
            // card is x 853..1373 y 50..300, that box is x 1020..1540 y 6..256 -- so the leftmost tile
            // column, and the whole bottom band, were click-through.
            {
                const RECT windowRect = {mon.screen.left, mon.work.top, mon.screen.right,
                                         mon.work.top +
                                             static_cast<LONG>(kWindowHeightDip * scale)};
                RoundedBox paintedCard = {};
                paintedCard.rect.left   = cardLeftDip;
                paintedCard.rect.right  = cardLeftDip + kExpandedWidthDip;
                paintedCard.rect.top    = cardBottomDip - kExpandedHeightDip;
                paintedCard.rect.bottom = cardBottomDip;
                paintedCard.radiusX     = kCardRadiusDip;
                paintedCard.radiusY     = kCardRadiusDip;

                const SystemPanelLayout panel =
                    SystemPanelFor(paintedCard, 6, 1);
                bool everyTileAccepted = panel.tileCount == 6;
                for (int i = 0; i < panel.tileCount; ++i) {
                    const POINT centre = {
                        static_cast<LONG>((panel.tiles[i].left + panel.tiles[i].right) * 0.5f),
                        static_cast<LONG>((panel.tiles[i].top + panel.tiles[i].bottom) * 0.5f)};
                    if (!IslandWindow::ShouldAcceptPointer(mon, windowRect, centre,
                                                          IslandState::Expanded, 8, cardLeftDip)) {
                        everyTileAccepted = false;
                    }
                }
                std::swprintf(dt, 192,
                              L"card x %.0f..%.0f y %.0f..%.0f; first tile centre (%.0f,%.0f)",
                              paintedCard.rect.left, paintedCard.rect.right, paintedCard.rect.top,
                              paintedCard.rect.bottom, (panel.tiles[0].left + panel.tiles[0].right) * 0.5f,
                              (panel.tiles[0].top + panel.tiles[0].bottom) * 0.5f);
                Check(everyTileAccepted,
                      L"every tile's painted centre is accepted by the pass-through test", dt);

                // The state's own box does not cover the card -- which is exactly why the card has to
                // be added to the pass-through test rather than assumed to be inside it.
                const RectF& first = panel.tiles[0];
                const float cx = (first.left + first.right) * 0.5f;
                const float cy = (first.top + first.bottom) * 0.5f;
                Check(!ContainsWithTolerance(BoxForState(IslandState::Expanded, regionAnchor), cx, cy,
                                             0.0f),
                      L"the card's own tiles lie outside the state's centred box", dt);
                std::swprintf(dt, 192, L"the first tile's centre (%.0f,%.0f) reports tile %d",
                              cx, cy, SystemTileAt(paintedCard, 6, cx, cy));
                Check(SystemTileAt(paintedCard, 6, cx, cy) == 0,
                      L"a tile's painted centre reports that tile", dt);
            }

            // The region must cover the icon row at its WIDEST, not at rest.
            //
            // A button opens to 132 DIP on hover and pushes the ones after it along, so a region sized
            // from the resting 30 DIP per button is 102 DIP short -- and what falls outside the region is
            // cut by a hard edge, which is reported as an icon's title being clipped when the pointer is
            // on it. Painting, region and hit test are three statements about one rectangle; this is the
            // one that was short.
            {
                const RoundedBox rowIsland =
                    IslandBoxFor(widthDip, kCollapsedWidthDip, kCollapsedHeightDip);
                const int icons = TaskIconsThatFit(widthDip, kMaxTaskIcons);
                const RectF restingIcon = TaskIconButtonAt(rowIsland, icons - 1, icons, -1, 0.0f);
                const RectF openedIcon = TaskIconButtonAt(rowIsland, icons - 1, icons, icons - 1, 1.0f);

                const RECT wideRegion = IslandWindow::InputRegionFor(
                    mon, IslandState::Collapsed, regionAnchor, RoundedBox{}, 0.0f, icons);
                wchar_t wideDetail[224];
                std::swprintf(wideDetail, 224,
                              L"%d icons: the resting row ends at %.0f DIP, the opened one at %.0f DIP "
                              L"(%+.0f); the region ends at %.0f DIP",
                              icons, restingIcon.right, openedIcon.right,
                              openedIcon.right - restingIcon.right,
                              static_cast<float>(wideRegion.right) / scale);
                Check(openedIcon.right > restingIcon.right + 1.0f,
                      L"an opened button really is wider than a resting one", wideDetail);
                Check(wideRegion.right >= static_cast<LONG>(openedIcon.right * scale),
                      L"the input region covers the row with its last icon opened", wideDetail);

            // The card's COLUMN -- card plus the gap above it -- must be accepted as "the bar's", and must be
            // the same rectangle the policy is told about.
            //
            // The gap is the part that matters: the pointer travelling from the pill onto the card crosses 8
            // DIP of it, and a first version of this accepted only the card itself, so the pointer counted as
            // having left the bar mid-crossing and the card was put away exactly as the user moved onto it.
            // The assertion that passed had stepped from one to the other without crossing it.
            {
                const RoundedBox barForColumn =
                    IslandBoxFor(widthDip, kCollapsedWidthDip, kCollapsedHeightDip);
                const float columnLeft = 853.0f;
                const RectF column = CardColumnFor(barForColumn, columnLeft);
                const RECT windowRect = {mon.screen.left, mon.work.top, mon.screen.right,
                                         mon.work.top +
                                             static_cast<LONG>(kWindowHeightDip * scale)};
                const float centreX = (column.left + column.right) * 0.5f;
                const float gapY = (barForColumn.rect.bottom + column.top + kCardTopGapDip) * 0.5f;
                const float topOfCardY = column.top + kCardTopGapDip + 4.0f;
                const float bottomOfCardY = column.bottom - 4.0f;

                const POINT inGap = {static_cast<LONG>(centreX), static_cast<LONG>(gapY)};
                const POINT topOfCard = {static_cast<LONG>(centreX), static_cast<LONG>(topOfCardY)};
                const POINT bottomOfCard = {static_cast<LONG>(centreX),
                                            static_cast<LONG>(bottomOfCardY)};
                const POINT besideCard = {static_cast<LONG>(column.left - 20.0f),
                                          static_cast<LONG>(topOfCardY)};

                wchar_t columnDetail[224];
                std::swprintf(columnDetail, 224,
                              L"column y %.0f..%.0f; bar underside %.0f; gap point y %.0f",
                              column.top, column.bottom, barForColumn.rect.bottom, gapY);
                Check(IslandWindow::ShouldAcceptPointer(mon, windowRect, inGap, IslandState::Expanded, 8,
                                                        columnLeft),
                      L"the gap between the bar and the card counts as the bar's", columnDetail);
                Check(IslandWindow::ShouldAcceptPointer(mon, windowRect, topOfCard,
                                                        IslandState::Expanded, 8, columnLeft),
                      L"the top of the card counts as the bar's", columnDetail);
                Check(IslandWindow::ShouldAcceptPointer(mon, windowRect, bottomOfCard,
                                                        IslandState::Expanded, 8, columnLeft),
                      L"the bottom of the card counts as the bar's", columnDetail);
                Check(!IslandWindow::ShouldAcceptPointer(mon, windowRect, besideCard,
                                                         IslandState::Expanded, 8, columnLeft),
                      L"the desktop beside the card does not", columnDetail);
            }

            // Each card hangs from the thing it belongs to.
            //
            // The panel from the control that opened it; the media card and a message from the ISLAND,
            // centred under the pill. 7.76 moved all three together because the question was asked about
            // the panel, and the media card ended up under a button it has nothing to do with -- reported
            // as "this should be under the island, not under this".
            {
                const RoundedBox islandBox =
                    IslandBoxFor(widthDip, kCollapsedWidthDip, kCollapsedHeightDip);
                const float anchorCentre = (islandBox.rect.left + islandBox.rect.right) * 0.5f;
                const RectF button = SystemButtonFor(islandBox);
                const float buttonCentre = (button.left + button.right) * 0.5f;

                const float islandLeft =
                    CardLeftFor(anchorCentre - kExpandedWidthDip * 0.5f, kExpandedWidthDip, widthDip);
                const float buttonLeft =
                    CardLeftFor(buttonCentre - kExpandedWidthDip * 0.5f, kExpandedWidthDip, widthDip);

                wchar_t anchorDetail[224];
                std::swprintf(anchorDetail, 224,
                              L"island centre %.0f -> card x %.0f..%.0f; button centre %.0f -> card x "
                              L"%.0f..%.0f",
                              anchorCentre, islandLeft, islandLeft + kExpandedWidthDip, buttonCentre,
                              buttonLeft, buttonLeft + kExpandedWidthDip);
                Check(Nearly(anchorCentre, islandLeft + kExpandedWidthDip * 0.5f, 0.5f),
                      L"a card hung from the island is centred under the pill", anchorDetail);
                Check(buttonLeft < islandLeft - 1.0f,
                      L"the panel's card sits further left than the island's", anchorDetail);
            }
            }

            // The card outlives the reason it was opened by exactly the length of its exit.
            //
            // That is what makes the exit visible: the card is composed from its OWNER, and if the owner
            // were recomputed from "what is asking for a card" on every frame then a card being put away
            // would draw nothing. Reported for the panel (it vanished in one frame) and true of the other
            // two cards as well.
            {
                Check(NextCardOwner(CardOwner::Notification, /*cardUp=*/true, /*cardWanted=*/false,
                                    /*panelRequested=*/false, /*notificationActive=*/false,
                                    /*musicPresent=*/true) == CardOwner::Notification,
                      L"a card being put away keeps its owner until the shape is back");
                Check(NextCardOwner(CardOwner::SystemPanel, true, false, false, false, false) ==
                          CardOwner::SystemPanel,
                      L"a retracting panel keeps its owner with nothing asking for a card");
                Check(NextCardOwner(CardOwner::Music, true, true, true, true, true) ==
                          CardOwner::SystemPanel,
                      L"the open panel outranks everything");
                Check(NextCardOwner(CardOwner::SystemPanel, true, true, false, true, true) ==
                          CardOwner::Notification,
                      L"a message outranks what is playing");
                Check(NextCardOwner(CardOwner::Notification, false, false, false, false, true) ==
                          CardOwner::None,
                      L"with the shape back at the pill there is no card to own");
            }

        }

        // The full-screen bar spans the monitor, and is as tall as the band it fills.
        //
        // It exists because the band cannot be taken away: a screen edge holds one AppBar and the
        // shell's hidden taskbar keeps its claim, so the work area stays inset and a maximized window
        // keeps an unusable strip above it. Filling that strip is the one arrangement in which nothing
        // on screen is wasted, so the two numbers that matter are its width and its height.
        {
            MonitorGeometry mon;
            mon.screen = {0, 0, 2560, 1440};
            mon.work = {0, 32, 2560, 1440};
            mon.dpi = 96;
            mon.dpiY = 96;

            // WITH the band, the way the window builds its own anchor: the height of this state is the
            // measurement, not a constant (see the assertions below).
            const float bandDip = PixelsToDip(mon.work.top - mon.screen.top, mon.dpi);
            const IslandAnchor fullAnchor =
                AnchorForWindow(0.0f, kPillTopMarginDip, 2560.0f, bandDip);
            const RoundedBox full = BoxForState(IslandState::FullBar, fullAnchor);

            wchar_t dt[224];
            std::swprintf(dt, 224, L"full bar x [%.0f, %.0f] y [%.0f, %.0f] of 2560x1440",
                          full.rect.left, full.rect.right, full.rect.top, full.rect.bottom);
            Check(Nearly(full.rect.left, 0.0f, 0.01f) && Nearly(full.rect.right, 2560.0f, 0.01f),
                  L"the full bar spans the whole monitor width", dt);

            // Its height has to cover the band the work area leaves. Too short and a strip of the band
            // shows above the application; too tall and it covers part of the application.
            std::swprintf(dt, 224,
                          L"the band is %.0f DIP, the full bar is %.0f DIP, work starts at %.0f DIP",
                          bandDip, full.rect.bottom - full.rect.top,
                          PixelsToDip(mon.work.top, mon.dpi));
            Check(full.rect.bottom - full.rect.top >= bandDip - 0.01f,
                  L"the full bar covers the band the work area leaves", dt);

            // AND IT IS THE BAND'S HEIGHT, NOT A CONSTANT.
            //
            // The height used to be `kFullBarHeightDip` = 32, which is what THIS machine's taskbar
            // happens to be -- and the number is the user's setting (Settings > Personalisation >
            // Taskbar). Three bands are checked, none of them 32, so a return to the constant fails
            // here rather than on a machine nobody is looking at.
            for (float requested : {24.0f, 40.0f, 64.0f}) {
                const IslandAnchor bandAnchor =
                    AnchorForWindow(0.0f, kPillTopMarginDip, 2560.0f, requested);
                const RoundedBox bar = BoxForState(IslandState::FullBar, bandAnchor);
                const float height = bar.rect.bottom - bar.rect.top;
                std::swprintf(dt, 224, L"a %.0f DIP band makes a %.0f DIP bar", requested, height);
                Check(Nearly(height, requested, 0.01f),
                      L"the full bar is as tall as the band the work area leaves", dt);
            }

            // A work area that reports no band at all (a taskbar on another edge) falls back to the
            // constant rather than producing an invisible state.
            {
                const IslandAnchor noBand = AnchorForWindow(0.0f, kPillTopMarginDip, 2560.0f, 0.0f);
                const RoundedBox bar = BoxForState(IslandState::FullBar, noBand);
                const float height = bar.rect.bottom - bar.rect.top;
                std::swprintf(dt, 224, L"a 0 DIP band makes a %.0f DIP bar of %.0f", height,
                              kFullBarHeightDip);
                Check(Nearly(height, kFullBarHeightDip, 0.01f),
                      L"a machine that reports no band falls back to the constant", dt);
            }

            // And it reaches the work area's top edge, so there is no gap between the two.
            Check(full.rect.bottom >= PixelsToDip(mon.work.top, mon.dpi) - 0.01f,
                  L"the full bar meets the top of the work area", dt);

            // The input region follows it: full width, and only as tall as the bar.
            const RECT region =
                IslandWindow::InputRegionFor(mon, IslandState::FullBar, fullAnchor);
            std::swprintf(dt, 224, L"full-bar region %ld,%ld %ldx%ld", region.left, region.top,
                          region.right - region.left, region.bottom - region.top);
            Check(region.left <= 0 && region.right >= 2560,
                  L"the full-bar input region spans the screen", dt);
            Check(region.bottom <= static_cast<LONG>((kPillTopMarginDip + kFullBarHeightDip + 4.0f)),
                  L"the full-bar input region is no taller than the bar", dt);

            // The state is reachable through the policy, and left again.
            PointerContext fullState;
            fullState.shape = IslandState::Collapsed;
            fullState.resting = IslandState::Collapsed;
            fullState.fullscreenApp = true;
            Check(NextShape(fullState) == IslandState::FullBar,
                  L"a full-screen application produces the full bar", dt);

            PointerContext leaveState;
            leaveState.shape = IslandState::FullBar;
            leaveState.resting = IslandState::MusicStrip;
            leaveState.fullscreenApp = false;
            Check(NextShape(leaveState) == IslandState::MusicStrip,
                  L"the full bar gives way to the resting shape when the application stops being "
                  L"full-screen",
                  dt);

            // A CALL OR A MESSAGE STILL COMES UP OVER A FULL-SCREEN APPLICATION, and nothing else does.
            //
            // The order of those two rules was the other way round until the user reported the
            // consequence: "现在的灵动岛一行的在玩游戏的时候也显示出来了，只有通话 或者 通知的时候 灵动岛
            // 显示". Two things are asserted, because the second is the half that is easy to lose: a
            // message during a game produces the STRIP, and a full-screen application produces no card
            // even with the pointer sitting on the island and a player running -- "如果音乐的时候也显示那就
            // 鼠标放灵动岛的时候 不要 显示出音乐卡片".
            {
                PointerContext notified;
                notified.shape = IslandState::Collapsed;
                notified.resting = IslandState::MusicStrip;
                notified.fullscreenApp = true;
                notified.notificationActive = true;
                std::swprintf(dt, 224, L"full-screen + message -> %ls", StateName(NextShape(notified)));
                Check(NextShape(notified) == IslandState::NoticeStrip,
                      L"a message comes up even over a full-screen application", dt);

                PointerContext hovered;
                hovered.shape = IslandState::Collapsed;
                hovered.resting = IslandState::MusicStrip;
                hovered.fullscreenApp = true;
                hovered.pointerInside = true;
                hovered.pointerOnIsland = true;
                hovered.musicPresent = true;
                std::swprintf(dt, 224, L"full-screen + pointer on the island + a player -> %ls",
                              StateName(NextShape(hovered)));
                Check(NextShape(hovered) != IslandState::Expanded,
                      L"a full-screen application never opens the details card", dt);
            }

            // AND THE BAR IS NOT THERE TO POINT AT in that state: every point is refused, including the
            // middle of the pill, so the pointer crossing the top of the screen during a game cannot
            // open anything.
            {
                MonitorGeometry fullMon;
                fullMon.screen = {0, 0, 2560, 1440};
                fullMon.work = {0, 32, 2560, 1408};
                fullMon.dpi = 96;
                fullMon.dpiY = 96;
                const RECT windowRect = {0, 0, 2560, 320};
                const RoundedBox pill = IslandBoxFor(2560.0f, kCollapsedWidthDip, kCollapsedHeightDip);
                const POINT onThePill = {
                    static_cast<LONG>((pill.rect.left + pill.rect.right) * 0.5f),
                    static_cast<LONG>((pill.rect.top + pill.rect.bottom) * 0.5f)};
                std::swprintf(dt, 224, L"pill centre %ld,%ld", onThePill.x, onThePill.y);
                Check(!IslandWindow::ShouldAcceptPointer(fullMon, windowRect, onThePill,
                                                        IslandState::FullBar, 8, 0.0f),
                      L"a full-screen application leaves the bar accepting nothing", dt);
                Check(IslandWindow::ShouldAcceptPointer(fullMon, windowRect, onThePill,
                                                        IslandState::Collapsed, 8, 0.0f),
                      L"and the same point is the island's once the application is not full-screen", dt);

                // ... AND NOTHING ELSE IS, WHILE IT IS: the icon row and the deck are not drawn, so the
                // pointer must not be able to land on them either. "左右两侧的不用显示" is about what is on
                // screen; this is the half that decides whether anything still ANSWERS there.
                const RoundedBox strip = BoxForState(IslandState::NoticeStrip,
                                                    AnchorForWindow(0.0f, kPillTopMarginDip, 2560.0f));
                const RectF firstIcon = TaskIconButtonAt(strip, 0, 4, -1);
                const RectF startButton = StartButtonFor(strip);
                const POINT onTheFirstIcon = {
                    static_cast<LONG>((firstIcon.left + firstIcon.right) * 0.5f),
                    static_cast<LONG>((firstIcon.top + firstIcon.bottom) * 0.5f)};
                const POINT onTheStartButton = {
                    static_cast<LONG>((startButton.left + startButton.right) * 0.5f),
                    static_cast<LONG>((startButton.top + startButton.bottom) * 0.5f)};
                const POINT onTheStrip = {
                    static_cast<LONG>((strip.rect.left + strip.rect.right) * 0.5f),
                    static_cast<LONG>((strip.rect.top + strip.rect.bottom) * 0.5f)};

                std::swprintf(dt, 224, L"first icon %ld,%ld, Start %ld,%ld, strip %ld,%ld",
                              onTheFirstIcon.x, onTheFirstIcon.y, onTheStartButton.x,
                              onTheStartButton.y, onTheStrip.x, onTheStrip.y);
                Check(IslandWindow::ShouldAcceptPointer(fullMon, windowRect, onTheStrip,
                                                        IslandState::NoticeStrip, 8, 0.0f, true),
                      L"a message over a full-screen application leaves the island clickable", dt);
                Check(!IslandWindow::ShouldAcceptPointer(fullMon, windowRect, onTheFirstIcon,
                                                         IslandState::NoticeStrip, 8, 0.0f, true) &&
                          !IslandWindow::ShouldAcceptPointer(fullMon, windowRect, onTheStartButton,
                                                             IslandState::NoticeStrip, 8, 0.0f, true),
                      L"but neither the icon row nor the Start button answers", dt);
                Check(IslandWindow::ShouldAcceptPointer(fullMon, windowRect, onTheFirstIcon,
                                                        IslandState::NoticeStrip, 8, 0.0f, false),
                      L"and both answer again as soon as the application is not full-screen", dt);
            }

            // The rectangle test that decides whether an application has the screen. The near miss is the
            // case that matters: a MAXIMIZED window covers the work area, which starts below the band, so
            // its top edge is 32 px down the screen and it must not count.
            {
                const RECT monitor = {0, 0, 2560, 1440};
                const RECT maximized = {0, 32, 2560, 1440};
                const RECT game = {0, 0, 2560, 1440};
                const RECT borderless = {-2, -2, 2562, 1442};
                const RECT tallWindow = {100, 0, 2460, 1440};
                std::swprintf(dt, 224, L"maximized %ld,%ld %ldx%ld against the monitor 0,0 2560x1440",
                              maximized.left, maximized.top, maximized.right - maximized.left,
                              maximized.bottom - maximized.top);
                Check(!WindowCoversMonitor(maximized, monitor),
                      L"a maximized window does NOT count as full-screen", dt);
                Check(WindowCoversMonitor(game, monitor) && WindowCoversMonitor(borderless, monitor),
                      L"an application covering the monitor does", dt);
                Check(!WindowCoversMonitor(tallWindow, monitor),
                      L"a window that is merely tall does not", dt);
            }

            // The strip released: the same question, one state later.
            //
            // Once the bar gives the work area back (NativeTaskbar::ReleaseWorkArea) a maximized window
            // covers the monitor as well, so "does it cover the monitor" stops being the test -- and the
            // obvious replacement, "is it zoomed", is wrong. Both of those are measured on this desktop
            // at 100%, and the numbers below are the measurements:
            //
            //     work area with the strip   0,32-2560,1440    a maximized window is  -8,24-2568,1448
            //     work area released           0,0-2560,1440   a maximized window is  -8,-8-2568,1448
            //     a maximized window made borderless over the screen:  0,0-2560,1440, and STILL zoomed
            //
            // The last line is what makes the rectangle the test rather than IsZoomed: the geometry of a
            // window that went full-screen from a maximized state is EXACTLY that of a game.
            {
                const RECT monitor = {0, 0, 2560, 1440};
                const RECT workWithStrip = {0, 32, 2560, 1440};
                const RECT workReleased = {0, 0, 2560, 1440};
                const RECT maximizedBehindStrip = {-8, 24, 2568, 1448};
                const RECT maximizedAfterRelease = {-8, -8, 2568, 1448};
                const RECT borderlessFullScreen = {0, 0, 2560, 1440};

                // `rdt`, not `dt`: the block above already has a `dt` in scope, and a shadowed one is a
                // warning this project does not carry (the compiler saw the first attempt).
                wchar_t rdt[224];
                const BorderInflation border = MaximizedBorderPx(96);
                std::swprintf(rdt, 224, L"border %ld x %ld px at 96 dpi", border.x, border.y);
                Check(border.x == 8 && border.y == 8,
                      L"the invisible resize border is 8 px at 100%, which the rest of this rests on",
                      rdt);

                std::swprintf(rdt, 224,
                              L"work %ld,%ld-%ld,%ld -> maximized %ld,%ld-%ld,%ld",
                              workWithStrip.left, workWithStrip.top, workWithStrip.right,
                              workWithStrip.bottom, maximizedBehindStrip.left, maximizedBehindStrip.top,
                              maximizedBehindStrip.right, maximizedBehindStrip.bottom);
                Check(LooksLikeWindowsMaximizedRect(maximizedBehindStrip, workWithStrip, 96),
                      L"a maximized window is the work area inflated by the resize border", rdt);

                Check(LooksLikeWindowsMaximizedRect(maximizedAfterRelease, workReleased, 96),
                      L"... and the same holds after the strip is released",
                      L"released work 0,0-2560,1440, maximized -8,-8-2568,1448");

                Check(!LooksLikeWindowsMaximizedRect(borderlessFullScreen, workReleased, 96),
                      L"a window that covers the monitor exactly is NOT a maximized one",
                      L"it is what a game, or a player that dropped its frame, reports");

                // The two questions must disagree on the released state, or the bar would stand down --
                // island only, no deck, no tray -- on every ordinary maximized window, which is the
                // state the user is in whenever a browser is maximized.
                Check(WindowCoversMonitor(maximizedAfterRelease, monitor) &&
                      WindowCoversMonitor(borderlessFullScreen, monitor) &&
                      LooksLikeWindowsMaximizedRect(maximizedAfterRelease, workReleased, 96) &&
                      !LooksLikeWindowsMaximizedRect(borderlessFullScreen, workReleased, 96),
                      L"covering the monitor and being a maximized window are two different questions",
                      L"both cover it; only one is a maximized window");
            }
        }

        // The Start button mirrors the icon row, and is inside the input region.
        //
        // Across the island from the icons, the same size as a resting one, and -- the part that is easy
        // to get wrong and impossible to see -- INSIDE the window region. A button outside its own
        // window's region cannot be clicked at all, and nothing about the drawing would look wrong.
        {
            MonitorGeometry startMon;
            startMon.screen = {0, 0, 2560, 1440};
            startMon.work = {0, 0, 2560, 1408};
            startMon.dpi = 96;
            startMon.dpiY = 96;

            const IslandAnchor startAnchor = AnchorForWindow(0.0f, kPillTopMarginDip, 2560.0f);
            const RoundedBox restIsland =
                IslandBoxFor(2560.0f, kCollapsedWidthDip, kCollapsedHeightDip);
            const RectF start = StartButtonFor(restIsland);
            const RectF firstIcon = TaskIconButtonAt(restIsland, 0, 4, -1);

            wchar_t dt[224];
            std::swprintf(dt, 224,
                          L"start x [%.0f, %.0f], island x [%.0f, %.0f], first icon x [%.0f, %.0f]",
                          start.left, start.right, restIsland.rect.left, restIsland.rect.right,
                          firstIcon.left, firstIcon.right);
            Check(start.right <= restIsland.rect.left,
                  L"the Start button is to the left of the island, not over it", dt);
            Check(firstIcon.left >= restIsland.rect.right,
                  L"the icons are to the right of the island", dt);
            Check(Nearly(start.right - start.left, kStartButtonDip, 0.01f),
                  L"the Start button is the same size as a resting icon button", dt);

            // Vertically centred on the island, like the row.
            const float startCentreY = (restIsland.rect.top + restIsland.rect.bottom) * 0.5f;
            Check(Nearly((start.top + start.bottom) * 0.5f, startCentreY, 0.01f),
                  L"the Start button is centred on the island vertically", dt);

            // Hit testing agrees with the rectangle, and nothing just outside it does.
            Check(StartButtonHit(restIsland, (start.left + start.right) * 0.5f,
                                 (start.top + start.bottom) * 0.5f),
                  L"a point in the middle of the Start button hits it", dt);
            Check(!StartButtonHit(restIsland, start.left - 2.0f, startCentreY),
                  L"a point to the left of the Start button misses it", dt);
            Check(!StartButtonHit(restIsland, restIsland.rect.left + 4.0f, startCentreY),
                  L"a point on the island is not the Start button", dt);

            // And it is INSIDE the window region -- the check that catches an unclickable button.
            const RECT startRegion =
                IslandWindow::InputRegionFor(startMon, IslandState::Collapsed, startAnchor);
            std::swprintf(dt, 224, L"start [%.0f, %.0f] DIP = [%ld, %ld] px; region [%ld, %ld] px",
                          start.left, start.right, static_cast<LONG>(start.left),
                          static_cast<LONG>(start.right), startRegion.left, startRegion.right);
            Check(startRegion.left <= static_cast<LONG>(start.left) &&
                      startRegion.right >= static_cast<LONG>(start.right),
                  L"the input region reaches the Start button, or it could never be clicked", dt);
        }

        // No island state may reach the Start button.
        //
        // Every state grows symmetrically about the centre, so a state wider than the resting pill moves
        // its LEFT edge outwards -- straight towards the button, which sits one padding away from it. The
        // 520 DIP lyric strip began at 1020 while the button occupied 1132..1162, so the strip covered
        // the button completely. Reported with a screenshot of lyrics and no Start button.
        //
        // Asserted across EVERY state rather than the one that was wrong, because the next state added
        // wider than the pill would do the same thing, and nothing else in the code would notice.
        {
            const MonitorGeometry mon96 = [] {
                MonitorGeometry m;
                m.screen = {0, 0, 2560, 1440};
                m.work = {0, 0, 2560, 1408};
                m.dpi = 96;
                m.dpiY = 96;
                return m;
            }();
            const IslandAnchor a = AnchorForWindow(0.0f, kPillTopMarginDip, 2560.0f);

            const IslandState states[] = {IslandState::Collapsed, IslandState::Hover,
                                          IslandState::Expanded, IslandState::MusicStrip,
                                          IslandState::FullBar};
            const wchar_t* names[] = {L"collapsed", L"hover", L"expanded", L"music-strip",
                                      L"full-bar"};

            const RectF button = StartButtonFor(BoxForState(IslandState::Collapsed, a));
            for (int i = 0; i < 5; ++i) {
                const RoundedBox box = BoxForState(states[i], a);
                wchar_t dt2[224];
                std::swprintf(dt2, 224,
                              L"%ls: island x [%.0f, %.0f], start button x [%.0f, %.0f]",
                              names[i], box.rect.left, box.rect.right, button.left, button.right);
                // Reported as information, not asserted, while the collision is being worked out: the
                // card and the full bar legitimately span the screen, and the Start button is drawn ON
                // TOP of them, so overlapping is not by itself a defect. What matters is whether the
                // button is visible, which is a question about draw order, not geometry.
                Info(L"island vs start button", dt2);
            }
            Check(button.right <= BoxForState(IslandState::Collapsed, a).rect.left,
                  L"the Start button is left of the resting island", nullptr);
            (void)mon96;
        }

        // The island has ONE shape, and only the card and the full-screen bar differ.
        //
        // This is the user's decision written as an assertion: "the island does not need to grow
        // sideways any more -- take that animation out". Two things follow, and both were defects
        // before:
        //
        //  * the outline no longer moves as the pointer crosses the bar or as music starts, so the
        //    transition between those states is zero distance -- there is nothing to animate;
        //  * nothing is wider than the resting pill, so nothing reaches LEFT towards the Start button.
        //    The lyric strip at 520 DIP began at 1020 while the button occupied 1132..1162, which is
        //    how it came to be hidden.
        //
        // Asserted on the settled shapes, so a future state that reintroduces a width is caught here
        // rather than on screen.
        {
            const IslandAnchor a = AnchorForWindow(0.0f, kPillTopMarginDip, 2560.0f);

            const IslandState same[] = {IslandState::Collapsed, IslandState::Hover,
                                        IslandState::MusicStrip};
            const wchar_t* sameNames[] = {L"collapsed", L"hover", L"music-strip"};
            const RoundedBox pill = BoxForState(IslandState::Collapsed, a);

            bool anyDiffer = false;
            wchar_t dt2[224];
            for (int i = 0; i < 3; ++i) {
                const RoundedBox box = BoxForState(same[i], a);
                if (!Nearly(box.rect.left, pill.rect.left, 0.01f) ||
                    !Nearly(box.rect.right, pill.rect.right, 0.01f) ||
                    !Nearly(box.rect.top, pill.rect.top, 0.01f) ||
                    !Nearly(box.rect.bottom, pill.rect.bottom, 0.01f)) {
                    anyDiffer = true;
                }
            }
            std::swprintf(dt2, 224, L"pill x [%.0f, %.0f] y [%.0f, %.0f]", pill.rect.left,
                          pill.rect.right, pill.rect.top, pill.rect.bottom);
            Check(!anyDiffer,
                  L"collapsed, hover and music are all the same shape as the pill", dt2);

            // The card is a different KIND of thing and is allowed to differ -- but it must not reach
            // the Start button either, now that nothing else does.
            const RectF button = StartButtonFor(pill);
            const RoundedBox wideCard = BoxForState(IslandState::Expanded, a);
            std::swprintf(dt2, 224, L"card x [%.0f, %.0f], start button x [%.0f, %.0f]",
                          wideCard.rect.left, wideCard.rect.right, button.left, button.right);
            Info(L"card vs start button", dt2);

            // And the transition between two same-shaped states is instant, because there is no
            // distance to cover: a state change must not start an animation at all.
            Motion m;
            m.SetAnchor(a);
            m.SnapTo(IslandState::Collapsed);
            Check(!m.SetTarget(IslandState::Hover),
                  L"switching between two same-shaped states starts no transition", dt2);
            Check(!m.SetTarget(IslandState::MusicStrip),
                  L"and neither does switching to the lyric strip", dt2);
        }

        // The bar's left deck, and the island beside it.
        //
        //   [ system ][ Start ]   island   [ application icons ... ]
        //
        // The deck is anchored to the ISLAND's left edge, not to the window's margin, so the two
        // buttons sit against the thing they belong to instead of floating in an empty strip. The
        // system button is leftmost and the Start button is between it and the island.
        //
        // The system indicators used to be checked here as well. They were removed from the bar, and
        // their assertions went with them -- an assertion about geometry that is no longer drawn is a
        // test that can only pass.
        {
            const float deckMonitorWidth = 2560.0f;
            const RoundedBox pill =
                IslandBoxFor(deckMonitorWidth, kCollapsedWidthDip, kCollapsedHeightDip);

            const RectF startBox = StartButtonFor(pill);
            const RectF systemBox = SystemButtonFor(pill);

            wchar_t dt4[256];
            std::swprintf(dt4, 256,
                          L"system [%.0f,%.0f], start [%.0f,%.0f], island left %.0f",
                          systemBox.left, systemBox.right, startBox.left, startBox.right,
                          pill.rect.left);
            Check(systemBox.right < startBox.left,
                  L"the system button is left of the Start button", dt4);
            Check(startBox.right <= pill.rect.left + 0.01f,
                  L"the Start button does not overlap the island", dt4);
            Check(Nearly(startBox.right, pill.rect.left - kTaskIconPadDip, 0.01f),
                  L"the Start button sits one pad from the island's left edge", dt4);
            Check(Nearly(systemBox.right - systemBox.left, kStartButtonDip, 0.01f),
                  L"the system button is the same size as the Start button", dt4);
            Check(systemBox.left > 0.0f,
                  L"the deck's left edge is on the screen", dt4);

            // The two buttons are on the same line as the island, and the deck is to its LEFT. The
            // deck's position must not depend on how many application icons exist -- that is the whole
            // reason it is anchored here rather than to the growing row.
            const float midY = (pill.rect.top + pill.rect.bottom) * 0.5f;
            Check(startBox.top < midY && startBox.bottom > midY,
                  L"the Start button is vertically centred on the island", dt4);
            Check(systemBox.top < midY && systemBox.bottom > midY,
                  L"the system button is vertically centred on the island", dt4);
            Check(StartButtonHit(pill, (startBox.left + startBox.right) * 0.5f, midY),
                  L"a point in the Start button hits it", dt4);
            Check(!StartButtonHit(pill, (systemBox.left + systemBox.right) * 0.5f, midY),
                  L"a point in the system button is not the Start button", dt4);
            Check(SystemButtonHit(pill, (systemBox.left + systemBox.right) * 0.5f, midY),
                  L"a point in the system button hits it", dt4);
            Check(!SystemButtonHit(pill, (startBox.left + startBox.right) * 0.5f, midY),
                  L"a point in the Start button is not the system button", dt4);

            // The deck must not swallow the island's own clickable area, whatever the monitor width.
            for (float width : {800.0f, 1280.0f, 1920.0f, 2560.0f, 3840.0f}) {
                const RoundedBox box = IslandBoxFor(width, kCollapsedWidthDip, kCollapsedHeightDip);
                std::swprintf(dt4, 256, L"on a %.0f px screen the deck ends at %.0f, island at %.0f",
                              width, SystemButtonFor(box).left, box.rect.left);
                Check(SystemButtonFor(box).left > 0.0f,
                      L"the deck starts on the screen at every width", dt4);
            }

            // The icon row starts to the right of the island and is unaffected by the deck.
            const float deckRowLeft = TaskRowFor(pill, 1).left;
            std::swprintf(dt4, 256, L"island right %.0f, icon row starts %.0f", pill.rect.right,
                          deckRowLeft);
            Check(deckRowLeft > pill.rect.right,
                  L"the icon row starts to the right of the island", dt4);
        }

        // The date, the weekday, the time and the weather: one line, two homes.
        //
        // WHY IT EXISTS. The island's own line holds one thing -- the clock at rest, and the lyric or
        // the message when there is one -- so the date, the weekday and the weather had nowhere to be
        // at all, and while the island was busy the time had nowhere either. Asked for in two steps:
        // the band left of the system button while the island is occupied ("当放音乐或者灵动岛被占用的
        // 时候 就在我红框处显示日期时间星期天气"), and the ISLAND ITSELF at rest ("是灵动岛不占用时 灵动
        // 岛内要这么显示").
        //
        // The assertions are the things a photograph cannot say: that the line is placed in exactly one
        // of the two homes, that the strip cannot collide with the deck or leave the screen, that the
        // LONGEST line it can be asked to print fits BOTH boxes, and that the input region covers the
        // band -- the region is what decides which pixels this window owns, so a region that stopped at
        // the deck would clip the strip's left end, on screen, with nothing in the log to explain it.
        {
            const float stripMonitorWidth = 2560.0f;
            const RoundedBox stripIsland =
                IslandBoxFor(stripMonitorWidth, kCollapsedWidthDip, kCollapsedHeightDip);
            const RoundedBox stripBox = StatusStripFor(stripIsland);
            const RectF& box = stripBox.rect;
            const RectF systemButton = SystemButtonFor(stripIsland);
            const float midY = (stripIsland.rect.top + stripIsland.rect.bottom) * 0.5f;

            wchar_t stripDetail[256];
            std::swprintf(stripDetail, 256,
                          L"strip [%.0f..%.0f] y [%.0f..%.0f], system button [%.0f..%.0f], island "
                          L"left %.0f",
                          box.left, box.right, box.top, box.bottom, systemButton.left,
                          systemButton.right, stripIsland.rect.left);
            Check(box.right <= systemButton.left - 1.0f,
                  L"the status strip ends clear of the system button", stripDetail);
            Check(box.left > 0.0f, L"the status strip starts on the screen", stripDetail);
            Check(box.right < stripIsland.rect.left,
                  L"the status strip stands left of the island", stripDetail);
            Check(Nearly(box.right - box.left, kStatusStripWidthDip, 0.01f),
                  L"the status strip is the width its own constant says", stripDetail);
            Check(box.top < midY && box.bottom > midY,
                  L"the status strip is vertically centred on the island", stripDetail);
            Check(box.top >= 0.0f && box.bottom <= kWindowHeightDip,
                  L"the status strip is inside the window", stripDetail);

            // EXACTLY ONE HOME, which is the rule both draw sites obey.
            //
            // While the pill's line is free the words are in the island; while it is taken -- a call, a
            // message, a lyric -- the same words are beside the deck. Never both (two clocks on one bar)
            // and never neither (the date and the weather were nowhere at all before this). The rule is
            // written as a pair, and called by the frame loop and by nothing else, because neither draw
            // site can check it on its own: each knows only its own half.
            {
                const StatusPlacement idle = StatusPlacementFor(false);
                const StatusPlacement busy = StatusPlacementFor(true);
                Check(idle.inIsland && !idle.besideDeck,
                      L"an idle island keeps the date and the time inside the pill");
                Check(!busy.inIsland && busy.besideDeck,
                      L"a busy island shows them beside the deck instead");
                const int idleHomes = (idle.inIsland ? 1 : 0) + (idle.besideDeck ? 1 : 0);
                const int busyHomes = (busy.inIsland ? 1 : 0) + (busy.besideDeck ? 1 : 0);
                wchar_t homesDetail[160];
                std::swprintf(homesDetail, 160,
                              L"an idle island draws it in %d place(s), a busy one in %d",
                              idleHomes, busyHomes);
                Check(idleHomes == 1 && busyHomes == 1,
                      L"the line has exactly one home in both cases", homesDetail);
            }

            // The fade. "It pops in" and "it fades in" are the same picture once the fade is over,
            // so the arithmetic is the only place the difference can be seen: it has to move by the
            // elapsed time, arrive, stop at its ends, and take the same time in both directions.
            {
                float amount = 0.0f;
                int frames = 0;
                while (amount < 1.0f && frames < 1000) {
                    amount = StatusStripFadeStep(amount, true, 1.0f / 60.0f);
                    ++frames;
                }
                const float appearSeconds = static_cast<float>(frames) / 60.0f;
                int backFrames = 0;
                while (amount > 0.0f && backFrames < 1000) {
                    amount = StatusStripFadeStep(amount, false, 1.0f / 60.0f);
                    ++backFrames;
                }
                const float leaveSeconds = static_cast<float>(backFrames) / 60.0f;

                wchar_t fadeDetail[224];
                std::swprintf(fadeDetail, 224,
                              L"%.0f ms in over %d frames, %.0f ms out over %d",
                              appearSeconds * 1000.0f, frames, leaveSeconds * 1000.0f, backFrames);
                Check(appearSeconds > 0.08f && appearSeconds < 0.45f,
                      L"the status strip fades in rather than appearing", fadeDetail);
                Check(Nearly(appearSeconds, leaveSeconds, 0.02f),
                      L"the strip takes the same time to come and to go", fadeDetail);
                Check(Nearly(StatusStripFadeStep(0.5f, true, 0.05f), 0.75f, 0.001f) &&
                          Nearly(StatusStripFadeStep(0.5f, false, 0.05f), 0.25f, 0.001f),
                      L"one step moves the strip's fade by the elapsed time", fadeDetail);
                Check(Nearly(StatusStripFadeStep(1.0f, true, 0.5f), 1.0f, 0.001f) &&
                          Nearly(StatusStripFadeStep(0.0f, false, 0.5f), 0.0f, 0.001f),
                      L"the fade stops at its ends", fadeDetail);
                Check(Nearly(StatusStripFadeStep(0.0f, true, 0.0f), 0.0f, 0.001f),
                      L"a frame of no length moves the fade nowhere", fadeDetail);
            }

            // The run is a composition, and BOTH of the boxes it is drawn in have to hold it.
            //
            // THE BOX IS A FIXED WIDTH and the run is centred in it, so a run that outgrew its box
            // would lose a piece off each end -- and it would lose them at the moment the weather
            // reading arrived, which is the last moment anybody is looking for a rendering fault.
            // Measured rather than eyeballed: the width of a piece is a property of the face and the
            // text, and both can change under this code.
            //
            // The island is the tighter of the two boxes and the one that sets the sizes: the band is
            // 268 wide (252 usable) and the pill is 220 (204 usable), and the SAME run is drawn in
            // both -- there is no second form to fall back to. That is only true because the calendar
            // glyph at the head of the run was removed on the user's word; with it the run was
            // 235.6 DIP and the pill could not hold it (HANDOFF 8.48, 8.49).
            {
                TextRenderer runText;
                if (runText.Initialize()) {
                    // The widest run that can occur: the longest weekday and month, a three-character
                    // weather word, and a negative two-digit temperature.
                    const wchar_t* weekday = L"\u5468\u4e09";              // 周三
                    const wchar_t* day = L"12\u670828\u65e5";              // 12月28日
                    const wchar_t* time = L"23:59";
                    const wchar_t* sky = L"\u5f3a\u9635\u96e8";            // 强阵雨
                    const wchar_t* temperature = L"-12\u00b0";

                    const float widest =
                        runText.MeasureStatusLine(weekday, day, time, sky, temperature, 0);
                    const float bandAvailable = kStatusStripWidthDip - kStatusStripPadDip * 2.0f;
                    const float pillAvailable = kCollapsedWidthDip - kStatusStripPadDip * 2.0f;

                    wchar_t fitDetail[256];
                    std::swprintf(fitDetail, 256,
                                  L"widest run %.1f DIP; pill %.1f available, band %.1f",
                                  widest, pillAvailable, bandAvailable);
                    Check(pillAvailable < bandAvailable,
                          L"the island is the tighter of the two boxes", fitDetail);
                    Check(widest > 0.0f && widest <= pillAvailable,
                          L"the widest run fits inside the island itself", fitDetail);
                    Check(widest > 0.0f && widest <= bandAvailable,
                          L"the same run fits the band beside the deck", fitDetail);

                    // A reading that is missing takes its word with it: with no sky the run is
                    // shorter, which is what a machine with no position and no city shows.
                    const float noReading =
                        runText.MeasureStatusLine(weekday, day, time, nullptr, nullptr, -1);
                    std::swprintf(fitDetail, 256, L"with a reading %.1f, without %.1f DIP", widest,
                                  noReading);
                    Check(noReading > 0.0f && noReading <= pillAvailable && noReading < widest,
                          L"a line with no reading is shorter and still fits", fitDetail);
                }
            }

            // The input region covers the band the strip stands in -- and only while it is asked for.
            //
            // A region wider than the drawing costs nothing: a point inside it but off the bar is
            // answered HTTRANSPARENT by WM_NCHITTEST, so the click passes through to whatever is
            // underneath. A region narrower than the drawing is the defect, and it is invisible in
            // every assertion about the strip's own geometry.
            {
                MonitorGeometry stripMonitor;
                stripMonitor.screen = {0, 0, 2560, 1440};
                stripMonitor.work = {0, 0, 2560, 1408};
                stripMonitor.dpi = 96;
                stripMonitor.dpiY = 96;
                const IslandAnchor stripAnchor =
                    AnchorForWindow(0.0f, kPillTopMarginDip, 2560.0f);

                const RECT withStrip = IslandWindow::InputRegionFor(
                    stripMonitor, IslandState::Collapsed, stripAnchor, RoundedBox{}, 0.0f, 8,
                    kStatusStripWidthDip);
                const RECT withoutStrip = IslandWindow::InputRegionFor(
                    stripMonitor, IslandState::Collapsed, stripAnchor, RoundedBox{}, 0.0f, 8, 0.0f);

                wchar_t regionDetail[256];
                std::swprintf(regionDetail, 256,
                              L"with the strip the region starts at %ld px, without it at %ld px; "
                              L"the strip's box starts at %.0f DIP",
                              withStrip.left, withoutStrip.left, box.left);
                Check(withoutStrip.left > withStrip.left,
                      L"the strip widens the input region to cover its band", regionDetail);
                Check(static_cast<float>(withStrip.left) <= box.left + 1.0f,
                      L"the region covers the strip's own left edge", regionDetail);
                Check(withStrip.bottom == withoutStrip.bottom,
                      L"the strip does not make the region taller", regionDetail);
            }

            // THE DECK AND THE ROW'S RIGHT END FOLLOW THE NOTICE STRIP, IN THE REGION AND IN THE HIT TEST.
            //
            // These are the two assertions that would have caught the defect reported with a photograph of
            // the bar during a call: "通话或者通知的时候左侧是开始菜单和系统按钮点不了，右侧还有少一部分".
            //
            // Both halves were the same mistake in two places. The hit test asked for the controls from the
            // RESTING PILL while the drawing placed them from the strip, so the Start button and the panel
            // toggle were painted 100 DIP to the left of where the pointer was accepted. And the region's
            // RIGHT edge was measured from the resting pill while its LEFT edge followed the strip, so the
            // region stopped short of the row it is supposed to cover -- and the region clips PAINTING as
            // well as input, which is why the right-hand end of the row was not on screen at all.
            //
            // Written against the row the renderer lays out rather than against the region's own inputs,
            // because the property is "the region covers what is drawn" and not "these two numbers are
            // equal" -- and the two numbers being equal is exactly what was once true and wrong.
            {
                MonitorGeometry stripMonitor;
                stripMonitor.screen = {0, 0, 2560, 1440};
                stripMonitor.work = {0, 0, 2560, 1408};
                stripMonitor.dpi = 96;
                stripMonitor.dpiY = 96;
                // Screen-sized window rect and 96 dpi, so one DIP is one pixel and the two sides of each
                // comparison below are in the same units.
                const RECT windowRect = {0, 0, 2560, 360};
                const IslandAnchor stripAnchor = AnchorForWindow(0.0f, kPillTopMarginDip, 2560.0f);
                const RoundedBox restingPill = BoxForState(IslandState::Collapsed, stripAnchor);
                const RoundedBox noticeStrip = BoxForState(IslandState::NoticeStrip, stripAnchor);

                constexpr int kIcons = 8;
                constexpr int kTrayIcons = 15;
                const RECT region = IslandWindow::InputRegionFor(
                    stripMonitor, IslandState::NoticeStrip, stripAnchor, noticeStrip, 0.0f, kIcons,
                    0.0f, kTrayIcons);

                // The row exactly as the renderer lays it out with a strip up: from the strip's own edge,
                // pushed one button along by the tray button.
                const RoundedBox rowWithTray =
                    RowAnchorWithTray(RowAnchorBox(noticeStrip, restingPill), kTrayIcons > 0);
                const float rowRightDip =
                    TaskIconButtonAt(rowWithTray, kIcons - 1, kIcons, kIcons - 1, 1.0f).right;

                // `stripRowDetail`, not `stripDetail`: the latter is declared earlier in this same check and
                // hiding it was a warning (C4456) that only showed up when this file was rebuilt.
                wchar_t stripRowDetail[256];
                std::swprintf(stripRowDetail, 256,
                              L"row ends at %.0f DIP, region ends at %ld px (strip right %.0f)", rowRightDip,
                              region.right, noticeStrip.rect.right);
                Check(static_cast<float>(region.right) >= rowRightDip - 1.0f,
                      L"the region covers the right end of the icon row while the notice strip is up",
                      stripRowDetail);

                // The deck, from the pointer's side: the Start button is placed from the box the row is
                // anchored to, so a strip moves it -- and a point on it, as drawn, must be the bar's. The
                // same point is then asked again with the resting pill standing in for the strip, and the
                // answer must be the opposite: that IS the defect, stated as a predicate rather than as two
                // coordinates.
                const RectF startOnStrip = StartButtonFor(RowAnchorBox(noticeStrip, restingPill));
                const RectF startOnPill = StartButtonFor(restingPill);
                const POINT onStart = {
                    static_cast<LONG>((startOnStrip.left + startOnStrip.right) * 0.5f),
                    static_cast<LONG>((startOnStrip.top + startOnStrip.bottom) * 0.5f)};
                std::swprintf(stripDetail, 256, L"start drawn %.0f..%.0f, from the pill %.0f..%.0f DIP",
                              startOnStrip.left, startOnStrip.right, startOnPill.left, startOnPill.right);
                Check(IslandWindow::ShouldAcceptPointer(stripMonitor, windowRect, onStart,
                                                        IslandState::NoticeStrip, kIcons, 0.0f, false,
                                                        kTrayIcons, noticeStrip),
                      L"the Start button is the bar's where the notice strip draws it", stripDetail);
                Check(!IslandWindow::ShouldAcceptPointer(stripMonitor, windowRect, onStart,
                                                         IslandState::NoticeStrip, kIcons, 0.0f, false,
                                                         kTrayIcons, restingPill),
                      L"... and it is not the bar's where the resting pill would put it", stripDetail);
            }
        }

        // The weather's own arithmetic: the code table, the rounding, and the two wire formats.
        //
        // All three are pure functions of text and numbers, and all three fail QUIETLY. A code that
        // fell out of the table shows a temperature with no word; a rounding that went the wrong way
        // is one degree out for the rest of the day; a parser that reads the wrong field prints the
        // weather code as the temperature. The two documents below are COPIES OF REAL ANSWERS from the
        // two services (Beijing, 2026-10-12), kept verbatim so the parsers are tested against the
        // format they actually receive rather than the one they were written expecting.
        {
            Check(WeatherWordForCode(0) != nullptr &&
                      std::wcscmp(WeatherWordForCode(0), L"\u6674") == 0,
                  L"code 0 reads as clear");
            Check(WeatherWordForCode(95) != nullptr &&
                      std::wcscmp(WeatherWordForCode(95), L"\u96f7\u9635\u96e8") == 0,
                  L"a thunderstorm reads as a thunderstorm");
            Check(WeatherWordForCode(-1) == nullptr && WeatherWordForCode(1000) == nullptr,
                  L"a code the table does not know has no word");

            {
                wchar_t weatherLine[64] = {};
                WeatherLineFor(19.4, 3, weatherLine, 64);
                Check(std::wcscmp(weatherLine, L"\u9634 19\u00b0") == 0,
                      L"a reading is the word and the rounded temperature", weatherLine);
                WeatherLineFor(-12.6, 999, weatherLine, 64);
                Check(std::wcscmp(weatherLine, L"-13\u00b0") == 0,
                      L"an unknown code leaves the temperature alone", weatherLine);
                WeatherLineFor(19.5, 0, weatherLine, 64);
                Check(std::wcscmp(weatherLine, L"\u6674 20\u00b0") == 0,
                      L"half a degree rounds up rather than down", weatherLine);
            }

            // The forecast answer, as Open-Meteo sends it: a header block with the coordinates, a
            // blank line, a header naming the columns, and the reading LAST.
            const char* forecast =
                "latitude,longitude,elevation,utc_offset_seconds,timezone,timezone_abbreviation\n"
                "39.89455,116.35983,47.0,28800,Asia/Shanghai,GMT+8\n"
                "\n"
                "time,temperature_2m,weather_code\n"
                "2026-10-12T17:15,19.4,3\n";

            double temperature = 0.0;
            int code = -1;
            {
                // The values are read AFTER the parse so the detail describes what was actually
                // read: a detail formatted before the call reports the parser's zeros next to a
                // failed check, which is the one case the detail exists for.
                const bool readForecast = ParseCurrentCsv(forecast, &temperature, &code);
                wchar_t csvDetail[160];
                std::swprintf(csvDetail, 160, L"parsed %.1f C, code %d", temperature, code);
                Check(readForecast &&
                          Nearly(static_cast<float>(temperature), 19.4f, 0.001f) && code == 3,
                      L"the forecast CSV's last line is the reading", csvDetail);
                Check(!ParseCurrentCsv(nullptr, &temperature, &code),
                      L"a null document is not a reading");
                Check(!ParseCurrentCsv("", &temperature, &code) &&
                          !ParseCurrentCsv("latitude,longitude\n", &temperature, &code),
                      L"a document with no reading is not read as one", csvDetail);
                Check(!ParseCurrentCsv("2026-10-12T17:15,19.4\n", &temperature, &code),
                      L"a line missing the weather code is not a reading", csvDetail);
            }

            // The daily answer, likewise: the LAST line is today's "date,sunrise,sunset".
            {
                const char* daily =
                    "latitude,longitude,elevation,utc_offset_seconds,timezone,timezone_abbreviation\n"
                    "39.89455,116.35983,47.0,28800,Asia/Shanghai,GMT+8\n"
                    "\n"
                    "time,sunrise,sunset\n"
                    "2026-10-12,2026-10-12T06:18,2026-10-12T17:39\n";
                int sunrise = -1;
                int sunset = -1;
                const bool readDaily = ParseDailyCsv(daily, &sunrise, &sunset);
                wchar_t dailyDetail[160];
                std::swprintf(dailyDetail, 160, L"sunrise %d, sunset %d", sunrise, sunset);
                Check(readDaily && sunrise == 6 * 60 + 18 && sunset == 17 * 60 + 39,
                      L"the daily CSV gives the sunrise and the sunset", dailyDetail);
                Check(!ParseDailyCsv(nullptr, &sunrise, &sunset) &&
                          !ParseDailyCsv("time,sunrise,sunset\n", &sunrise, &sunset),
                      L"a daily answer with no row is refused", dailyDetail);
            }

            // The ambient kind: precipitation first, then the REAL sunset window for a clear sky.
            {
                const int rise = 6 * 60 + 18;
                const int set = 17 * 60 + 39;
                Check(AmbientKindFor(61, 12 * 60, rise, set) == AmbientKind::Rain,
                      L"rain is rain at noon");
                Check(AmbientKindFor(0, 12 * 60, rise, set) == AmbientKind::ClearDay,
                      L"a clear noon is the clear day");
                Check(AmbientKindFor(0, set - 10, rise, set) == AmbientKind::Sunset,
                      L"the real sunset window is the sunset");
                Check(AmbientKindFor(0, set + 40, rise, set) == AmbientKind::Sunset,
                      L"the afterglow after the sunset is still the sunset");
                Check(AmbientKindFor(0, 23 * 60, rise, set) == AmbientKind::Night,
                      L"after the sunset it is night");
                Check(AmbientKindFor(95, 12 * 60, rise, set) == AmbientKind::Thunder,
                      L"a thunderstorm code is the storm");
                Check(AmbientKindFor(71, 12 * 60, rise, set) == AmbientKind::Snow,
                      L"a snow code is the snow");
            }

            // The geocoder's answer, as Open-Meteo sends it: the wanted coordinates are the first
            // ones inside the FIRST entry of the results array.
            const char* geocode =
                "{\"results\":[{\"id\":1816670,\"name\":\"\u5317\u4eac\",\"latitude\":39.9075,"
                "\"longitude\":116.39723,\"elevation\":49.0,\"country\":\"\u4e2d\u56fd\"}],"
                "\"generationtime_ms\":0.119}";
            double latitude = 0.0;
            double longitude = 0.0;
            {
                const bool readPlace = ParseGeocodeJson(geocode, &latitude, &longitude);
                wchar_t geocodeDetail[192];
                std::swprintf(geocodeDetail, 192, L"read %.4f, %.4f", latitude, longitude);
                Check(readPlace &&
                          Nearly(static_cast<float>(latitude), 39.9075f, 0.0001f) &&
                          Nearly(static_cast<float>(longitude), 116.39723f, 0.0001f),
                      L"the geocoder's first result is the place that was asked for", geocodeDetail);
                Check(!ParseGeocodeJson("{}", &latitude, &longitude) &&
                          !ParseGeocodeJson("{\"results\":[]}", &latitude, &longitude),
                      L"a search that matched nothing is not a place", geocodeDetail);
            }

            // A Chinese city name has to become a legal query value, or the request is rejected.
            // The bytes are the UTF-8 encoding: \u5317 is E5 8C 97 and \u4EAC is E4 BA AC.
            {
                char encoded[128] = {};
                PercentEncodeUtf8("\u5317\u4eac", encoded, sizeof(encoded));
                wchar_t encodedDetail[160] = {};
                std::swprintf(encodedDetail, 160, L"encoded as %hs", encoded);
                Check(std::strcmp(encoded, "%E5%8C%97%E4%BA%AC") == 0,
                      L"a Chinese city name is percent-encoded", encodedDetail);

                char numbers[128] = {};
                PercentEncodeUtf8("39.9042,116.4074", numbers, sizeof(numbers));
                wchar_t numberDetail[160] = {};
                std::swprintf(numberDetail, 160, L"encoded as %hs", numbers);
                Check(std::strcmp(numbers, "39.9042%2C116.4074") == 0,
                      L"coordinates are escaped and their digits are not", numberDetail);
            }
        }

        // The hover preview: where the box goes, and whether the compositor will draw one.
        //
        // TWO QUESTIONS, and they fail for different reasons. The placement is pure arithmetic over three
        // rectangles -- it has to keep the source's shape, sit centred under the icon, hang below it and
        // never leave the monitor -- and the registration is a call to the compositor that can be refused
        // with a specific HRESULT (a layered destination, an elevated source, composition off). Both are
        // asked here, because "the preview never appeared" has no other witness.
        {
            const RECT monitor = {0, 0, 2560, 1440};
            const RECT icon = {1200, 40, 1230, 70};

            const ThumbnailPlacement wide = ThumbnailBoxFor(1920, 1080, icon, monitor, 1.0f);
            const ThumbnailPlacement tall = ThumbnailBoxFor(600, 1200, icon, monitor, 1.0f);
            const ThumbnailPlacement unknown = ThumbnailBoxFor(0, 0, icon, monitor, 1.0f);
            const ThumbnailPlacement sliver = ThumbnailBoxFor(1, 9000, icon, monitor, 1.0f);

            wchar_t previewDetail[256];
            std::swprintf(previewDetail, 256,
                          L"wide picture %ldx%ld in a %ldx%ld card, tall %ldx%ld, unknown %ldx%ld",
                          wide.picture.right - wide.picture.left,
                          wide.picture.bottom - wide.picture.top,
                          wide.card.right - wide.card.left, wide.card.bottom - wide.card.top,
                          tall.picture.right - tall.picture.left,
                          tall.picture.bottom - tall.picture.top,
                          unknown.picture.right - unknown.picture.left,
                          unknown.picture.bottom - unknown.picture.top);

            const auto aspectOf = [](const RECT& box) {
                const float w = static_cast<float>(box.right - box.left);
                const float h = static_cast<float>(box.bottom - box.top);
                return h > 0.0f ? w / h : 0.0f;
            };
            Check(wide.valid && tall.valid && unknown.valid && sliver.valid,
                  L"every preview box is a real rectangle", previewDetail);
            Check(Nearly(aspectOf(wide.picture), 1920.0f / 1080.0f, 0.02f) &&
                      Nearly(aspectOf(tall.picture), 0.5f, 0.02f),
                  L"the picture keeps the shape of what the compositor will draw", previewDetail);
            Check(wide.picture.right - wide.picture.left <= static_cast<LONG>(kThumbnailMaxWidthDip) &&
                      tall.picture.bottom - tall.picture.top <=
                          static_cast<LONG>(kThumbnailMaxHeightDip),
                  L"a picture never exceeds its maximum box", previewDetail);
            // THE SHAPE IS KEPT EVEN WHEN IT IS ABSURD: a 1 x 9000 window is a sliver, and the picture is
            // a sliver inside an ordinary card rather than a stretched lie. (The card is one size, so a
            // degenerate source can no longer make the CARD degenerate -- which is what the old minimum
            // side was for.)
            std::swprintf(previewDetail, 256, L"a 1x9000 source gives a %ldx%ld picture in a %ldx%ld card",
                          sliver.picture.right - sliver.picture.left,
                          sliver.picture.bottom - sliver.picture.top,
                          sliver.card.right - sliver.card.left, sliver.card.bottom - sliver.card.top);
            Check(aspectOf(sliver.picture) > 0.0f && aspectOf(sliver.picture) < 0.01f &&
                      sliver.picture.right <= sliver.card.right - 8 &&
                      sliver.picture.bottom <= sliver.card.bottom - 8,
                  L"even an absurd window keeps its shape inside an ordinary card", previewDetail);

            // ONE SIZE FOR EVERY WINDOW. This is the property the user asked for after seeing a row of
            // previews of different sizes -- and it is the reason the card no longer takes its shape from
            // the source: a wide window, a tall one and an unknown one must all give the same card.
            std::swprintf(previewDetail, 256,
                          L"wide %ldx%ld, tall %ldx%ld, unknown %ldx%ld",
                          wide.card.right - wide.card.left, wide.card.bottom - wide.card.top,
                          tall.card.right - tall.card.left, tall.card.bottom - tall.card.top,
                          unknown.card.right - unknown.card.left,
                          unknown.card.bottom - unknown.card.top);
            Check((wide.card.right - wide.card.left) == (tall.card.right - tall.card.left) &&
                      (wide.card.bottom - wide.card.top) == (tall.card.bottom - tall.card.top) &&
                      (wide.card.right - wide.card.left) == (unknown.card.right - unknown.card.left) &&
                      (wide.card.bottom - wide.card.top) == (unknown.card.bottom - unknown.card.top),
                  L"every window gets a card of the same size", previewDetail);

            // THE CARD IS A MARGIN AND A TITLE ROW AROUND A FIXED SLOT, and the picture is centred inside
            // that slot. Measured against the SLOT rather than against the picture, because the picture's
            // own margins include whatever the letterbox leaves.
            const LONG pad = static_cast<LONG>(kThumbnailPadDip);
            const LONG titleDip = static_cast<LONG>(kThumbnailTitleDip);
            const LONG leftGap = wide.picture.left - wide.card.left;
            const LONG rightGap = wide.card.right - wide.picture.right;
            // The letterbox on each side of the slot: zero when the picture fills the slot one way, and
            // LARGER than the margin when it does not -- which is why these are measured against the slot
            // rather than against the picture.
            const LONG topLetterbox = wide.picture.top - (wide.card.top + pad + titleDip);
            const LONG bottomLetterbox = (wide.card.bottom - pad) - wide.picture.bottom;
            const LONG cardWidth = wide.card.right - wide.card.left;
            const LONG cardHeight = wide.card.bottom - wide.card.top;
            const LONG expectedWidth =
                static_cast<LONG>(kThumbnailMaxWidthDip + kThumbnailPadDip * 2.0f);
            const LONG expectedHeight = static_cast<LONG>(
                kThumbnailMaxHeightDip + kThumbnailPadDip * 2.0f + kThumbnailTitleDip);
            std::swprintf(previewDetail, 256,
                          L"card %ldx%ld (want %ldx%ld), gaps l%ld r%ld, letterbox t%ld b%ld, margin %.0f",
                          cardWidth, cardHeight, expectedWidth, expectedHeight, leftGap, rightGap,
                          topLetterbox, bottomLetterbox, kThumbnailPadDip);
            Check(cardWidth == expectedWidth && cardHeight == expectedHeight &&
                      leftGap >= pad && rightGap >= pad && topLetterbox >= 0 && bottomLetterbox >= 0,
                  L"the card is a margin and a title row around a fixed slot", previewDetail);
            Check(std::abs(leftGap - rightGap) <= 1,
                  L"the picture is centred in its slot, so its letterbox is even", previewDetail);
            Check(std::abs(topLetterbox - bottomLetterbox) <= 1,
                  L"and even top to bottom", previewDetail);

            const float iconCentre = (icon.left + icon.right) * 0.5f;
            const float cardCentre = (wide.card.left + wide.card.right) * 0.5f;
            std::swprintf(previewDetail, 256, L"icon centre %.0f, card centre %.0f, card top %ld",
                          iconCentre, cardCentre, wide.card.top);
            Check(Nearly(cardCentre, iconCentre, 1.0f) && wide.card.top >= icon.bottom,
                  L"a preview is centred under its icon and hangs below it", previewDetail);

            // An icon in the corner: the box is pushed back inside the monitor rather than hanging off it.
            const RECT cornerIcon = {2540, 40, 2560, 70};
            const ThumbnailPlacement corner = ThumbnailBoxFor(1920, 1080, cornerIcon, monitor, 1.0f);
            std::swprintf(previewDetail, 256, L"corner icon -> box %ld..%ld (monitor 0..2560)",
                          corner.card.left, corner.card.right);
            Check(corner.card.left >= monitor.left && corner.card.right <= monitor.right &&
                      corner.card.bottom <= monitor.bottom,
                  L"a preview near the edge is kept on the monitor", previewDetail);

            // AND THE COMPOSITOR ITSELF, on a real window: the one part of this that no amount of
            // arithmetic can answer. The source is a plain STATIC window -- a real top-level window of
            // this process -- because what is being asked is whether the registration is granted at all.
            island::ThumbnailPreview preview;
            // No owner here: the bar's own window is not in scope in this part of the self-test, and what
            // this block is asking about is the registration, not the z-order (which is the owner's job).
            const bool created = preview.Create(nullptr);
            Check(created, L"the preview window is created");

            HWND source = nullptr;
            if (created) {
                source = CreateWindowExW(0, L"STATIC", L"preview source", WS_OVERLAPPEDWINDOW, 0, 0,
                                         640, 400, nullptr, nullptr, GetModuleHandleW(nullptr),
                                         nullptr);
                const bool shown =
                    source != nullptr && preview.Begin(source).cx > 0 &&
                    preview.Place(wide) && preview.Visible();
                std::swprintf(previewDetail, 256,
                              L"source window %hs, DwmRegisterThumbnail returned 0x%08llX, shown=%d",
                              source ? "created" : "NOT created", preview.LastResult(), shown ? 1 : 0);
                Check(shown,
                      L"the compositor draws a thumbnail of a real window into the preview",
                      previewDetail);

                preview.Hide();
                Check(!preview.Visible(), L"the preview can be put away again");
            }
            preview.Destroy();
            if (source) DestroyWindow(source);
        }

        // The system tray: the applications' own tray icons, at the right-hand end of the bar.
        //
        // WHY IT IS ANCHORED TO THE MONITOR and not to the row beside it: the row grows and shrinks with
        // the windows that are open, so a tray hung off its right edge would slide sideways every time a
        // window opened. The properties below are all statements about that decision -- the group does not
        // move, and it is the ROW that has to give way.
        {
            const float trayMonitor = 2560.0f;
            const RoundedBox trayIsland =
                IslandBoxFor(trayMonitor, kCollapsedWidthDip, kCollapsedHeightDip);

            wchar_t trayDetail[256];
                    // Its own buffer rather than trayDetail, which the layout checks below keep using: two
                    // checks writing the same detail would make a failure point at the wrong one.
                    wchar_t trayWireDetail[256] = {};
            {
                // THE BUTTON, at the monitor's right margin: one control, in the same place whatever the
                // application row is doing (see TrayButtonFor).
                const RectF button = TrayButtonFor(trayIsland);
                std::swprintf(trayDetail, 256,
                              L"button x [%.0f, %.0f] y [%.0f, %.0f], island right %.0f", button.left,
                              button.right, button.top, button.bottom, trayIsland.rect.right);
                Check(Nearly(button.left, trayIsland.rect.right + kTaskIconPadDip, 0.01f),
                      L"the tray button stands one pad to the right of the island", trayDetail);
                Check(button.right > trayIsland.rect.right,
                      L"the tray button is on the island's right-hand side", trayDetail);
                Check(button.right < trayMonitor, L"the tray button is on the screen", trayDetail);
                Check(Nearly((button.top + button.bottom) * 0.5f,
                             (trayIsland.rect.top + trayIsland.rect.bottom) * 0.5f, 0.01f),
                      L"the tray button is on the island's own line", trayDetail);
            }

            // WHAT THE BUTTON MOVES, AND WHAT IT MUST NOT.
            //
            // The row is told where it starts by moving ONE edge of the box it is measured from (see
            // RowAnchorWithTray): everything the row does -- its left edge, every button, every hit test --
            // follows from that box's right edge, and nothing else about the box may change. The island's
            // left edge, top and bottom belong to the deck and the pill: a version of this that moved them
            // would take the Start button and the status line along with the tray.
            {
                const RoundedBox moved = RowAnchorWithTray(trayIsland, true);
                const float reserved = TrayButtonReservedDip();
                std::swprintf(trayDetail, 256,
                              L"island right %.0f -> anchor right %.0f (reserved %.0f), anchor left %.0f",
                              trayIsland.rect.right, moved.rect.right, reserved, moved.rect.left);
                Check(Nearly(moved.rect.right, trayIsland.rect.right + reserved, 0.01f),
                      L"the tray button moves the row's anchor by exactly its own width", trayDetail);
                Check(Nearly(moved.rect.left, trayIsland.rect.left, 0.01f) &&
                          Nearly(moved.rect.top, trayIsland.rect.top, 0.01f) &&
                          Nearly(moved.rect.bottom, trayIsland.rect.bottom, 0.01f),
                      L"... and moves nothing else, so the deck and the pill stay put", trayDetail);
                const RoundedBox untrayed = RowAnchorWithTray(trayIsland, false);
                Check(Nearly(untrayed.rect.right, trayIsland.rect.right, 0.01f),
                      L"with no tray to show the anchor is the island itself", trayDetail);
            }

            // THE CARD'S GRID: the icons behind the button, in a wrapping grid inside the SAME card the
            // system panel opens -- three rows of five on a machine with sixteen icons, where the panel's own
            // two rows of three would hold six.
            {
                const RoundedBox trayCard =
                    BoxForState(IslandState::Expanded,
                                AnchorForWindow(0.0f, kPillTopMarginDip, 2560.0f));
                // The counts are the ones that MATTER, not the ones that fit: this machine has sixteen tray
                // icons, and the old list stopped at ten -- exactly the number five columns and two rows
                // hold -- so the grid could overflow the card and nothing said so until a photograph did
                // ("托盘卡片里没有显示全啊").
                for (int count : {1, 3, 6, 7, 10, 16, kMaxTrayIcons}) {
                    const TrayPageLayout page = TrayPageFor(trayCard, count);
                    bool inside = page.count == count && page.columns > 0 &&
                                  page.columns <= kTrayPageMaxColumns;
                    float smallestCell = 1e9f;
                    bool noOverlap = true;
                    for (int i = 0; i < page.count; ++i) {
                        const RectF& cell = page.cells[i];
                        if (cell.right - cell.left < smallestCell) smallestCell = cell.right - cell.left;
                        if (cell.left < trayCard.rect.left || cell.right > trayCard.rect.right ||
                            cell.top < trayCard.rect.top || cell.bottom > trayCard.rect.bottom) {
                            inside = false;
                        }
                        for (int j = i + 1; j < page.count; ++j) {
                            const RectF& other = page.cells[j];
                            if (cell.left < other.right && other.left < cell.right &&
                                cell.top < other.bottom && other.top < cell.bottom) {
                                noOverlap = false;
                            }
                        }
                    }
                    std::swprintf(trayDetail, 256,
                                  L"%d icons -> %d columns, first cell %.0f (w%.1f, min %.1f), "
                                  L"last cell bottom %.0f of %.0f",
                                  count, page.columns, page.cells[0].left,
                                  page.cells[0].right - page.cells[0].left,
                                  smallestCell, page.cells[count - 1].bottom, trayCard.rect.bottom);
                    Check(inside, L"every tray cell is inside the card it is drawn in", trayDetail);
                    Check(noOverlap, L"no two tray cells overlap", trayDetail);
                    Check(smallestCell >= kTrayPageMinCellDip - 0.01f,
                          L"... and every cell is still big enough to press", trayDetail);

                    Check(page.cells[0].top > trayCard.rect.top + 30.0f,
                          L"the grid starts below the page's heading", trayDetail);

                    // 7 icons on this card is 5+2, and the incomplete row must be CENTRED. Left-aligning
                    // it put two cells against the left, three empty columns on the right, and read as a
                    // row that had lost icons. The two gaps either side of the short row have to match.
                    if (count == 7 && page.columns == 5) {
                        const float rowLeft = page.cells[5].left;
                        const float rowRight = page.cells[6].right;
                        const float fullLeft = page.cells[0].left;
                        const float fullRight = page.cells[4].right;
                        Check(std::abs((rowLeft - fullLeft) - (fullRight - rowRight)) <= 0.51f,
                              L"the tray grid's incomplete last row is centred", trayDetail);
                    }
                }
            }

            // --- THE WIRE FORMAT OF A TRAY REGISTRATION ------------------------------------------
            //
            // The module that reads registrations lives inside the shell's process, where no test can
            // follow it: everything it is told to read arrives as a WM_COPYDATA whose payload is the
            // shell's own copy of NOTIFYICONDATA. So the READING of those bytes is a pure function in
            // domain/TrayRegistration.h, and this is the test that keeps it honest.
            //
            // THE FIRST CASE IS REAL BYTES, not a construction: the 192 bytes below are the head of the
            // copy that probe-tray-registrar.exe (uid 7, message 0xABCD, tip ISLANDBAR-PROBE-4711) sent
            // to the shell on 2026-10-15, pasted from reviews/tray-copydata-dump.txt. The payload on the
            // wire is 1484 bytes and only its head was captured, so the rest is zero here -- which is
            // exactly why this case asserts no guid: the guid field lives at +0x3B0, past the capture.
            {
                static const char* probeHead =
                    "2334753400000000BC030000820A1F000700000007000000CDAB00002B000100490053004C0041004E"
                    "0044004200410052002D00500052004F00420045002D003400370031003100000000000000000000"
                    "000000000000000000000000000000000000000000000000000000000000000000000000000000"
                    "000000000000000000000000000000000000000000000000000000000000000000000000000000"
                    "000000000000000000000000000000000000000000000000000000000000000000000000000000"
                    "0000000000000000000000000000000000000000000000000000000000000000000000000000";
                std::vector<unsigned char> payload(island::kTrayCopyMinimum, 0);
                for (size_t i = 0; i < std::strlen(probeHead) / 2 && i < payload.size(); ++i) {
                    payload[i] = static_cast<unsigned char>(
                        std::strtoul(std::string(probeHead + i * 2, 2).c_str(), nullptr, 16));
                }
                const island::TrayRegistration decoded =
                    island::DecodeTrayRegistration(payload.data(), payload.size());
                std::swprintf(trayWireDetail, 256,
                              L"valid=%d uid=%u msg=0x%04X tip='%ls'",
                              decoded.valid ? 1 : 0, decoded.uid, decoded.callback, decoded.tip);
                Check(decoded.valid && decoded.command == island::kTrayCopyAdd && decoded.uid == 7 &&
                          decoded.callback == 0xABCD && decoded.trayWindow == 0x1F0A82 &&
                          !decoded.hasGuid &&
                          std::wcscmp(decoded.tip, L"ISLANDBAR-PROBE-4711") == 0,
                      L"a registration's real bytes decode to the values it was registered with",
                      trayWireDetail);
            }
            // A GUID-registered icon -- the shape ChatGPT.exe registers in, with the guid Explorer's
            // own registry entry names for it. Built here at the measured offsets because the captured
            // head above does not reach the guid field; the values are the ones the capture carried.
            {
                std::vector<unsigned char> payload(island::kTrayCopyMinimum, 0);
                const unsigned tag = island::kTrayCopyTag;
                const unsigned window = 0x000E0B32, uid = 3, flags = 0x27, callback = 0x8001;
                std::memcpy(payload.data(), &tag, 4);
                std::memcpy(payload.data() + island::kTrayCopyOffsetWindow, &window, 4);
                std::memcpy(payload.data() + island::kTrayCopyOffsetUid, &uid, 4);
                std::memcpy(payload.data() + island::kTrayCopyOffsetFlags, &flags, 4);
                std::memcpy(payload.data() + island::kTrayCopyOffsetCallback, &callback, 4);
                const unsigned char guidBytes[16] = {0x8B, 0x8D, 0x76, 0xE5, 0x36, 0x69, 0x45, 0x4F,
                                                     0xB1, 0xAD, 0x4C, 0x5F, 0xB4, 0x14, 0xCB, 0x35};
                std::memcpy(payload.data() + island::kTrayCopyOffsetGuid, guidBytes, 16);
                const wchar_t* tipText = L"ChatGPT";
                std::memcpy(payload.data() + island::kTrayCopyOffsetTip, tipText,
                            (std::wcslen(tipText) + 1) * sizeof(wchar_t));
                const island::TrayRegistration decoded =
                    island::DecodeTrayRegistration(payload.data(), payload.size());
                std::swprintf(trayWireDetail, 256, L"valid=%d guid=%d uid=%u msg=0x%04X",
                              decoded.valid ? 1 : 0, decoded.hasGuid ? 1 : 0, decoded.uid,
                              decoded.callback);
                Check(decoded.valid && decoded.hasGuid && decoded.uid == 3 &&
                          decoded.callback == 0x8001 &&
                          std::memcmp(decoded.guid, guidBytes, 16) == 0 &&
                          std::wcscmp(decoded.tip, L"ChatGPT") == 0,
                      L"a guid-registered icon decodes with its guid, which is what it is pressed by",
                      trayWireDetail);
            }
            // ... AND THE TWO WAYS A PAYLOAD MUST BE REFUSED, because a reader that guesses is worse
            // than one that learns nothing: the shell changing this format must cost the tray its
            // learning, visibly, and not produce presses aimed at the wrong message number.
            {
                std::vector<unsigned char> payload(island::kTrayCopyMinimum, 0);
                const unsigned notTheShellsTag = 0x12345678u;
                std::memcpy(payload.data(), &notTheShellsTag, 4);
                const island::TrayRegistration decoded =
                    island::DecodeTrayRegistration(payload.data(), payload.size());
                const island::TrayRegistration tooSmall =
                    island::DecodeTrayRegistration(payload.data(), island::kTrayCopyMinimum - 1);
                std::swprintf(trayWireDetail, 256, L"wrong tag valid=%d, short valid=%d",
                              decoded.valid ? 1 : 0, tooSmall.valid ? 1 : 0);
                Check(!decoded.valid && !tooSmall.valid,
                      L"a payload that is not the shell's, or too short, is refused rather than read",
                      trayWireDetail);
            }

            // --- THE CHANNEL BETWEEN THE MODULE AND THE BAR -------------------------------------
            //
            // A registration travels over a shared-memory ring (platform/TrayRing.h): the module
            // publishes while an application waits for the shell, the bar drains it on its own time.
            // Two things about that are worth a test on this machine: a record survives the round trip
            // unchanged, and a ring whose ABI disagrees is REFUSED rather than read -- because the
            // failure that produces is a press aimed at the wrong message number, which looks like
            // nothing at all.
            {
                island::TrayRingPublisher publisher;
                // A ring of this test's own. Publishing into the production ring would put records in a
                // running bar's queue, and the ABI check below deliberately corrupts a header -- which
                // must never be done to the ring the bar is reading.
                const bool opened = publisher.Open(L"Local\\IslandBar.TrayRing.selftest",
                                                   L"Local\\IslandBar.TrayRing.selftest.Event");
                std::swprintf(trayWireDetail, 256, L"publisher opened=%d abi=%u record=%u",
                              opened ? 1 : 0, island::kTrayRingAbi,
                              static_cast<unsigned>(sizeof(island::TrayRingRecord)));
                Check(opened && publisher.header && publisher.header->recordSize ==
                                    sizeof(island::TrayRingRecord),
                      L"the ring opens and states the record size both sides agreed on",
                      trayWireDetail);

                if (opened && publisher.header) {
                    island::TrayRegistration pushed;
                    pushed.valid = true;
                    pushed.command = island::kTrayCopyAdd;
                    pushed.trayWindow = 0x1234;
                    pushed.uid = 9;
                    pushed.flags = 0x7;
                    pushed.callback = 0xBEEF;
                    wcsncpy_s(pushed.tip, L"ROUND-TRIP", _TRUNCATE);
                    publisher.Publish(pushed);

                    island::TrayRingReader reader;
                    unsigned cursor = 0;
                    island::TrayRingRecord taken[island::kTrayRingCapacity] = {};
                    reader.Open(L"Local\\IslandBar.TrayRing.selftest",
                                L"Local\\IslandBar.TrayRing.selftest.Event");
                    // Skip everything already in the ring (a bar may be running), then take the record
                    // this check just pushed: the last one written.
                    const unsigned all = reader.Drain(cursor, taken, island::kTrayRingCapacity);
                    const island::TrayRegistration& back =
                        taken[all > 0 ? all - 1 : 0].registration;
                    std::swprintf(trayWireDetail, 256, L"took=%u uid=%u msg=0x%04X tip='%ls'",
                                  all, back.uid, back.callback, back.tip);
                    Check(all > 0 && back.valid && back.uid == 9 && back.callback == 0xBEEF &&
                              back.trayWindow == 0x1234 &&
                              std::wcscmp(back.tip, L"ROUND-TRIP") == 0,
                          L"a registration survives the ring unchanged", trayWireDetail);

                    // ... AND THE CONTROL RECORD THE HANDSHAKE TRAVELS IN. The bar waits for this before it
                    // asks the applications to register again (shell/TrayLearn.h), so the two kinds must not
                    // be readable as each other: a hook state filed as a registration would invent an
                    // application entry out of the module's own numbers, and a registration read as state
                    // would end the wait on somebody else's icon.
                    island::TrayHookState state;
                    state.pid = GetCurrentProcessId();
                    state.inShell = true;
                    state.ready = true;
                    state.trayThread = 0x21;
                    state.hooks = 1;
                    state.attempts = 2;
                    state.lastError = 0;
                    publisher.PublishHookState(state);
                    const unsigned more = reader.Drain(cursor, taken, island::kTrayRingCapacity);
                    const island::TrayRingRecord& control = taken[more > 0 ? more - 1 : 0];
                    std::swprintf(trayWireDetail, 256, L"took=%u kind=%u ready=%d tid=%u hooks=%u attempts=%u",
                                  more, static_cast<unsigned>(control.kind), control.hook.ready ? 1 : 0,
                                  control.hook.trayThread, control.hook.hooks, control.hook.attempts);
                    Check(more > 0 && control.kind == island::TrayRingKind::HookState && control.hook.ready &&
                              control.hook.trayThread == 0x21 && control.hook.hooks == 1 &&
                              control.hook.attempts == 2 &&
                              control.hook.pid == static_cast<unsigned>(GetCurrentProcessId()),
                          L"the observer's handshake travels the ring as a control record, with the numbers "
                          L"the log needs",
                          trayWireDetail);

                    // ... and the kind belongs to the RECORD, not to the slot it landed in: the ring wraps,
                    // so a registration that reuses a control record's slot must still read as one. The ring
                    // is filled past its capacity on purpose -- that is the only state in which the two
                    // share a slot.
                    for (unsigned i = 0; i < island::kTrayRingCapacity; ++i) {
                        publisher.PublishHookState(state);
                    }
                    island::TrayRegistration after = pushed;
                    after.uid = 11;
                    publisher.Publish(after);
                    const unsigned reused = reader.Drain(cursor, taken, island::kTrayRingCapacity);
                    const island::TrayRingRecord& newest = taken[reused > 0 ? reused - 1 : 0];
                    std::swprintf(trayWireDetail, 256, L"took=%u kind=%u uid=%u", reused,
                                  static_cast<unsigned>(newest.kind), newest.registration.uid);
                    Check(reused > 0 && newest.kind == island::TrayRingKind::Registration &&
                              newest.registration.uid == 11,
                          L"a registration reusing a control record's slot does not read as a control record",
                          trayWireDetail);

                    // ... and the ABI is refused when it disagrees. Corrupted here on purpose and put
                    // straight back: this ring belongs to this test process, and the next check below
                    // is the one that reads it.
                    const unsigned savedAbi = publisher.header->abi;
                    publisher.header->abi = savedAbi + 1;
                    island::TrayRingReader refusing;
                    const bool refused = !refusing.Open(L"Local\\IslandBar.TrayRing.selftest",
                                                        L"Local\\IslandBar.TrayRing.selftest.Event");
                    publisher.header->abi = savedAbi;
                    std::swprintf(trayWireDetail, 256, L"abi seen=%u, refused=%d",
                                  refusing.abiSeen, refused ? 1 : 0);
                    Check(refused && refusing.abiSeen == savedAbi + 1,
                          L"a ring from another build is refused, not read", trayWireDetail);
                }
            }
            // WHERE THE TRAY PAGE'S CARD STANDS: under the TRAY button, not under the one at the other
            // end of the island.
            //
            // The panel and the tray page are one card with one owner (NextCardOwner sees only "a panel is
            // asked for"), so the position can only come from the PAGE -- and the two controls are the
            // island's two ends, about 300 DIP apart. The tray page was centred on the SYSTEM button, which
            // is what "点击托盘按钮 弹出的卡片不在这个按钮下面" was: press the button on the right, get a
            // card under the left.
            //
            // Asserted as the two things a user would notice: the answer names the button that was pressed,
            // and the card that follows from it contains that button's centre and not the other's.
            {
                const float trayCentre = DeckCardAnchorCentre(trayIsland, CardHang::TrayButton);
                const float systemCentre = DeckCardAnchorCentre(trayIsland, CardHang::SystemButton);
                const RectF trayButton = TrayButtonFor(trayIsland);
                const RectF systemButton = SystemButtonFor(trayIsland);
                const float trayButtonCentre = (trayButton.left + trayButton.right) * 0.5f;
                const float systemButtonCentre = (systemButton.left + systemButton.right) * 0.5f;

                const float monitor = 2560.0f;
                const float cardLeft = CardLeftFor(trayCentre - kExpandedWidthDip * 0.5f,
                                                   kExpandedWidthDip, monitor);
                const float cardRight = cardLeft + kExpandedWidthDip;

                std::swprintf(trayDetail, 256,
                              L"tray anchor %.0f (button %.0f), panel anchor %.0f (button %.0f), card "
                              L"%.0f..%.0f",
                              trayCentre, trayButtonCentre, systemCentre, systemButtonCentre, cardLeft,
                              cardRight);
                const float startCentre = DeckCardAnchorCentre(trayIsland, CardHang::StartButton);
                const float startButtonCentre =
                    (StartButtonFor(trayIsland).left + StartButtonFor(trayIsland).right) * 0.5f;
                Check(Nearly(trayCentre, trayButtonCentre, 0.01f) &&
                          Nearly(systemCentre, systemButtonCentre, 0.01f) &&
                          Nearly(startCentre, startButtonCentre, 0.01f),
                      L"each page's card is centred on the button that opened it", trayDetail);
                Check(std::fabs(trayCentre - systemCentre) > 200.0f,
                      L"... and those two buttons are far enough apart for a wrong answer to show",
                      trayDetail);
                Check(trayCentre > cardLeft && trayCentre < cardRight &&
                          !(systemButtonCentre > cardLeft && systemButtonCentre < cardRight),
                      L"the tray page's card covers the tray button and not the system button",
                      trayDetail);
                Check(cardLeft >= 8.0f && cardRight <= monitor - 8.0f,
                      L"and that card is still on the screen", trayDetail);
            }

            // ONLY THE LEVEL THAT IS SHOWING MAY ANSWER THE POINTER (2026-10-14, HANDOFF 8.81) -------
            //
            // Reported with a photograph of the tray page's lower band: "在托盘卡片红框的地方拖动 声音也会跟着
            // 变". All four levels are ONE card in one rectangle, so each of them has controls where another
            // has empty space -- the panel's volume slider lies exactly across that band -- and the hit test
            // was asking "is the panel open", which is true for every level of it.
            {
                const PanelLevel levels[4] = {PanelLevel::Tiles, PanelLevel::Sound, PanelLevel::Tray,
                                              PanelLevel::Start};
                int answering = 0;
                int crossing = 0;
                for (const PanelLevel showing : levels) {
                    for (const PanelLevel controls : levels) {
                        if (LevelAnswersInput(showing, controls)) {
                            ++answering;
                            if (showing != controls) ++crossing;
                        }
                    }
                }
                wchar_t levelInputDetail[192];
                std::swprintf(levelInputDetail, 192,
                              L"%d of 16 pairs answer, %d of them from another level", answering,
                              crossing);
                Check(answering == 4 && crossing == 0,
                      L"exactly one level answers a point: the one that is showing", levelInputDetail);
            }

            // WHICH LEVEL THE CARD SHOWS, INCLUDING WHILE IT IS PUT AWAY -- the half that is easy to lose.
            //
            // Every row of NextPanelLevel's table, because the two "put away" rows are what a later change
            // would drop: the card is still on screen for the length of its exit, so a level read per frame
            // from the page that was asked for switches the CONTENT on the first frame of the retraction --
            // the tiles appear under a shrinking tray page ("点击展开 再点击收回去 会显示出系统卡片的东西"),
            // and the card would also slide 296 DIP back to the other end of the bar.
            {
                const PanelLevel panelOpen =
                    NextPanelLevel(PanelLevel::Tiles, true, true, false, false, false);
                const PanelLevel trayOpen =
                    NextPanelLevel(PanelLevel::Tiles, true, true, true, false, false);
                const PanelLevel soundOpen =
                    NextPanelLevel(PanelLevel::Tiles, true, true, false, true, false);
                const PanelLevel startOpen =
                    NextPanelLevel(PanelLevel::Tiles, true, true, false, false, true);
                const PanelLevel back =
                    NextPanelLevel(PanelLevel::Sound, true, true, false, false, false);
                const PanelLevel trayClosing =
                    NextPanelLevel(PanelLevel::Tray, true, false, true, false, false);
                const PanelLevel soundClosing =
                    NextPanelLevel(PanelLevel::Sound, true, false, false, false, false);
                const PanelLevel startClosing =
                    NextPanelLevel(PanelLevel::Start, true, false, false, false, false);
                const PanelLevel gone =
                    NextPanelLevel(PanelLevel::Tray, false, false, false, false, false);
                std::swprintf(trayDetail, 256,
                              L"tiles=%d tray=%d sound=%d start=%d back=%d trayClosing=%d "
                              L"soundClosing=%d startClosing=%d gone=%d",
                              static_cast<int>(panelOpen), static_cast<int>(trayOpen),
                              static_cast<int>(soundOpen), static_cast<int>(startOpen),
                              static_cast<int>(back), static_cast<int>(trayClosing),
                              static_cast<int>(soundClosing), static_cast<int>(startClosing),
                              static_cast<int>(gone));
                Check(panelOpen == PanelLevel::Tiles && trayOpen == PanelLevel::Tray &&
                          soundOpen == PanelLevel::Sound && startOpen == PanelLevel::Start &&
                          back == PanelLevel::Tiles,
                      L"a page that is asked for is the level the card shows", trayDetail);
                Check(trayClosing == PanelLevel::Tray && soundClosing == PanelLevel::Sound &&
                          startClosing == PanelLevel::Start,
                      L"... and the panel being put away holds that level for the whole exit", trayDetail);
                Check(gone == PanelLevel::Tiles && trayOpen != panelOpen,
                      L"... released only when the card is gone, so the next open decides afresh",
                      trayDetail);

                // ... AND EXACTLY ONE CONTROL LOOKS OPEN, whichever level is showing -- the property the
                // system button's chevron was getting wrong (it pointed up for every level, because it was
                // asked `systemPanelOpen`, which is true whenever any card is up).
                const PanelLevel every[] = {PanelLevel::Tiles, PanelLevel::Sound, PanelLevel::Tray,
                                            PanelLevel::Start};
                bool oneEach = true;
                bool noneWithoutACard = true;
                for (PanelLevel level : every) {
                    const ControlLit lit = ControlLitFor(level, true);
                    const int count = (lit.system ? 1 : 0) + (lit.tray ? 1 : 0) + (lit.start ? 1 : 0);
                    if (count != 1) oneEach = false;
                    // NO CARD AT ALL: nothing may be lit. This is the case the first version got wrong -- a
                    // bar that had just started, with no card on screen, showed the system button's chevron
                    // pointing up, because `Tiles` also means "nothing was asked for".
                    const ControlLit off = ControlLitFor(level, false);
                    if (off.system || off.tray || off.start) noneWithoutACard = false;
                }
                std::swprintf(trayDetail, 256,
                              L"tiles: system=%d start=%d tray=%d | start: system=%d start=%d tray=%d | "
                              L"no card: system=%d start=%d tray=%d",
                              ControlLitFor(PanelLevel::Tiles, true).system ? 1 : 0,
                              ControlLitFor(PanelLevel::Tiles, true).start ? 1 : 0,
                              ControlLitFor(PanelLevel::Tiles, true).tray ? 1 : 0,
                              ControlLitFor(PanelLevel::Start, true).system ? 1 : 0,
                              ControlLitFor(PanelLevel::Start, true).start ? 1 : 0,
                              ControlLitFor(PanelLevel::Start, true).tray ? 1 : 0,
                              ControlLitFor(PanelLevel::Tiles, false).system ? 1 : 0,
                              ControlLitFor(PanelLevel::Tiles, false).start ? 1 : 0,
                              ControlLitFor(PanelLevel::Tiles, false).tray ? 1 : 0);
                Check(oneEach,
                      L"exactly one control shows that it is open, for every level", trayDetail);
                Check(noneWithoutACard,
                      L"... and none of them does when no card is up at all", trayDetail);
                Check(SystemButtonShowsOpen(PanelLevel::Tiles) &&
                          !SystemButtonShowsOpen(PanelLevel::Tray) &&
                          !SystemButtonShowsOpen(PanelLevel::Start) &&
                          StartButtonShowsOpen(PanelLevel::Start),
                      L"the system button is not lit by the tray page or the Start card", trayDetail);
            }

            // THE START CARD'S OWN GEOMETRY, which is the whole of its layout: a grid that starts empty,
            // a footer with the account picture and the power button, and the two lists that can take the
            // grid's place. Every one of these is a pure function shared by the drawing and the hit tests,
            // so what is asserted here is what a click is routed against.
            {
                const RoundedBox startCard =
                    BoxForState(IslandState::Expanded, AnchorForWindow(0.0f, kPillTopMarginDip, 2560.0f));
                const StartPageLayout startLayout = StartPageFor(startCard);
                std::swprintf(trayDetail, 256,
                              L"card x[%.0f,%.0f] y[%.0f,%.0f]; grid x[%.0f,%.0f] y[%.0f,%.0f]; "
                              L"footer top %.0f; avatar x[%.0f,%.0f]; power x[%.0f,%.0f]",
                              startCard.rect.left, startCard.rect.right, startCard.rect.top, startCard.rect.bottom,
                              startLayout.grid.left, startLayout.grid.right, startLayout.grid.top, startLayout.grid.bottom,
                              startLayout.footer.top, startLayout.avatar.left, startLayout.avatar.right,
                              startLayout.powerButton.left, startLayout.powerButton.right);
                Check(startLayout.valid, L"the Start card has a layout", trayDetail);
                Check(startLayout.grid.bottom < startLayout.footer.top &&
                          startLayout.footer.bottom <= startCard.rect.bottom &&
                          startLayout.grid.top > startCard.rect.top,
                      L"the pins area sits above the footer, inside the card", trayDetail);
                Check(startLayout.avatar.right < startLayout.powerButton.left &&
                          startLayout.footer.left <= startLayout.avatar.left &&
                          startLayout.powerButton.right <= startLayout.footer.right,
                      L"the account picture and the power button share the footer without overlapping",
                      trayDetail);
                Check(startLayout.grid.left >= startCard.rect.left && startLayout.grid.right <= startCard.rect.right,
                      L"... and the grid is inside the card's width", trayDetail);

                // The cells: six across, reading order, and every one of them inside the grid.
                Check((StartPinCellFor(startLayout, 0).left == startLayout.grid.left),
                      L"the first pinned tile starts at the grid's own corner", trayDetail);
                Check(Nearly(StartPinCellFor(startLayout, 5).left - StartPinCellFor(startLayout, 0).left,
                             5.0f * (kStartCellWidth + kStartCellGap), 0.01f),
                      L"six tiles fill the first row at one pitch each", trayDetail);
                Check(StartPinCellFor(startLayout, 6).left == startLayout.grid.left &&
                          StartPinCellFor(startLayout, 6).top > StartPinCellFor(startLayout, 0).top,
                      L"the seventh tile starts the second row", trayDetail);
                bool cellsInside = true;
                for (int i = 0; i < StartPinCapacity(); ++i) {
                    const RectF cell = StartPinCellFor(startLayout, i);
                    if (cell.right > startLayout.grid.right + 0.01f || cell.bottom > startLayout.grid.bottom + 0.01f) {
                        cellsInside = false;
                    }
                }
                Check(cellsInside, L"every cell of a full grid is inside the pins area", trayDetail);
                Check(StartPinCapacity() >= 12,
                      L"the grid holds enough tiles to be worth filling by hand", trayDetail);

                // The hit tests, at the points a user would press.
                const RectF third = StartPinCellFor(startLayout, 3);
                const float pinCentreY = (third.top + third.bottom) * 0.5f;
                Check(StartPinCellAt(startLayout, 6, (third.left + third.right) * 0.5f, pinCentreY) == 3,
                      L"a point on the fourth tile answers the fourth tile", trayDetail);
                Check(StartPinCellAt(startLayout, 6, third.left - 2.0f, pinCentreY) == 2,
                      L"... and the gap before it belongs to the tile before", trayDetail);
                Check(StartPinCellAt(startLayout, 0, (startLayout.grid.left + startLayout.grid.right) * 0.5f,
                                     (startLayout.grid.top + startLayout.grid.bottom) * 0.5f) == -1 &&
                          StartRectHasPoint(startLayout.grid, (startLayout.grid.left + startLayout.grid.right) * 0.5f,
                                            (startLayout.grid.top + startLayout.grid.bottom) * 0.5f),
                      L"with nothing pinned, the middle of the area is the area and not a tile",
                      trayDetail);
                Check(StartRectHasPoint(startLayout.powerButton,
                                        (startLayout.powerButton.left + startLayout.powerButton.right) * 0.5f,
                                        (startLayout.powerButton.top + startLayout.powerButton.bottom) * 0.5f),
                      L"the power button is under its own centre", trayDetail);
                Check(startLayout.allApps.right <= startCard.rect.right - 15.0f &&
                          startLayout.allApps.right - startLayout.allApps.left > 20.0f &&
                          startLayout.allApps.bottom <= startLayout.grid.top,
                      L"the 全部 icon button owns a header row above the grid", trayDetail);

                const RectF powerRow = StartPowerRowFor(startCard, 0);
                Check(StartPowerRowAt(startCard, (powerRow.left + powerRow.right) * 0.5f,
                                      (powerRow.top + powerRow.bottom) * 0.5f) == 0,
                      L"the first power row answers zero", trayDetail);
                Check(StartPowerRowFor(startCard, 4).bottom < startLayout.footer.top,
                      L"... and the fifth stands above the footer, not on it", trayDetail);

                // The picker: rows from the grid's top, one per application, and the scroll offset is part
                // of the answer.
                const RectF pickerRow = StartPickerRowFor(startCard, 1);
                Check(StartPickerRowAt(startCard, 0, 40, (pickerRow.left + pickerRow.right) * 0.5f,
                                       (pickerRow.top + pickerRow.bottom) * 0.5f) == 1,
                      L"the second visible row answers the second application", trayDetail);
                Check(StartPickerRowAt(startCard, 7, 40, (pickerRow.left + pickerRow.right) * 0.5f,
                                       (pickerRow.top + pickerRow.bottom) * 0.5f) == 8,
                      L"... and with the list scrolled by seven, the second row is the ninth application",
                      trayDetail);
                Check(StartPickerRowAt(startCard, 0, 40, startLayout.grid.left + 4.0f, startLayout.footer.top - 2.0f) ==
                          -1,
                      L"the footer's edge is not a row", trayDetail);

                // ... AND THE SCROLL OFFSET IS PART OF THAT ANSWER, which is also what makes the card ask
                // for a frame at all when the pointer moves or the wheel turns. The three checks below are
                // three lines of arithmetic, and they are here because their absence was invisible: the
                // condition that decides whether to draw did not mention these numbers, so the list and the
                // highlight moved once a second and every geometry check still passed.
                //
                // Taken from the enum the frame loop casts, so the numbers here cannot drift from the ones
                // the loop compares.
                const int kNone = static_cast<int>(IslandWindow::StartHitTarget::None);
                const int kPin = static_cast<int>(IslandWindow::StartHitTarget::Pin);
                const int kPickerRow = static_cast<int>(IslandWindow::StartHitTarget::PickerRow);
                Check(!StartCardAsksForFrame(kPickerRow, 3, 7, kPickerRow, 3, 7),
                      L"a pointer that has not moved asks for nothing", trayDetail);
                Check(StartCardAsksForFrame(kPickerRow, 4, 7, kPickerRow, 3, 7),
                      L"another row under the same pointer is a change", trayDetail);
                Check(StartCardAsksForFrame(kPin, 0, 7, kPickerRow, 0, 7),
                      L"... and so is a different kind of control", trayDetail);
                Check(StartCardAsksForFrame(kPickerRow, 3, 8, kPickerRow, 3, 7),
                      L"one notch of wheel with the pointer standing still is a change too", trayDetail);
                // The frame loop's own initialisers: "no target, no row, no scroll" against trackers that
                // start at a value no hit can equal. The first frame of every run is this comparison.
                Check(StartCardAsksForFrame(kNone, -1, 0, kNone, -2, -1),
                      L"the first frame is a change (the trackers start unknown)", trayDetail);
            }

            // THE ROW GIVES WAY, AND THAT IS THE POINT OF THE RESERVATION.
            //
            // The count of application icons that fit is computed from the room left after the BUTTON
            // (see the frame loop): what has to hold is that the row, laid out with that count, ends before
            // the button begins -- for every number of windows, which is what makes this a property rather
            // than a coincidence for today's.
            {
                const float reserved = TrayButtonReservedDip();
                const RectF button = TrayButtonFor(trayIsland);
                bool clear = true;
                for (int wanted : {1, 4, 12, 40}) {
                    const int fits = TaskIconsThatFit(trayMonitor - reserved, wanted);
                    if (fits <= 0) continue;
                    float rowAmounts[FrameState::kMaxTaskButtons] = {};
                    for (int i = 0; i < fits; ++i) rowAmounts[i] = 0.0f;
                    // Laid out from the row's own box, which is the island moved along by the button: the
                    // same box the renderer and the hit test use.
                    const RoundedBox rowBox = RowAnchorWithTray(trayIsland, true);
                    const RectF lastIcon = TaskIconButtonAtAmounts(rowBox, fits - 1, fits, rowAmounts);
                    // The WIDEST case: that last button opened, which is what the row is sized against.
                    const RectF lastIconOpen =
                        TaskIconButtonAt(rowBox, fits - 1, fits, fits - 1, 1.0f);
                    // The row must not run INTO the button, which is now to its LEFT as well -- so the test
                    // is against the button's left edge at one end and the monitor's margin at the other.
                    if (lastIcon.left < button.right || lastIconOpen.right > trayMonitor - kRowRightMarginDip) {
                        clear = false;
                        std::swprintf(trayDetail, 256,
                                      L"%d windows -> %d icons: row starts at %.0f (button ends %.0f), open "
                                      L"row ends at %.0f (monitor margin %.0f)",
                                      wanted, fits, lastIcon.left, button.right, lastIconOpen.right,
                                      trayMonitor - kRowRightMarginDip);
                    }
                }
                Check(clear, L"the application row ends before the tray button, however many windows are "
                             L"open",
                      trayDetail);
            }

            // The button's own hit test, and the card's: a control that could not be reached would be drawn
            // and completely inert -- the defect the application icons had before the union was extended for
            // them.
            {
                const RectF button = TrayButtonFor(trayIsland);
                const float centreX = (button.left + button.right) * 0.5f;
                const float centreY = (button.top + button.bottom) * 0.5f;

                // ... and a cell of the card's grid is a cell of that grid: two rows down and one along is
                // icon number 6, and its neighbour to the left is a different one.
                const RoundedBox trayCellCard =
                    BoxForState(IslandState::Expanded,
                                AnchorForWindow(0.0f, kPillTopMarginDip, 2560.0f));
                const TrayPageLayout page = TrayPageFor(trayCellCard, kMaxTrayIcons);
                const RectF& sixth = page.cells[6];
                const int cellHit = TrayPageCellAt(trayCellCard, kMaxTrayIcons,
                                                   (sixth.left + sixth.right) * 0.5f,
                                                   (sixth.top + sixth.bottom) * 0.5f);
                const int cellMiss = TrayPageCellAt(trayCellCard, kMaxTrayIcons,
                                                    page.cells[0].left - 6.0f,
                                                    page.cells[0].top + 4.0f);
                std::swprintf(trayDetail, 256,
                              L"button centre %ld,%ld; cell 6 centre -> %d, left of the grid -> %d",
                              static_cast<LONG>(centreX), static_cast<LONG>(centreY), cellHit,
                              cellMiss);
                Check(cellHit == 6 && cellMiss < 0,
                      L"a point on a tray cell is that icon and the margin beside it is not",
                      trayDetail);

                // --- the tray's OWN menu (2026-10-14, HANDOFF 8.80) -------------------------------
                //
                // A right click on a cell used to ask the shell for the application's menu: 300-600 ms,
                // the shell's panel on screen (until 8.79), and the menu at the corner of the screen
                // instead of under the pointer. This menu is the bar's own, and these are the two rules it
                // runs on: WHICH rows it offers, and WHERE they land.
                {
                    // Which rows: THE FOUR THE USER ASKED FOR, in his order -- 打开 / 文件位置 / 应用菜单 /
                    // 关闭应用 (2026-10-16) -- and an entry that would do nothing is still not offered. A row
                    // whose message cannot be sent (no callback learned) or whose path is unknown is absent,
                    // which is the rule that was measured the hard way on 2026-10-15: the user pressed
                    // 应用自己的菜单… on such a cell and it "did nothing", because all it could do was
                    // re-open the same menu.
                    //
                    // 打开 and 应用菜单 are the application's own left and right actions, so both need a
                    // callback; 文件位置 needs a recorded path; 关闭应用 always has something to do.
                    TrayMenuAction all[kMaxTrayMenuEntries] = {};
                    const int withEverything = TrayMenuEntriesFor(TrayMenuFacts{true, true, true}, all,
                                                                  kMaxTrayMenuEntries);
                    TrayMenuAction noCallback[kMaxTrayMenuEntries] = {};
                    const int withNoCallback = TrayMenuEntriesFor(TrayMenuFacts{true, true, false}, noCallback,
                                                                  kMaxTrayMenuEntries);
                    TrayMenuAction bare[kMaxTrayMenuEntries] = {};
                    const int withNothing = TrayMenuEntriesFor(TrayMenuFacts{false, false, false}, bare,
                                                               kMaxTrayMenuEntries);
                    TrayMenuAction onlyPath[kMaxTrayMenuEntries] = {};
                    const int withPath = TrayMenuEntriesFor(TrayMenuFacts{false, true, true}, onlyPath,
                                                            kMaxTrayMenuEntries);
                    std::swprintf(trayDetail, 256,
                                  L"everything=%d (last=%d), no callback=%d, nothing=%d, path only=%d",
                                  withEverything,
                                  withEverything > 0
                                      ? static_cast<int>(all[withEverything - 1])
                                      : -1,
                                  withNoCallback, withNothing, withPath);
                    Check(withEverything == 4 && all[0] == TrayMenuAction::Open &&
                              all[1] == TrayMenuAction::OpenLocation &&
                              all[2] == TrayMenuAction::ApplicationMenu &&
                              all[3] == TrayMenuAction::QuitApplication &&
                              // no callback: the two rows that ARE the application's own action go, and the
                              // two that do not need it stay.
                              withNoCallback == 2 && noCallback[0] == TrayMenuAction::OpenLocation &&
                              noCallback[1] == TrayMenuAction::QuitApplication &&
                              // nothing known at all: one row, and it is the one that always has something
                              // to do. path only, with a callback: three, because there is no window but
                              // there is a path -- and 关闭应用 is still there.
                              withNothing == 1 && bare[0] == TrayMenuAction::QuitApplication &&
                              withPath == 4 && onlyPath[0] == TrayMenuAction::Open &&
                              onlyPath[1] == TrayMenuAction::OpenLocation &&
                              onlyPath[2] == TrayMenuAction::ApplicationMenu &&
                              onlyPath[3] == TrayMenuAction::QuitApplication,
                          L"the menu offers only what can be done, and never a row that would do nothing",
                          trayDetail);
                    // THE ROW THAT TELLS THE TRUTH ABOUT ITSELF (2026-10-16, round 44). When an icon's own menu
                    // has been shown to be out of reach -- both shapes a right press may send were tried and the
                    // application answered neither -- the row stays (the press opens this bar's menu) and says
                    // so. A row promising the application's menu and delivering this bar's is the quiet lie
                    // this round was about.
                    Check(std::wcscmp(TrayMenuLabel(TrayMenuAction::ApplicationMenu, true),
                                      L"应用菜单（本机拿不到）") == 0 &&
                              std::wcscmp(TrayMenuLabel(TrayMenuAction::ApplicationMenu), L"应用菜单") == 0 &&
                              std::wcscmp(TrayMenuLabel(TrayMenuAction::Open, true), L"打开") == 0 &&
                              std::wcscmp(TrayMenuLabel(TrayMenuAction::QuitApplication, true),
                                          L"关闭应用") == 0,
                          L"only the 应用菜单 row changes when its menu cannot be reached, and it says so",
                          trayDetail);
                    // The one entry with a second state says so in words, and only that one does.
                    wchar_t armedDetail[192];
                    std::swprintf(armedDetail, 192, L"'%ls' / '%ls'",
                                  TrayMenuArmedLabel(TrayMenuAction::QuitApplication),
                                  TrayMenuArmedLabel(TrayMenuAction::Open));
                    Check(std::wcscmp(TrayMenuArmedLabel(TrayMenuAction::QuitApplication),
                                      L"再点一次：关闭应用") == 0 &&
                              std::wcscmp(TrayMenuArmedLabel(TrayMenuAction::Open), L"打开") == 0,
                          L"an armed entry says what the second press will do, and only it changes",
                          armedDetail);
                }
                {
                    // Where: the box hangs off the point it was opened for, and stays inside the card --
                    // the card is what the window's input region covers, so a menu outside it would be
                    // drawn where clicks cannot reach.
                    const RoundedBox menuCard =
                        BoxForState(IslandState::Expanded,
                                    AnchorForWindow(0.0f, kPillTopMarginDip, 2560.0f));
                    const float midX = (menuCard.rect.left + menuCard.rect.right) * 0.5f;
                    const float midY = (menuCard.rect.top + menuCard.rect.bottom) * 0.5f;
                    const TrayMenuLayout middle = TrayMenuFor(menuCard, midX, midY, 4);
                    // A pointer in the card's bottom-right corner: the menu has to flip, not overflow.
                    const TrayMenuLayout corner =
                        TrayMenuFor(menuCard, menuCard.rect.right - 2.0f, menuCard.rect.bottom - 2.0f, 4);
                    // ... and one at the top-left, where there is no room below or to the right either.
                    const TrayMenuLayout topLeft = TrayMenuFor(menuCard, menuCard.rect.left + 2.0f,
                                                              menuCard.rect.top + 2.0f, 4);
                    const bool rowsInside =
                        middle.rows[3].bottom <= middle.box.bottom + 0.01f &&
                        middle.box.left >= menuCard.rect.left && middle.box.right <= menuCard.rect.right;
                    const bool cornerInside = corner.box.left >= menuCard.rect.left &&
                                              corner.box.right <= menuCard.rect.right &&
                                              corner.box.top >= menuCard.rect.top &&
                                              corner.box.bottom <= menuCard.rect.bottom;
                    const bool topLeftInside = topLeft.box.left >= menuCard.rect.left &&
                                               topLeft.box.right <= menuCard.rect.right &&
                                               topLeft.box.top >= menuCard.rect.top &&
                                               topLeft.box.bottom <= menuCard.rect.bottom;
                    const int rowHit = TrayMenuRowAt(middle, (middle.rows[1].left + middle.rows[1].right) *
                                                                  0.5f,
                                                     (middle.rows[1].top + middle.rows[1].bottom) * 0.5f);
                    const int rowMiss =
                        TrayMenuRowAt(middle, middle.box.left - 30.0f, middle.box.top - 30.0f);
                    const bool rowsOrdered = middle.rows[0].bottom <= middle.rows[1].top + 0.01f &&
                                             middle.rows[1].bottom <= middle.rows[2].top + 0.01f;
                    std::swprintf(trayDetail, 256,
                                  L"middle %ld..%ld x %ld..%ld, corner %ld..%ld, row hit %d miss %d",
                                  static_cast<LONG>(middle.box.left), static_cast<LONG>(middle.box.right),
                                  static_cast<LONG>(middle.box.top), static_cast<LONG>(middle.box.bottom),
                                  static_cast<LONG>(corner.box.left),
                                  static_cast<LONG>(corner.box.right), rowHit, rowMiss);
                    Check(middle.count == 4 && rowsInside && cornerInside && topLeftInside &&
                              rowsOrdered && rowHit == 1 && rowMiss < 0,
                          L"the menu hangs off its point, stays in the card, and its rows are hit-tested",
                          trayDetail);
                }

                MonitorGeometry trayMon;
                trayMon.screen = {0, 0, 2560, 1440};
                trayMon.work = {0, 32, 2560, 1408};
                trayMon.dpi = 96;
                trayMon.dpiY = 96;
                const RECT trayWindow = {0, 0, 2560, 320};
                const POINT onTheTray = {static_cast<LONG>(centreX), static_cast<LONG>(centreY)};
                std::swprintf(trayDetail, 256, L"tray button centre %ld,%ld", onTheTray.x, onTheTray.y);
                Check(IslandWindow::ShouldAcceptPointer(trayMon, trayWindow, onTheTray,
                                                        IslandState::Collapsed, 0, 0.0f, false, 1),
                      L"the window accepts the pointer on the tray button", trayDetail);
                Check(!IslandWindow::ShouldAcceptPointer(trayMon, trayWindow, onTheTray,
                                                         IslandState::Collapsed, 0, 0.0f, false, 0),
                      L"and does not when there is no tray to show", trayDetail);
            }

            // THE WINDOW REGION MUST COVER IT, or the tray is drawn and clipped -- measured on the way to
            // this feature: sixteen icons found, handed to the frame, and not one pixel on screen, because
            // the region stopped at the application row. (The same rule in its newest form: with the button
            // beside the ISLAND, the region has to reach past the button -- which it does by reaching the
            // row's own right edge, since the row is laid out from the box the button moves along.)
            {
                MonitorGeometry trayMon;
                trayMon.screen = {0, 0, 2560, 1440};
                trayMon.work = {0, 32, 2560, 1408};
                trayMon.dpi = 96;
                trayMon.dpiY = 96;
                const IslandAnchor trayAnchor = AnchorForWindow(0.0f, kPillTopMarginDip, 2560.0f);
                const RECT withTray = IslandWindow::InputRegionFor(
                    trayMon, IslandState::Collapsed, trayAnchor, RoundedBox{}, 0.0f, -1, 0.0f,
                    kMaxTrayIcons);
                // The button, and the row that starts after it: both inside the region.
                const RoundedBox restPill =
                    IslandBoxFor(2560.0f, kCollapsedWidthDip, kCollapsedHeightDip);
                const RectF button = TrayButtonFor(restPill);
                const float rowRight =
                    TaskIconButtonAt(restPill, 0, 1, -1).right;   // one icon, at rest
                std::swprintf(trayDetail, 256,
                              L"button ends at %.0f, region at %ld px, one-icon row ends at %.0f",
                              button.right, withTray.right, rowRight);
                Check(withTray.right >= static_cast<LONG>(button.right) &&
                          withTray.right >= static_cast<LONG>(rowRight),
                      L"the region covers the tray button and the row beside it", trayDetail);

                // ... AND THE WHOLE SURFACE THE FIXED CONTROLS STAND ON, on the LEFT as well.
                //
                // That surface reaches a pad further left than the system button, and the region is what
                // CLIPS the window: measured from the system button, the region stopped 4 DIP short of what
                // is painted and the left end of the capsule came out with a square bite out of it, where the
                // round end should be. Reported with a photograph: "看一下 这个地方缺了一角".
                const RoundedBox surface = BarClusterFor(restPill, true);
                std::swprintf(trayDetail, 256,
                              L"surface left %.0f, region left %ld px; surface y [%.0f,%.0f], region y "
                              L"[%ld,%ld]",
                              surface.rect.left, withTray.left, surface.rect.top, surface.rect.bottom,
                              withTray.top, withTray.bottom);
                Check(withTray.left <= static_cast<LONG>(surface.rect.left) &&
                          withTray.top <= static_cast<LONG>(surface.rect.top) &&
                          withTray.bottom >= static_cast<LONG>(surface.rect.bottom),
                      L"the region covers the one surface the deck, the island and the tray stand on",
                      trayDetail);

                // NOT asserted any more: that asking for the tray WIDENS the region. It did while the button
                // stood at the window's right margin -- the region then had to be pushed out to reach it.
                // Beside the ISLAND the button is between two things the region already covers, and the
                // property that replaced it is the one asserted where the button is laid out: the row
                // starts after the button.
            }

            // PRESSING AN ICON IN THE CARD: the identity, the position, and the test that picks the shell's
            // button out of the ones on screen.
            //
            // The press itself cannot be exercised here -- it shows the shell's tray for a moment and
            // presses a real application's icon, which a self-test has no business doing -- but its three
            // ingredients can be, and they are where the defects were:
            //
            //   * every icon carries a shell identity (a UID or a GUID), or pressing it is impossible;
            //   * the shell answers where the icons it displays ARE, and does not answer for the ones it
            //     only has registered -- which is what tells a cell that can be pressed from one that
            //     cannot;
            //   * the rectangle the shell gives is matched to a UI Automation element by POSITION, and that
            //     match has to be exact: a press on the wrong rectangle opens the wrong application.
            {
                TrayIcons live;
                const int count = live.Refresh();
                wchar_t detail[256];
                std::swprintf(detail, 256, L"the live tray holds %d icon(s)", count);
                Check(count > 0, L"the tray list has something in it to press", detail);

                int withIdentity = 0;
                for (int i = 0; i < count; ++i) {
                    const TrayIcon& icon = live.At(i);
                    if (icon.hasUid || icon.hasGuid) ++withIdentity;
                }
                std::swprintf(detail, 256, L"%d of %d carry a UID or a GUID", withIdentity, count);
                Check(withIdentity == count,
                      L"every icon in the card carries the identity an application registers it with",
                      detail);

                // ... AND WHAT A PRESS NEEDS IS THE APPLICATION'S CALLBACK, not a position: the icons that
                // have one are pressable, the rest open this bar's own menu (see TrayPress.h). Asserted
                // loosely on purpose -- how many are learned depends on what is running and on what allows a
                // module inside it, so the number is reported rather than demanded.
                const int covered = CountCoveredIcons(live);
                std::swprintf(detail, 256,
                              L"%d of %d icon(s) have a learned callback (cache: %ls)", covered, count,
                              L"tray_callbacks.txt");
                Check(covered >= 0 && covered <= count,
                      L"the callback cache answers for the tray it describes and for nobody else", detail);
                Info(L"tray presses go by learned callback", detail);
            }
            {
                // Every result has a sentence, because these are what the log shows a user who asks why a
                // press did not work. A case the switch forgot would print "?".
                const TrayPressResult all[] = {
                    TrayPressResult::Pressed, TrayPressResult::NoEntry, TrayPressResult::NoWindow,
                    TrayPressResult::Refused,
                };
                bool named = true;
                for (TrayPressResult result : all) {
                    const wchar_t* text = TrayPressResultName(result);
                    if (!text || !*text || std::wcscmp(text, L"?") == 0) named = false;
                }
                Check(named, L"every press result can be said in words");
            }
        }

        // The input region must be as tall as the shape that is DRAWN, including the card.
        //
        // The region is what the window manager uses to decide which pixels belong to the bar, so a
        // region shorter than the drawing CLIPS it: the card is filled and then cut off at the pill's
        // height, which on screen is a wide dark band with no content in it -- reported exactly that way,
        // as "it has turned into this".
        //
        // The bug is that the region was derived from the RESTING island rather than from the state's
        // own shape. It survived because the music card reached only 22 DIP past the resting bottom and
        // no one had looked closely; the system panel's card is the same shape but the REPORT was
        // clearer.
        {
            MonitorGeometry mon;
            mon.screen = {0, 0, 2560, 1440};
            mon.work = {0, 0, 2560, 1408};
            mon.dpi = 96;
            mon.dpiY = 96;

            const IslandAnchor a = AnchorForWindow(0.0f, kPillTopMarginDip, 2560.0f);

            wchar_t dt5[256];
            for (int i = 0; i < 2; ++i) {
                const IslandState st = i == 0 ? IslandState::Collapsed : IslandState::Expanded;
                const RoundedBox shape = BoxForState(st, a);
                const RECT region = IslandWindow::InputRegionFor(mon, st, a);
                std::swprintf(dt5, 256, L"%ls: shape bottom %.0f DIP = %.0f px, region bottom %ld px",
                              StateName(st), shape.rect.bottom, shape.rect.bottom, region.bottom);
                Check(region.bottom >= static_cast<LONG>(shape.rect.bottom) - 1,
                      L"the input region covers the whole shape that is drawn", dt5);
            }
        }

        // The system panel's grid: two rows of three, inside the card, above the slider.
        //
        // The panel is a settings surface, so the two things that make it unusable are a tile hanging
        // off the card and a tile overlapping the slider. Both are invisible in a screenshot of a
        // corner of the panel and both are arithmetic, so they are asserted.
        {
            const IslandAnchor a = AnchorForWindow(0.0f, kPillTopMarginDip, 2560.0f);
            const RoundedBox gridCard = BoxForState(IslandState::Expanded, a);

            wchar_t dt6[256];
            for (int tiles = 1; tiles <= kMaxSystemTiles; ++tiles) {
                const SystemPanelLayout grid = SystemPanelFor(gridCard, tiles, 1);
                bool allInside = true;
                bool allAboveSlider = true;
                for (int i = 0; i < grid.tileCount; ++i) {
                    const RectF& r = grid.tiles[i];
                    if (r.left < gridCard.rect.left - 0.01f || r.right > gridCard.rect.right + 0.01f ||
                        r.top < gridCard.rect.top - 0.01f || r.bottom > gridCard.rect.bottom + 0.01f) {
                        allInside = false;
                    }
                    if (grid.sliderCount > 0 && r.bottom > grid.sliders[0].top + 0.01f) {
                        allAboveSlider = false;
                    }
                }
                std::swprintf(dt6, 256,
                              L"%d tiles: grid [%.0f..%.0f] x [%.0f..%.0f], slider top %.0f, "
                              L"card [%.0f..%.0f] x [%.0f..%.0f]",
                              tiles, grid.tiles[0].left, grid.tiles[tiles - 1].right,
                              grid.tiles[0].top, grid.tiles[tiles - 1].bottom,
                              grid.sliderCount > 0 ? grid.sliders[0].top : 0.0f, gridCard.rect.left,
                              gridCard.rect.right, gridCard.rect.top, gridCard.rect.bottom);
                Check(allInside, L"every tile is inside the card", dt6);
                Check(allAboveSlider, L"no tile overlaps the slider", dt6);
            }

            // And the hit test agrees with the drawing: the centre of a tile hits that tile.
            const SystemPanelLayout six = SystemPanelFor(gridCard, 6, 1);
            for (int i = 0; i < 6; ++i) {
                const float cx = (six.tiles[i].left + six.tiles[i].right) * 0.5f;
                const float cy = (six.tiles[i].top + six.tiles[i].bottom) * 0.5f;
                std::swprintf(dt6, 256, L"tile %d centre (%.0f,%.0f) -> %d", i, cx, cy,
                              SystemTileAt(card, 6, cx, cy));
                Check(SystemTileAt(card, 6, cx, cy) == i, L"a tile's centre hits that tile", dt6);
            }
            // And a point below the grid is not a tile, so the slider area is not swallowed.
            std::swprintf(dt6, 256, L"below the grid -> %d",
                          SystemTileAt(card, 6, (gridCard.rect.left + gridCard.rect.right) * 0.5f,
                                       six.sliders[0].top + 4.0f));
            Check(SystemTileAt(card, 6, (gridCard.rect.left + gridCard.rect.right) * 0.5f,
                               six.sliders[0].top + 4.0f) == -1,
                  L"a point on the slider is not a tile", dt6);

            // The grid FILLS the space down to the slider -- there is no dead band under it.
            //
            // The tiles used to take their designed 46 DIP whatever the card's height, which on a 250 DIP card
            // left 56 DIP of nothing between the last row and the slider. Asserted as a property rather than
            // as a number, because the number follows from the card's own height.
            {
                // WITH THE BRIGHTNESS ROW, which is the shape this panel is laid out for when the display
                // answers (DDC/CI): two sliders under the grid, and the grid fills the space exactly.
                const SystemPanelLayout sixTwo = SystemPanelFor(gridCard, 6, 2);
                const float expectedBottom = sixTwo.sliders[0].top - 10.0f;
                std::swprintf(dt6, 256,
                              L"last slot ends at %.1f, slider starts at %.1f, the band between is %.1f DIP",
                              sixTwo.tileLabels[5].bottom, sixTwo.sliders[0].top,
                              sixTwo.sliders[0].top - sixTwo.tileLabels[5].bottom);
                Check(Nearly(sixTwo.tileLabels[5].bottom, expectedBottom, 0.75f),
                      L"the tile grid fills the space down to the slider", dt6);

                // WITH ONE SLIDER there is a band, and it is BOUNDED rather than unbounded: the tiles are
                // capped at kSystemTileMaxHeightDip so a tall card cannot turn them into slabs, and the 280 DIP
                // card exists for the two-row panel. A display that does not answer for its brightness
                // therefore gets a slightly airier grid -- the tiles stay inside the card and clear of the
                // slider (asserted above for every tile count), and this is the ceiling on the gap.
                const float oneSliderBand = six.sliders[0].top - six.tileLabels[5].bottom;
                std::swprintf(dt6, 256, L"one slider: the band under the grid is %.1f DIP (ceiling 45)",
                              oneSliderBand);
                Check(oneSliderBand <= 45.0f,
                      L"without a brightness row the grid's gap is bounded, not a hole", dt6);
                // And the CAPTION stands under the box rather than inside it, as the shell's own flyout has it:
                // the box carries the glyph (and a chevron), the caption under it the state or the name.
                Check(six.tileLabels[0].top >= six.tiles[0].bottom - 0.25f &&
                          six.tileLabels[0].bottom <= six.tiles[0].bottom + 30.0f,
                      L"a tile's caption sits below its box", dt6);
                Check(six.tileIcons[0].left >= six.tiles[0].left + 8.0f &&
                          six.tileIcons[0].left <= six.tiles[0].left + 20.0f,
                      L"the glyph sits at the box's left, not in a corner", dt6);
            }

            // The paging column: reserved OUT of the grid, inside the card, and the grid narrows for it.
            //
            // The dots and the two arrows are controls, so they have to be where the drawing puts them and the
            // hit test has to agree -- which is asserted by asking for the hit at the centre of each.
            {
                const RoundedBox pagedCard = gridCard;
                const SystemPanelLayout paged = SystemPanelFor(pagedCard, 6, 1, /*paging=*/true, 0, 2);
                const SystemPanelLayout plain = SystemPanelFor(pagedCard, 6, 1);

                std::swprintf(dt6, 256,
                              L"paged tiles [%.0f..%.0f] vs plain [%.0f..%.0f]; dots %d; arrows y %.0f, %.0f",
                              paged.tiles[0].left, paged.tiles[2].right, plain.tiles[0].left,
                              plain.tiles[2].right, paged.pageDotCount, paged.pageUp.top,
                              paged.pageDown.bottom);
                Check(paged.pageDotCount == 2 && paged.tiles[2].right < plain.tiles[2].right - 10.0f,
                      L"paging reserves a column and the grid narrows for it", dt6);
                Check(paged.pageDots[0].right <= pagedCard.rect.right &&
                          paged.pageUp.top >= pagedCard.rect.top &&
                          paged.pageDown.bottom <= pagedCard.rect.bottom,
                      L"the paging column is inside the card", dt6);

                const float arrowX = (paged.pageUp.left + paged.pageUp.right) * 0.5f;
                const float upY = (paged.pageUp.top + paged.pageUp.bottom) * 0.5f;
                const float downY = (paged.pageDown.top + paged.pageDown.bottom) * 0.5f;
                const float gearX = (paged.gearButton.left + paged.gearButton.right) * 0.5f;
                const float gearY = (paged.gearButton.top + paged.gearButton.bottom) * 0.5f;
                Check(SystemPanelHitFor(pagedCard, 6, 1, 0, 2, arrowX, upY).target ==
                          SystemPanelTarget::PageUp,
                      L"the up arrow hits as the up arrow", dt6);
                Check(SystemPanelHitFor(pagedCard, 6, 1, 0, 2, arrowX, downY).target ==
                          SystemPanelTarget::PageDown,
                      L"the down arrow hits as the down arrow", dt6);
                Check(SystemPanelHitFor(pagedCard, 6, 1, 0, 2, gearX, gearY).target ==
                          SystemPanelTarget::Gear,
                      L"the settings button hits as the settings button", dt6);
            }

            // The panel's three ROW controls, which arrived with the volume button: the slider, the speaker
            // and the output-device button at the slider's right.
            //
            // Each one is asserted against the rectangle the layout drew, not against a remembered number:
            // the failure this guards against is a control that is drawn in one place and pressed in another,
            // which is exactly what the tiles and the music controls did before SystemPanelFor became the one
            // source of this geometry.
            {
                const SystemPanelLayout sliderRow = SystemPanelFor(gridCard, 6, 1);
                const float centreY = (sliderRow.sliders[0].top + sliderRow.sliders[0].bottom) * 0.5f;
                const float trackLeft = sliderRow.sliders[0].left;
                const float trackRight = sliderRow.sliders[0].right;

                std::swprintf(dt6, 256,
                              L"slider [%.0f..%.0f], speaker [%.0f..%.0f], device button [%.0f..%.0f], "
                              L"card right %.0f",
                              trackLeft, trackRight, sliderRow.speakerIcon.left,
                              sliderRow.speakerIcon.right, sliderRow.volumeButton.left,
                              sliderRow.volumeButton.right, gridCard.rect.right);
                Check(sliderRow.volumeButton.right <= gridCard.rect.right + 0.01f &&
                          sliderRow.volumeButton.left > trackRight + 0.5f,
                      L"the output-device button is inside the card and clear of the track", dt6);
                Check(SystemPanelHitFor(gridCard, 6, 1, 0, 1, trackLeft + (trackRight - trackLeft) * 0.5f,
                                        centreY).target == SystemPanelTarget::Slider,
                      L"the middle of the track hits the slider", dt6);
                Check(Nearly(SystemPanelHitFor(gridCard, 6, 1, 0, 1, trackRight, centreY).fraction, 1.0f,
                             0.02f),
                      L"a press at the track's right end is full volume", dt6);
                Check(Nearly(SystemPanelHitFor(gridCard, 6, 1, 0, 1, trackLeft, centreY).fraction, 0.0f,
                             0.02f),
                      L"a press at the track's left end is silence", dt6);
                Check(SystemPanelHitFor(gridCard, 6, 1, 0, 1,
                                        (sliderRow.speakerIcon.left + sliderRow.speakerIcon.right) * 0.5f,
                                        centreY)
                          .target == SystemPanelTarget::Speaker,
                      L"the speaker glyph hits as the mute control", dt6);
                Check(SystemPanelHitFor(gridCard, 6, 1, 0, 1,
                                        (sliderRow.volumeButton.left + sliderRow.volumeButton.right) * 0.5f,
                                        centreY)
                          .target == SystemPanelTarget::VolumeDevice,
                      L"the output-device button hits as the way to the sound page", dt6);
            }

            // THE SECOND SLIDER, the brightness row, and the three things about it that only exist because
            // there are two: its own glyph, its own hit index, and a track that uses the width the volume row
            // cannot (the output-device button owns that row's right end).
            {
                const SystemPanelLayout two = SystemPanelFor(gridCard, 6, 2);
                const float brightnessCentreY = (two.sliders[1].top + two.sliders[1].bottom) * 0.5f;
                std::swprintf(dt6, 256,
                              L"rows: volume y [%.0f..%.0f], brightness y [%.0f..%.0f]; tracks "
                              L"[%.0f..%.0f] and [%.0f..%.0f]",
                              two.sliders[0].top, two.sliders[0].bottom, two.sliders[1].top,
                              two.sliders[1].bottom, two.sliders[0].left, two.sliders[0].right,
                              two.sliders[1].left, two.sliders[1].right);

                Check(two.sliderCount == 2 && two.brightnessIcon.right > two.brightnessIcon.left,
                      L"the brightness row has its own glyph", dt6);
                Check(two.sliders[1].top > two.sliders[0].bottom - 0.01f,
                      L"the brightness row sits under the volume row, never on it", dt6);
                Check(two.sliders[1].right > two.sliders[0].right + 10.0f,
                      L"the brightness track uses the width the device button takes from the volume row",
                      dt6);
                Check(two.sliders[1].right <= gridCard.rect.right - 6.0f,
                      L"and it still stops inside the card", dt6);

                // The hit test has to say WHICH row, or a press on one of them would move the other: the
                // index is what the drag follows for the rest of the gesture.
                const SystemPanelHit onBrightness = SystemPanelHitFor(
                    gridCard, 6, 2, 0, 1, two.sliders[1].left + 1.0f, brightnessCentreY);
                const SystemPanelHit onVolume = SystemPanelHitFor(
                    gridCard, 6, 2, 0, 1, two.sliders[0].left + 1.0f,
                    (two.sliders[0].top + two.sliders[0].bottom) * 0.5f);
                std::swprintf(dt6, 256, L"brightness row -> target %d index %d; volume row -> target %d index %d",
                              static_cast<int>(onBrightness.target), onBrightness.index,
                              static_cast<int>(onVolume.target), onVolume.index);
                Check(onBrightness.target == SystemPanelTarget::Slider && onBrightness.index == 1 &&
                          onVolume.target == SystemPanelTarget::Slider && onVolume.index == 0,
                      L"each row reports itself, not the other", dt6);

                // The row's own reading, at the two ends: full and empty.
                Check(Nearly(SystemPanelHitFor(gridCard, 6, 2, 0, 1, two.sliders[1].left,
                                               brightnessCentreY).fraction, 0.0f, 0.02f) &&
                          Nearly(SystemPanelHitFor(gridCard, 6, 2, 0, 1, two.sliders[1].right,
                                                   brightnessCentreY).fraction, 1.0f, 0.02f),
                      L"the brightness track reads left to right like the volume one", dt6);
            }

            // The night light state, on the three blobs that were captured on this machine rather than on a
            // shape written from memory: the 13-byte off, the 41-byte off-with-history, and the 43-byte on.
            //
            // LENGTH IS THE TRAP HERE, which is why all three are here: a parse that went by length would
            // read the 43-byte "was switched off" blob as ON and the first one as OFF by luck.
            {
                const unsigned char offFresh[] = {0x43, 0x42, 0x01, 0x00, 0x0A, 0x02, 0x01, 0x00,
                                                  0x2A, 0x2A, 0x00, 0x00, 0x00};
                const unsigned char offAgain[] = {
                    0x43, 0x42, 0x01, 0x00, 0x0A, 0x02, 0x01, 0x00, 0x2A, 0x06, 0xBD, 0xE3,
                    0xB9, 0xD6, 0x06, 0x2A, 0x2B, 0x0E, 0x13, 0x43, 0x42, 0x01, 0x00, 0xD0,
                    0x0A, 0x02, 0xC6, 0x14, 0xBA, 0xBB, 0xFC, 0xDC, 0xCB, 0xE7, 0xD6, 0xEE,
                    0x01, 0x00, 0x00, 0x00, 0x00};
                const unsigned char on[] = {
                    0x43, 0x42, 0x01, 0x00, 0x0A, 0x02, 0x01, 0x00, 0x2A, 0x06, 0xBA, 0xE3,
                    0xB9, 0xD6, 0x06, 0x2A, 0x2B, 0x0E, 0x15, 0x43, 0x42, 0x01, 0x00, 0x10,
                    0x00, 0xD0, 0x0A, 0x02, 0xC6, 0x14, 0xB3, 0xD4, 0xFA, 0xCD, 0xCB, 0xE7,
                    0xD6, 0xEE, 0x01, 0x00, 0x00, 0x00, 0x00};

                std::swprintf(dt6, 256, L"blobs: %zu, %zu and %zu bytes", sizeof(offFresh),
                              sizeof(offAgain), sizeof(on));
                Check(NightLightStateFromBlob(offFresh, sizeof(offFresh)) == NightLightState::Off &&
                          NightLightStateFromBlob(offAgain, sizeof(offAgain)) == NightLightState::Off &&
                          NightLightStateFromBlob(on, sizeof(on)) == NightLightState::On,
                      L"the three measured blobs read off, off and on", dt6);
                Check(NightLightStateFromBlob(nullptr, 0) == NightLightState::Unknown &&
                          NightLightStateFromBlob(offFresh, 4) == NightLightState::Unknown,
                      L"a missing or truncated blob is Unknown, not a guess", dt6);
            }

            // THE THEMES, on the two things that decide whether switching one is a good idea: does a
            // theme FILE parse (the format a user types), and is the text still readable in every
            // theme that ships.
            //
            // The contrast floor is the plan's own requirement -- "do not over-transparent the
            // background so the text becomes unclear" -- turned into a number, because a theme whose
            // ink is a shade lighter than its surface looks fine in a file and unreadable on screen.
            {
                std::string why;
                Theme parsed;
                const char* good =
                    "# a theme written the way a user would write one\n"
                    "name=test\n"
                    "pill-top=0.02,0.03,0.06,0.90\n"
                    "pill-bottom=0.5\n"          // the one-component shorthand: a grey
                    "gloss=0.12\n"
                    "accent=0.2,0.6,1.0\n";
                Check(ThemeFromText(good, &parsed, &why),
                      L"a hand-written theme parses, and may state only what it changes",
                      L"name + three colours and a scalar");
                Check(parsed.gloss > 0.11f && parsed.gloss < 0.13f && parsed.pillBottom.r == 0.5f &&
                          parsed.accent.b == 1.0f && parsed.cardFill.a > 0.9f,
                      L"and the fields it did not mention keep the default's values",
                      L"pill-bottom=0.5 is the grey shorthand; cardFill comes from 默认");

                const char* noName = "accent=0.2,0.6,1.0\n";
                Check(!ThemeFromText(noName, &parsed, &why),
                      L"a theme with no name is refused (the name is what theme.txt stores)");
                const char* badColour = "name=x\naccent=1.4,0.2,0.2\n";
                Check(!ThemeFromText(badColour, &parsed, &why),
                      L"a channel outside 0..1 is refused rather than clamped");
                const char* badKey = "name=x\ncolour=0.2,0.2,0.2\n";
                Check(!ThemeFromText(badKey, &parsed, &why),
                      L"an unknown key is refused, so a typo cannot be silently ignored");

                for (int i = 0; i < BuiltInThemeCount(); ++i) {
                    const Theme& theme = BuiltInTheme(i);
                    const float contrast = ThemeInkContrast(theme);
                    std::swprintf(dt6, 256, L"theme '%ls': ink contrast %.1f:1", theme.name, contrast);
                    Check(contrast >= kInkContrastFloor, L"every built-in theme keeps its text readable",
                          dt6);
                }

                // The CARD ink is decided by the backdrop UNDER THE CARD, not by the island's: the system
                // panel and the Start menu hang below the bar, over a different part of the desktop. The
                // crossover is time-smoothed, so each sample gets a wait longer than one step (250 ms).
                {
                    island::SetCardInkBackdrop(0.05f);
                    Sleep(320);
                    const D2D1_COLOR_F onDark = island::CardInk(1.0f);
                    island::SetCardInkBackdrop(0.95f);
                    Sleep(320);
                    const D2D1_COLOR_F onLight = island::CardInk(1.0f);
                    std::swprintf(dt6, 256,
                                  L"card ink over dark %.2f/%.2f/%.2f, over light %.2f/%.2f/%.2f",
                                  onDark.r, onDark.g, onDark.b, onLight.r, onLight.g, onLight.b);
                    Check(onDark.r > 0.7f && onDark.g > 0.7f && onDark.b > 0.7f,
                          L"a dark backdrop under a card switches its words to the light ink", dt6);
                    Check(onLight.r < 0.3f && onLight.g < 0.3f && onLight.b < 0.3f,
                          L"a bright backdrop under a card switches its words to the dark ink", dt6);
                    island::SetCardInkBackdrop(0.5f);
                }

                // THE LIQUID-GLASS LAYERS (round 53). Two of them are pure arithmetic, which is the only
                // reason they can be asserted here at all: the sweep's centre and the wallpaper tint's
                // clamp. The third -- the drawing -- is checked by the screenshot, as every material is.
                {
                    // The sweep travels the INNER 80%: a pointer on either edge must not put the highlight
                    // on the corner, and a pointer outside the pill must not move it off the glass at all.
                    const float left = 100.0f;
                    const float right = 300.0f;
                    std::swprintf(dt6, 256, L"sweep centre: edge %.0f/%.0f, middle %.0f, past the edge %.0f",
                                  SpecularCentreX(left, right, 0.0f), SpecularCentreX(left, right, 1.0f),
                                  SpecularCentreX(left, right, 0.5f), SpecularCentreX(left, right, 4.0f));
                    Check(Nearly(SpecularCentreX(left, right, 0.0f), left + 20.0f, 0.01f) &&
                              Nearly(SpecularCentreX(left, right, 1.0f), right - 20.0f, 0.01f),
                          L"the specular sweep stays inside the pill at both ends", dt6);
                    Check(Nearly(SpecularCentreX(left, right, 0.5f), 200.0f, 0.01f),
                          L"... and sits in the middle when the pointer is", dt6);
                    Check(Nearly(SpecularCentreX(left, right, -3.0f),
                                 SpecularCentreX(left, right, 0.0f), 0.01f) &&
                              Nearly(SpecularCentreX(left, right, 4.0f),
                                     SpecularCentreX(left, right, 1.0f), 0.01f),
                          L"a pointer off the pill leaves the highlight on the glass", dt6);

                    // The tint, on the one thing that must not break: a bright wallpaper may lighten the
                    // pill, but the bar's own text has to stay readable over it -- the clamp walks the mix
                    // back until the contrast passes, so a WHITE wallpaper is the worst case to assert.
                    Theme tinted = BuiltInTheme(0);
                    tinted.tint = 1.0f;
                    const D2D1_COLOR_F white = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
                    const D2D1_COLOR_F dark = D2D1::ColorF(0.02f, 0.02f, 0.03f, 1.0f);
                    const D2D1_COLOR_F fromWhite =
                        TintedFill(tinted, tinted.cardFill, white, 1.0f);
                    const D2D1_COLOR_F fromDark = TintedFill(tinted, tinted.cardFill, dark, 1.0f);
                    std::swprintf(dt6, 256,
                                  L"tint 1.0 with a WHITE wallpaper: card %.2f -> %.2f (contrast floor %.1f)",
                                  tinted.cardFill.r, fromWhite.r, kInkContrastFloor);
                    Check(fromWhite.r < 0.9f,
                          L"a white wallpaper does not turn the card white", dt6);
                    // The clamp's own promise, asked of the answer rather than of the code: take the colour
                    // it handed back, put it on a theme, and ask the contrast floor again.
                    Theme asIf = tinted;
                    asIf.cardFill = fromWhite;
                    std::swprintf(dt6, 256, L"after the white-wallpaper tint the ink contrast is %.1f:1",
                                  ThemeInkContrast(asIf));
                    Check(ThemeInkContrast(asIf) >= kInkContrastFloor,
                          L"... and the text is still readable over the tinted surface", dt6);
                    Check(fromDark.r <= tinted.cardFill.r + 0.001f,
                          L"a dark wallpaper tints toward itself rather than away", dt6);
                    // ... and with the tint OFF (默认, which is what the fixed scenario runs) the colour is
                    // handed back untouched, bit for bit: that is what keeps the baseline's pixels valid.
                    const D2D1_COLOR_F untouched = TintedFill(BuiltInTheme(0), BuiltInTheme(0).cardFill,
                                                               white, 1.0f);
                    Check(untouched.r == BuiltInTheme(0).cardFill.r &&
                              untouched.a == BuiltInTheme(0).cardFill.a,
                          L"a theme with tint 0 gets its own colour back untouched");
                }

                // The grain, on the two properties that make it a material rather than a random number:
                // it is the SAME every run (a texture that changed between launches would be a different
                // material each time) and it is a narrow band around the middle rather than full-range
                // speckle, because it is drawn at 2-5%.
                const unsigned char* noise = NoisePixels();
                const int size = NoiseSize();
                Check(noise != nullptr && size == 256,
                      L"the grain is 256x256, as the material layer is specified", L"noise buffer");
                if (noise) {
                    unsigned long long sum = 0;
                    int low = 255;
                    int high = 0;
                    // Four bytes per pixel: the grey is the blue channel (they are all the same value).
                    for (int i = 0; i < size * size; ++i) {
                        const unsigned char grey = noise[i * 4];
                        sum += grey;
                        if (grey < low) low = grey;
                        if (grey > high) high = grey;
                    }
                    const int mean = static_cast<int>(sum / static_cast<unsigned long long>(size * size));
                    std::swprintf(dt6, 256, L"grain: mean %d, range %d..%d", mean, low, high);
                    // Centred near the middle and no wider than a few percent of the range: what the
                    // assertion is about is "grain, not speckle", so the numbers describe a narrow band.
                    Check(mean >= 120 && mean <= 135 && high - low <= 40,
                          L"the grain is a narrow band around the middle, not speckle", dt6);
                    // Determinism: the same buffer twice is the same bytes -- a cheap proxy for "the same
                    // bytes next launch", which the fixed seed gives.
                    bool stable = true;
                    const unsigned char* again = NoisePixels();
                    for (int i = 0; i < size * size && stable; i += 977) {
                        if (again[i * 4] != noise[i * 4]) stable = false;
                    }
                    Check(stable, L"and it does not change under the caller's feet", dt6);
                }

                // THE WEATHER MARK AND THE WEATHER WORD COME FROM THE SAME CODE, which is the whole of the
                // fix for "天气这个图标对应错了 现在显示晴但是下雨的": the line drew one cloud-with-rain for
                // every code, so 晴 sat beside a rain cloud.
                //
                // Asserted over the whole WMO table, code by code, and cross-checked against the word
                // table: a code that has a WORD but no MARK (or the reverse) is the bug class itself -- the
                // two answers disagreeing -- so that is a check of its own rather than a coincidence.
                {
                    const int codes[] = {0,  1,  2,  3,  45, 48, 51, 53, 55, 56, 57, 61, 63, 65,
                                         66, 67, 71, 73, 75, 77, 80, 81, 82, 85, 86, 95, 96, 99};
                    int mismatched = 0;
                    int marked = 0;
                    for (int code : codes) {
                        const wchar_t* word = WeatherWordForCode(code);
                        const SkyMark mark = SkyMarkForCode(code);
                        if ((word != nullptr) != (mark != SkyMark::None)) ++mismatched;
                        if (mark != SkyMark::None) ++marked;
                    }
                    std::swprintf(dt6, 256,
                                  L"%d known codes: %d with a mark, %d where the word and the mark disagree",
                                  static_cast<int>(sizeof(codes) / sizeof(codes[0])), marked,
                                  mismatched);
                    Check(mismatched == 0,
                          L"every code that has a word has a mark, and the other way round", dt6);

                    // The reported case, named: a clear sky is the SUN, not a cloud and certainly not rain.
                    Check(SkyMarkForCode(0) == SkyMark::Sun && SkyMarkForCode(1) == SkyMark::Sun &&
                              SkyMarkForCode(61) == SkyMark::Rain &&
                              SkyMarkForCode(71) == SkyMark::Snow &&
                              SkyMarkForCode(95) == SkyMark::Thunder &&
                              SkyMarkForCode(2) == SkyMark::SunCloud &&
                              SkyMarkForCode(-1) == SkyMark::None,
                          L"and the reported case is the sun: 晴 is drawn as a sun, rain as rain", dt6);
                }
            }
        }

        // The 声音输出 page, the panel's second level.
        //
        // Three things make it unusable and all three are arithmetic: a row hanging off the card, a row that
        // cannot be scrolled to, and a control whose hit test disagrees with where it was drawn. A screenshot
        // of the top of the page says nothing about whether its bottom is reachable, which is why this is
        // asserted rather than looked at.
        {
            const IslandAnchor a = AnchorForWindow(0.0f, kPillTopMarginDip, 2560.0f);
            const RoundedBox pageCard = BoxForState(IslandState::Expanded, a);
            const int devices = 3;
            const int mixRows = 5;   // the output device itself, and four applications
            const SoundPageLayout top = SoundPageFor(pageCard, devices, mixRows, 0.0f);

            wchar_t ds[256];
            std::swprintf(ds, 256,
                          L"header %.0f, viewport [%.0f..%.0f], footer [%.0f..%.0f], card [%.0f..%.0f]",
                          kSoundHeaderHeightDip, top.viewport.top, top.viewport.bottom, top.footer.top,
                          top.footer.bottom, pageCard.rect.top, pageCard.rect.bottom);
            Check(top.backButton.left >= pageCard.rect.left &&
                      top.backButton.top >= pageCard.rect.top &&
                      top.backButton.bottom <= top.viewport.top + 0.01f,
                  L"the back arrow is in the header, inside the card", ds);
            Check(top.footer.bottom <= pageCard.rect.bottom + 0.01f &&
                      top.footer.top >= top.viewport.bottom - 0.01f,
                  L"the footer is pinned to the card's bottom, below the viewport", ds);

            bool rowsInside = true;
            for (int i = 0; i < top.deviceCount; ++i) {
                const RectF& r = top.deviceIcons[i];
                if (r.left < pageCard.rect.left || r.right > pageCard.rect.right ||
                    r.top < top.viewport.top || r.bottom > top.viewport.bottom) {
                    rowsInside = false;
                }
            }
            std::swprintf(ds, 256, L"first device icon [%.0f..%.0f] x [%.0f..%.0f], card [%.0f..%.0f]",
                          top.deviceIcons[0].left, top.deviceIcons[0].right, top.deviceIcons[0].top,
                          top.deviceIcons[0].bottom, pageCard.rect.left, pageCard.rect.right);
            Check(rowsInside, L"the device rows that are on screen are inside the viewport", ds);

            // The page really does scroll, and the LAST row really is reachable when it is scrolled to its
            // limit: a list whose tail cannot be brought into view is the failure this guards against.
            const float limit = SoundPageScrollLimit(pageCard, devices, mixRows);
            const SoundPageLayout bottom = SoundPageFor(pageCard, devices, mixRows, limit);
            const SoundPageLayout clamped = SoundPageFor(pageCard, devices, mixRows, limit + 500.0f);
            std::swprintf(ds, 256, L"content %.0f DIP, viewport %.0f, limit %.0f",
                          top.contentHeight, top.viewport.bottom - top.viewport.top, limit);
            Check(limit > 0.0f, L"the page's content is taller than the viewport, so it scrolls", ds);
            Check(Nearly(clamped.scroll, limit, 0.01f),
                  L"a scroll past the end is clamped to the last row", ds);
            Check(bottom.mixerRows[mixRows - 1].bottom <= bottom.viewport.bottom + 0.01f &&
                      bottom.mixerRows[mixRows - 1].top >= bottom.viewport.top - 0.01f,
                  L"the last mixer row is inside the viewport at the end of the scroll", ds);

            // The hit test, at the centre of each control, against the SAME layout that was drawn.
            std::swprintf(ds, 256, L"device row 1 centre -> %d",
                          static_cast<int>(SoundPageHitFor(
                              pageCard, devices, mixRows, 0.0f,
                              (top.deviceRows[1].left + top.deviceRows[1].right) * 0.5f,
                              (top.deviceRows[1].top + top.deviceRows[1].bottom) * 0.5f).target));
            Check(SoundPageHitFor(pageCard, devices, mixRows, 0.0f,
                                  (top.deviceRows[1].left + top.deviceRows[1].right) * 0.5f,
                                  (top.deviceRows[1].top + top.deviceRows[1].bottom) * 0.5f)
                      .target == SoundPageTarget::Device,
                  L"a device row hits as a device row", ds);
            Check(SoundPageHitFor(pageCard, devices, mixRows, 0.0f,
                                  (top.backButton.left + top.backButton.right) * 0.5f,
                                  (top.backButton.top + top.backButton.bottom) * 0.5f)
                      .target == SoundPageTarget::Back,
                  L"the back arrow hits as the way out");
            Check(SoundPageHitFor(pageCard, devices, mixRows, 0.0f,
                                  (top.footer.left + top.footer.right) * 0.5f,
                                  (top.footer.top + top.footer.bottom) * 0.5f)
                      .target == SoundPageTarget::Footer,
                  L"the footer hits as 更多音量设置");
            // The spatial rows are BELOW the fold at the top of the page -- which the failing version of this
            // check proved by asking for a hit there and getting none. So they are asked for where they can
            // actually be seen: scrolled until the first of them is at the top of the viewport. A row that is
            // scrolled out of the viewport answering "nothing" is the rule, not a bug.
            const float spatialScroll = top.spatialRows[0].top - top.viewport.top;
            const SoundPageLayout scrolled = SoundPageFor(pageCard, devices, mixRows, spatialScroll);
            std::swprintf(ds, 256, L"spatial row 0 at scroll %.0f: y [%.0f..%.0f], viewport [%.0f..%.0f]",
                          spatialScroll, scrolled.spatialRows[0].top, scrolled.spatialRows[0].bottom,
                          scrolled.viewport.top, scrolled.viewport.bottom);
            Check(scrolled.spatialRows[0].top >= scrolled.viewport.top - 0.01f,
                  L"the spatial rows can be scrolled into view", ds);
            Check(SoundPageHitFor(pageCard, devices, mixRows, spatialScroll,
                                  (scrolled.spatialRows[0].left + scrolled.spatialRows[0].right) * 0.5f,
                                  (scrolled.spatialRows[0].top + scrolled.spatialRows[0].bottom) * 0.5f)
                      .target == SoundPageTarget::Spatial,
                  L"a spatial row hits as a spatial row", ds);
            // ... and the same place, unscrolled, is not a device row: the clip is what stops a row that is
            // under the header from being pressable through it.
            Check(SoundPageHitFor(pageCard, devices, mixRows, 0.0f,
                                  (scrolled.spatialRows[0].left + scrolled.spatialRows[0].right) * 0.5f,
                                  (scrolled.spatialRows[0].top + scrolled.spatialRows[0].bottom) * 0.5f)
                      .target != SoundPageTarget::Spatial,
                  L"a scrolled-away row is not pressable at its unscrolled place");

            // The mixer's sliders: the fraction is taken from the track, so a press at either end means what
            // it looks like, and a press on the row is never mistaken for one on its neighbour.
            const SoundPageLayout mixerTop = SoundPageFor(pageCard, devices, mixRows, limit);
            const RectF& track = mixerTop.mixerTracks[0];
            const float trackCentreY = (track.top + track.bottom) * 0.5f;
            const SoundPageHit rightEnd =
                SoundPageHitFor(pageCard, devices, mixRows, limit, track.right, trackCentreY);
            const SoundPageHit leftEnd =
                SoundPageHitFor(pageCard, devices, mixRows, limit, track.left, trackCentreY);
            std::swprintf(ds, 256, L"first mixer track [%.0f..%.0f], hits %d/%.2f and %d/%.2f", track.left,
                          track.right, static_cast<int>(rightEnd.target), rightEnd.fraction,
                          static_cast<int>(leftEnd.target), leftEnd.fraction);
            Check(rightEnd.target == SoundPageTarget::MixerSlider && Nearly(rightEnd.fraction, 1.0f, 0.02f),
                  L"a press at the mixer track's right end is full volume", ds);
            Check(leftEnd.target == SoundPageTarget::MixerSlider && Nearly(leftEnd.fraction, 0.0f, 0.02f),
                  L"a press at the mixer track's left end is silence", ds);

            // The scrollbar: inside the card, and its thumb inside its own track.
            std::swprintf(ds, 256, L"track [%.0f..%.0f] x [%.0f..%.0f], thumb y [%.0f..%.0f]",
                          top.scrollTrack.left, top.scrollTrack.right, top.scrollTrack.top,
                          top.scrollTrack.bottom, top.scrollThumb.top, top.scrollThumb.bottom);
            Check(top.scrollTrack.right <= pageCard.rect.right + 0.01f &&
                      top.scrollTrack.left >= pageCard.rect.left &&
                      top.scrollThumb.top >= top.scrollTrack.top - 0.01f &&
                      top.scrollThumb.bottom <= top.scrollTrack.bottom + 0.01f,
                  L"the scrollbar and its thumb are inside the card and the track", ds);
        }

        // Hit testing lands on the right button, and on nothing past the end.
        {
            const RectF second = TaskIconButtonAt(island, 1, 4, -1, 0.0f);
            const float midX = (second.left + second.right) * 0.5f;
            const float midY = (second.top + second.bottom) * 0.5f;
            Check(TaskIconHit(island, 4, -1, 0.0f, midX, midY) == 1,
                  L"a click in the second button hits the second button");
            Check(TaskIconHit(island, 4, -1, 0.0f, island.rect.left, midY) == -1,
                  L"a click on the island is not an icon");

            // Past the LAST button, not past the second one.
            //
            // With four buttons on a 34 DIP pitch, the second button's right edge plus 40 DIP lands
            // inside the third. The first version of this assertion said "past the last button" and
            // tested a point in the middle of the row, so it failed against correct code.
            const RectF lastButton = TaskIconButtonAt(island, 3, 4, -1, 0.0f);
            Check(TaskIconHit(island, 4, -1, 0.0f, lastButton.right + 40.0f, midY) == -1,
                  L"a click past the last button hits nothing");
        }

        // How many fit: never more than asked for, never more than the cap, and the row must not run
        // off the monitor even when its last button is the widened one.
        {
            Check(TaskIconsThatFit(monitorWidth, 0) == 0, L"asking for no icons gives none");
            Check(TaskIconsThatFit(monitorWidth, 5) == 5, L"a small number of icons all fit");
            Check(TaskIconsThatFit(monitorWidth, 1000) <= kMaxTaskIcons,
                  L"the row never exceeds its cap");

            // THE USER'S CASE, in one line: nine windows on a 2560-wide screen used to show EIGHT icons,
            // because the row was sized with every button open (kTaskWideButtonDip is 132 -- an open
            // button shows its title) while the row uses 272 DIP of the 1162 available. Reported as
            // "灵动岛 右侧现在只能显示8个 再开个窗口就不显示了", confirmed against the bar's own log:
            // "8 buttons from 9 windows listed".
            wchar_t fitDetail[160];
            const int forNine = TaskIconsThatFit(2560.0f, 9);
            const int forMany = TaskIconsThatFit(2560.0f, 1000);
            std::swprintf(fitDetail, 160, L"9 wanted -> %d, many wanted -> %d", forNine, forMany);
            Check(forNine == 9, L"nine windows all show on a 2560-wide monitor", fitDetail);
            Check(forMany > 8, L"a wide monitor shows more than the eight it used to", fitDetail);

            const int fits = TaskIconsThatFit(monitorWidth, 1000);
            const RectF lastResting = TaskIconButtonAt(island, fits - 1, fits, -1, 0.0f);
            Check(fits > 0 && lastResting.right < monitorWidth,
                  L"the last icon that fits is still on the monitor");

            // The same row with its last button widened: still on the monitor, which is the case
            // that matters because that is what happens when the pointer reaches the end.
            const RectF lastWide = TaskIconButtonAt(island, fits - 1, fits, fits - 1, 1.0f);
            Check(lastWide.right < monitorWidth,
                  L"the row still fits when its last button is widened");
        }

        // A narrow monitor must yield fewer icons rather than overflowing.
        {
            const int narrow = TaskIconsThatFit(1024.0f, 1000);
            Check(narrow < TaskIconsThatFit(monitorWidth, 1000),
                  L"a narrower monitor fits fewer icons");
        }

    // An icon's arrival and departure: "灵动岛右侧 图标出现和消失有点太突兀了没有动画".
    //
    // A pure function because the defect it fixes cannot be seen in a still: an icon that jumps in and one
    // that fades in are the same picture once the fade is over.
    {
        const float step = island::kTaskIconAppearSeconds * 0.5f;
        const float half = TaskIconAppearStep(0.0f, /*arriving=*/true, step);
        Check(half > 0.4f && half < 0.6f, L"an arriving icon moves gradually");

        // Over the whole time -- four steps of half a duration -- it arrives, and it stops there rather than
        // running past the end.
        float arrived = 0.0f;
        for (int i = 0; i < 4; ++i) arrived = TaskIconAppearStep(arrived, true, step);
        Check(Nearly(arrived, 1.0f, 0.001f), L"... and reaches one");

        // Going out is the same motion, mirrored: the step away from 1 is the step towards it.
        const float leaving = TaskIconAppearStep(1.0f, /*arriving=*/false, step);
        Check(Nearly(1.0f - leaving, half, 0.001f),
              L"a departing icon moves by the same step");

        // A HITCH DOES NOT JUMP AN ICON IN. The step clamps its own dt, so a frame that took a tenth of a
        // second longer than it should cannot turn a fade into a switch, and a negative dt (a clock that
        // went backwards) changes nothing.
        Check(TaskIconAppearStep(0.5f, true, 100.0f) < 1.0f,
              L"a very slow frame cannot jump the arrival to the end");
        Check(Nearly(TaskIconAppearStep(0.5f, true, -1.0f), 0.5f, 0.001f) &&
                  Nearly(TaskIconAppearStep(0.5f, false, 0.0f), 0.5f, 0.001f),
              L"a frame with no time in it changes nothing");
    }
    }

    // The card must fit inside the window, which is never resized.
    Check(cardRect.bottom <= kWindowHeightDip + 0.01f,
          L"the music card fits inside the never-resized window");
    Check(cardRect.right - cardRect.left >= 360.0f,
          L"the card is wide enough for a cover and two text lines");
    Check(cardRect.bottom - cardRect.top >= 200.0f,
          L"the card is tall enough for all five rows");
}

// THE SURFACE THE BAR FILLS, AND THE GLASS WINDOW THAT FOLLOWS IT (HANDOFF 8.168).
//
// These are the assertions that keep the second window honest without a photograph: it is placed from
// `IslandSurfaceFor`, so whatever that function answers IS where the blur is. The cases below are the three
// shapes the bar actually takes, plus the two conditions that switch the deck on and off.
void TestIslandSurface() {
    Section(L"the bar's surface, and the glass window that follows it");

    const IslandAnchor anchor = AnchorForWindow(0.0f, 0.0f, 2560.0f);
    const RoundedBox pill = BoxForState(IslandState::Collapsed, anchor);
    const RoundedBox notice = BoxForState(IslandState::NoticeStrip, anchor);
    const RoundedBox card = BoxForState(IslandState::Expanded, anchor);

    const auto sameRect = [](const RoundedBox& a, const RoundedBox& b) {
        return Nearly(a.rect.left, b.rect.left, 0.01f) && Nearly(a.rect.top, b.rect.top, 0.01f) &&
               Nearly(a.rect.right, b.rect.right, 0.01f) &&
               Nearly(a.rect.bottom, b.rect.bottom, 0.01f);
    };

    wchar_t detail[192];

    // At rest, with the deck on screen: the cluster, which reaches past the pill on both sides.
    const RoundedBox rest = IslandSurfaceFor(pill, pill, true, false);
    std::swprintf(detail, 192, L"pill [%.0f, %.0f], surface [%.0f, %.0f]", pill.rect.left,
                  pill.rect.right, rest.rect.left, rest.rect.right);
    Check(rest.rect.left < pill.rect.left - 1.0f && rest.rect.right > pill.rect.right + 1.0f,
          L"at rest the surface includes the deck and the tray room", detail);
    Check(Nearly(rest.radiusX, (rest.rect.bottom - rest.rect.top) * 0.5f, 0.01f),
          L"and it is a stadium, which is what the renderer fills");

    // The full-screen case: the deck is suppressed, so the two must be the same rectangle -- otherwise
    // the blur would sit wide under a narrow pill, or the reverse.
    const RoundedBox alone = IslandSurfaceFor(pill, pill, false, false);
    Check(sameRect(alone, pill), L"with the decks suppressed the surface is exactly the pill");

    // The notice strip really does move the surface with it (this is the 220 -> 420 animation).
    const RoundedBox announced = IslandSurfaceFor(notice, pill, true, false);
    Check(announced.rect.right - announced.rect.left >
              (rest.rect.right - rest.rect.left) + 150.0f,
          L"an announced strip widens the surface with it");

    // A card's ANIMATED box is 520x280, but the card is drawn by its own code hanging below the bar; the
    // island surface stays the pill cluster. A glass window placed from the animated box would blur a
    // 520x280 rectangle that nothing is drawn in.
    const RoundedBox expanded = IslandSurfaceFor(card, pill, true, false);
    Check(sameRect(expanded, rest),
          L"a card's animated box is not the island surface");

    // The tray button, when it is on screen, spends its room on the right.
    const RoundedBox withTray = IslandSurfaceFor(pill, pill, true, true);
    Check(withTray.rect.right > rest.rect.right + 1.0f,
          L"a tray button widens the surface by its reserved room");
}

} // namespace island
