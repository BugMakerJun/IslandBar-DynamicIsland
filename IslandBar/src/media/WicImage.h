// =====================================================================
//  WicImage.h
//  Decoding and re-encoding images through Windows Imaging Component.
//
//  Two jobs, both about album art:
//
//   * Shrink what SMTC hands us. The thumbnail stream is whatever the media app
//     published -- often several hundred kilobytes of full-size JPEG -- while the
//     card draws it at 96 DIP. Re-encoding once to a small PNG (a few tens of KB)
//     and holding that keeps the poll thread's memory flat and makes the per-frame
//     cost a decode of something already tiny.
//
//   * Turn bytes into pixels the renderer can use. Direct2D wants BGRA with
//     premultiplied alpha, and it cannot decode a PNG itself, so the decoder
//     lives here rather than in the renderer.
//
//  Everything here is a pure function of its inputs, so the self-test can run it
//  over hand-made bytes: a valid PNG, a truncated one, and garbage.
// =====================================================================
#pragma once

#include <cstddef>
#include <cstdint>

namespace island {

// A decoded image, owned by the caller's buffer.
struct DecodedImage {
    int    width  = 0;
    int    height = 0;
    // BGRA, premultiplied, tightly packed, width * height * 4 bytes.
    const uint8_t* pixels = nullptr;
};

// A representative colour taken from an album cover, 0..1 per channel.
struct AccentColour {
    float r = 0.40f;
    float g = 0.80f;
    float b = 0.95f;
    // False when the cover had no colour worth taking and the default above is what is in the fields --
    // a mostly grey cover, an empty buffer. The caller can still draw with it; this is for the log.
    bool fromArtwork = false;
};

// The colour to tint the island's audio wave with, taken from the cover. Asked for as
// "颜色跟随专辑获取歌曲变".
//
// Picked by SATURATION rather than by average: the average of a photograph is grey-brown, and a
// grey-brown wave is what the request was about. Pixels with some colour are averaged, and the result
// is pushed away from its own mean and lifted so it reads as an accent against the pill's near-black.
// A cover with nothing saturated in it keeps the default.
AccentColour AccentColourFromArtwork(const uint8_t* bgra, int width, int height);

// The average luminance (0..1) of a BGRA buffer. The music card's blurred album background is what
// this is asked about: the words switch between black and white on the result, because the album --
// not the island -- is what is behind them. Zero for an empty buffer.
float AverageLuminance(const uint8_t* bgra, int width, int height);

// Which of the decoder's buffers to use.
//
// A decoded image's pixels stay valid until the next decode INTO THE SAME BUFFER, and the music path has
// three of them alive at the same moment: the cover, the blurred background built from it, and the source
// application's icon. They used to share ONE buffer, so the last decode silently overwrote the first --
// and the card drew its cover out of the blurred background's bytes. The blur is smaller than the cover,
// so only the cover's first half was overwritten: the top came out smeared and the bottom still looked
// like album art. Reported in exactly those words: "歌曲封面这个上半部分会涂抹".
//
// So the buffer is chosen by PURPOSE. Callers that hold more than one decoded image at a time must give
// each its own slot; a caller with a single image (a probe, the self-test) takes the default.
enum class DecodeSlot : int {
    Artwork = 0,
    Background = 1,
    Icon = 2,
    Other = 3,
};
inline constexpr int kDecodeSlotCount = 4;

// Decodes an encoded image (PNG, JPEG, ...) into BGRA premultiplied pixels.
//
// `out` receives the pixel pointer, which is owned by the decoder's own buffer
// and stays valid until the next decode into the same slot. Returns false for anything the decoder
// cannot read, including truncated and non-image data -- a hostile or broken
// payload must not crash the bar.
bool DecodeImage(const uint8_t* bytes, size_t size, DecodedImage* out,
                 DecodeSlot slot = DecodeSlot::Other);

// Re-encodes an encoded image to a PNG no larger than `maxDimension` on its long
// side, writing the bytes to `out`.
//
// Returns the number of bytes written, or 0 on failure or when the result would
// not fit in `capacity`. Used to shrink album art once, on the polling thread,
// instead of carrying the original around.
size_t ReencodeToPng(const uint8_t* bytes, size_t size, int maxDimension,
                     uint8_t* out, size_t capacity);

// Encodes raw BGRA premultiplied pixels as a PNG.
//
// The inverse of DecodeImage, and it exists so a caller can build an image in
// memory and still send it through the real decode path. The music preview uses it
// to exercise artwork without shipping a test image file.
size_t EncodeBgraToPng(const uint8_t* bgra, int width, int height,
                       uint8_t* out, size_t capacity);

// Re-encodes an image to a PNG of exactly `width` x `height`, blurred.
//
// The blur is a downscale/upscale pair through WIC's Fant interpolation, which is a
// cheap approximation of a Gaussian and is what the result is used for: the card's
// background, where the artwork is shown hugely out of focus behind text. A true
// Gaussian would need a pixel shader for a difference nobody can see under a 60%
// dark scrim.
//
// Returns the encoded size, or 0 on failure.
size_t MakeBlurredBackgroundPng(const uint8_t* bytes, size_t size, int width, int height,
                                uint8_t* out, size_t capacity);

} // namespace island
