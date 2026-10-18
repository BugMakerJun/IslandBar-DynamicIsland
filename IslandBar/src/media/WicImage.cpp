// =====================================================================
//  WicImage.cpp
// =====================================================================
#include "media/WicImage.h"

#include <windows.h>
#include <objbase.h>
#include <shlwapi.h>   // CreateStreamOnHGlobal
#include <wincodec.h>

#include <cstring>
#include <new>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace island {
namespace {

// One factory for the process. WIC's factory is apartment-neutral and thread-safe
// for the operations used here, and creating it per call would be wasteful on a
// path that runs whenever a track changes.
IWICImagingFactory* Factory() {
    static IWICImagingFactory* factory = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&factory));
    }
    return factory;
}

// A read-only WIC stream over a caller-owned memory buffer.
//
// CreateStreamOnHGlobal with fDeleteOnRelease = TRUE does the COM plumbing and,
// crucially, owns the block: it frees it when the stream is released. An earlier
// version allocated an HGLOBAL by hand and locked it twice, which leaked the block
// on every successful decode -- once per track change, forever.
class MemoryStream {
public:
    MemoryStream(const uint8_t* bytes, size_t size) {
        if (!bytes || size == 0) return;

        HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, size);
        if (!global) return;

        void* target = GlobalLock(global);
        if (!target) {
            GlobalFree(global);
            return;
        }
        std::memcpy(target, bytes, size);
        GlobalUnlock(global);

        // On success the stream takes ownership of `global`.
        if (FAILED(CreateStreamOnHGlobal(global, TRUE, &stream_)) || !stream_) {
            GlobalFree(global);
            stream_ = nullptr;
            return;
        }

        IWICImagingFactory* factory = Factory();
        if (!factory || FAILED(factory->CreateStream(&wicStream_)) || !wicStream_ ||
            FAILED(wicStream_->InitializeFromIStream(stream_))) {
            if (wicStream_) {
                wicStream_->Release();
                wicStream_ = nullptr;
            }
            stream_->Release();
            stream_ = nullptr;
        }
    }

    ~MemoryStream() {
        if (wicStream_) wicStream_->Release();
        if (stream_) stream_->Release();
    }

    MemoryStream(const MemoryStream&) = delete;
    MemoryStream& operator=(const MemoryStream&) = delete;

    IWICStream* get() const { return wicStream_; }

private:
    IStream*    stream_ = nullptr;
    IWICStream* wicStream_ = nullptr;
};

// Decodes into a 32bpp BGRA bitmap. `frame` is released by the caller.
IWICBitmapFrameDecode* DecodeFrame(const uint8_t* bytes, size_t size, UINT* width,
                                   UINT* height) {
    IWICImagingFactory* factory = Factory();
    if (!factory) return nullptr;

    MemoryStream stream(bytes, size);
    if (!stream.get()) return nullptr;

    IWICBitmapDecoder* decoder = nullptr;
    if (FAILED(factory->CreateDecoderFromStream(stream.get(), nullptr,
                                                WICDecodeMetadataCacheOnDemand,
                                                &decoder)) ||
        !decoder) {
        return nullptr;
    }

    IWICBitmapFrameDecode* frame = nullptr;
    const HRESULT hr = decoder->GetFrame(0, &frame);
    decoder->Release();
    if (FAILED(hr) || !frame) return nullptr;

    if (width && height) {
        UINT w = 0;
        UINT h = 0;
        if (SUCCEEDED(frame->GetSize(&w, &h))) {
            *width = w;
            *height = h;
        }
    }
    return frame;
}

} // namespace

bool DecodeImage(const uint8_t* bytes, size_t size, DecodedImage* out, DecodeSlot slot) {
    if (!out || !bytes || size == 0) return false;
    out->width = 0;
    out->height = 0;
    out->pixels = nullptr;

    const int slotIndex = static_cast<int>(slot);
    if (slotIndex < 0 || slotIndex >= kDecodeSlotCount) return false;

    IWICImagingFactory* factory = Factory();
    if (!factory) return false;

    UINT width = 0;
    UINT height = 0;
    IWICBitmapFrameDecode* frame = DecodeFrame(bytes, size, &width, &height);
    if (!frame || width == 0 || height == 0) {
        if (frame) frame->Release();
        return false;
    }

    // Convert to the exact layout Direct2D wants. FormatConvertedBitmap handles
    // every source format WIC understands, which is the point of going through it
    // rather than assuming a JPEG's channel order.
    IWICFormatConverter* converter = nullptr;
    if (FAILED(factory->CreateFormatConverter(&converter)) || !converter) {
        frame->Release();
        return false;
    }

    const HRESULT hr = converter->Initialize(
        frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
        WICBitmapPaletteTypeCustom);
    frame->Release();
    if (FAILED(hr)) {
        converter->Release();
        return false;
    }

    // The converter owns a buffer of its own; keep the converter alive so that
    // buffer stays valid, and hand out a pointer into it.
    const UINT stride = width * 4;
    const UINT bytesNeeded = stride * height;

    // One buffer PER SLOT, sized on first use. Album art is small, the card draws a few of these at
    // once, and none of them needs freeing -- but "one buffer is enough" was wrong, and the way it was
    // wrong is worth keeping here: three images are alive together (cover, blurred background, icon),
    // the last decode won, and the cover was drawn from the blurred background's bytes. See DecodeSlot.
    static uint8_t* buffers[kDecodeSlotCount] = {};
    static size_t bufferSizes[kDecodeSlotCount] = {};
    uint8_t*& buffer = buffers[slotIndex];
    size_t& bufferSize = bufferSizes[slotIndex];
    if (bytesNeeded > bufferSize) {
        delete[] buffer;
        buffer = new (std::nothrow) uint8_t[bytesNeeded];
        bufferSize = buffer ? bytesNeeded : 0;
        if (!buffer) {
            converter->Release();
            return false;
        }
    }

    const HRESULT copy = converter->CopyPixels(nullptr, stride, bytesNeeded, buffer);
    converter->Release();
    if (FAILED(copy)) return false;

    out->width = static_cast<int>(width);
    out->height = static_cast<int>(height);
    out->pixels = buffer;
    return true;
}

// The exact length of a PNG in `buffer`, or 0 when it does not contain a complete one.
//
// The encoder cannot report its own output length (see EncodeFrameToPng), but the file itself
// says where it ends: a PNG is a sequence of chunks and the last is IEND, which is always 12
// bytes and carries no data. Walking the chunk headers gives the true end without guessing.
//
// This matters because the length is not merely decorative: reporting the buffer capacity made
// every encoded image carry whatever was behind it in the buffer, and `--icon-dump` wrote a
// 698-byte PNG as a 32768-byte file with 32 KB of garbage appended.
size_t PngLength(const uint8_t* buffer, size_t capacity) {
    static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (!buffer || capacity < 8) return 0;
    if (std::memcmp(buffer, kSignature, 8) != 0) return 0;

    size_t offset = 8;
    while (offset + 8 <= capacity) {
        // Big-endian length, then a four-character type.
        const size_t length = (static_cast<size_t>(buffer[offset]) << 24) |
                              (static_cast<size_t>(buffer[offset + 1]) << 16) |
                              (static_cast<size_t>(buffer[offset + 2]) << 8) |
                              static_cast<size_t>(buffer[offset + 3]);
        const uint8_t* type = buffer + offset + 4;

        if (length > capacity) return 0;   // a corrupt header; refuse rather than overrun
        if (std::memcmp(type, "IEND", 4) == 0) {
            const size_t end = offset + 12;   // length + type + no data + CRC
            return end <= capacity ? end : 0;
        }
        offset += 12 + length;
    }
    return 0;   // no IEND: truncated
}

// Shared by ReencodeToPng and EncodeBgraToPng: write `source` as a PNG through
// `factory`, given an already-sized source.
namespace {

size_t EncodeFrameToPng(IWICImagingFactory* factory, IWICBitmapSource* source,
                        UINT width, UINT height, uint8_t* out, size_t capacity,
                        bool keepAlpha) {
    // The format depends on what the image needs.
    //
    // Album art is opaque, and dropping alpha makes it smaller -- which is why this used to
    // unconditionally write 24bppBGR. That is wrong for an ICON: an application icon is mostly
    // transparent, and stripping the alpha turned its transparent background into opaque black
    // (which the card then showed as a solid white square over the blurred wash). The caller says
    // which it needs.
    const WICPixelFormatGUID targetFormat =
        keepAlpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;

    IWICFormatConverter* converter = nullptr;
    if (FAILED(factory->CreateFormatConverter(&converter)) || !converter) return 0;
    const HRESULT convertHr = converter->Initialize(
        source, targetFormat, WICBitmapDitherTypeNone, nullptr, 0.0,
        WICBitmapPaletteTypeCustom);
    if (FAILED(convertHr)) {
        converter->Release();
        return 0;
    }

    IWICStream* output = nullptr;
    if (FAILED(factory->CreateStream(&output)) || !output) {
        converter->Release();
        return 0;
    }

    HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, capacity);
    if (!global) {
        output->Release();
        converter->Release();
        return 0;
    }
    BYTE* locked = static_cast<BYTE*>(GlobalLock(global));
    if (!locked || FAILED(output->InitializeFromMemory(locked, static_cast<DWORD>(capacity)))) {
        if (locked) GlobalUnlock(global);
        GlobalFree(global);
        output->Release();
        converter->Release();
        return 0;
    }

    IWICBitmapEncoder* encoder = nullptr;
    size_t written = 0;
    if (SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        encoder && SUCCEEDED(encoder->Initialize(output, WICBitmapEncoderNoCache))) {
        IWICBitmapFrameEncode* encoderFrame = nullptr;
        IPropertyBag2* properties = nullptr;
        if (SUCCEEDED(encoder->CreateNewFrame(&encoderFrame, &properties)) && encoderFrame &&
            SUCCEEDED(encoderFrame->Initialize(properties)) &&
            SUCCEEDED(encoderFrame->SetSize(width, height))) {
            WICPixelFormatGUID format = targetFormat;
            if (SUCCEEDED(encoderFrame->SetPixelFormat(&format)) &&
                SUCCEEDED(encoderFrame->WriteSource(converter, nullptr)) &&
                SUCCEEDED(encoderFrame->Commit()) && SUCCEEDED(encoder->Commit())) {
                // The true length, read out of the PNG just written.
                //
                // WIC cannot tell us: neither Stat() nor Seek() on a memory-backed stream reports
                // the encoder's write position (Stat reports the size the stream was INITIALISED
                // with, and WIC wraps the stream again internally). The previous answer was to
                // return the buffer's CAPACITY as an admitted over-estimate, which the decoder
                // tolerated because it stops at the end marker -- but callers that WRITE the
                // buffer then wrote all the uninitialised bytes behind it.
                written = PngLength(locked, capacity);
                if (written > 0) std::memcpy(out, locked, written);
            }
        }
        if (properties) properties->Release();
        if (encoderFrame) encoderFrame->Release();
    }
    if (encoder) encoder->Release();

    GlobalUnlock(global);
    GlobalFree(global);
    output->Release();
    converter->Release();
    return written;
}

} // namespace

float AverageLuminance(const uint8_t* bgra, int width, int height) {
    if (!bgra || width <= 0 || height <= 0) return 0.0f;

    // Sampled on a grid, for the same reason the accent is: the blurred background changes once per
    // track, and a few hundred pixels a side walked per pixel would be wasted.
    const int stepX = width > 64 ? width / 64 : 1;
    const int stepY = height > 64 ? height / 64 : 1;

    double sum = 0.0;
    int counted = 0;
    for (int y = 0; y < height; y += stepY) {
        for (int x = 0; x < width; x += stepX) {
            const uint8_t* px = bgra + (static_cast<size_t>(y) * width + x) * 4;   // BGRA, premultiplied
            const float alpha = px[3] / 255.0f;
            if (alpha < 0.5f) continue;
            // Unpremultiplied, or a darker pixel's channels come out scaled by its own alpha.
            const float r = (px[2] / 255.0f) / alpha;
            const float g = (px[1] / 255.0f) / alpha;
            const float b = (px[0] / 255.0f) / alpha;
            sum += 0.2126 * r + 0.7152 * g + 0.0722 * b;
            ++counted;
        }
    }
    return counted ? static_cast<float>(sum / counted) : 0.0f;
}

AccentColour AccentColourFromArtwork(const uint8_t* bgra, int width, int height) {
    AccentColour accent;
    if (!bgra || width <= 0 || height <= 0) return accent;

    // Sampled on a grid rather than per pixel: covers arrive as thumbnails, but a 300x300 one walked
    // pixel by pixel is 90k iterations for a number that changes once per track.
    const int stepX = width > 64 ? width / 64 : 1;
    const int stepY = height > 64 ? height / 64 : 1;

    double sumR = 0.0, sumG = 0.0, sumB = 0.0;
    int counted = 0;
    for (int y = 0; y < height; y += stepY) {
        for (int x = 0; x < width; x += stepX) {
            const uint8_t* px = bgra + (static_cast<size_t>(y) * width + x) * 4;   // BGRA, premultiplied
            const float alpha = px[3] / 255.0f;
            if (alpha < 0.5f) continue;   // transparent corners say nothing about the cover's colour
            // Unpremultiplied, or a darker pixel's channels come out scaled by its own alpha.
            const float r = (px[2] / 255.0f) / alpha;
            const float g = (px[1] / 255.0f) / alpha;
            const float b = (px[0] / 255.0f) / alpha;

            const float high = (r > g ? (r > b ? r : b) : (g > b ? g : b));
            const float low = (r < g ? (r < b ? r : b) : (g < b ? g : b));
            // "Has colour", and "is not black". The floor is LOW on purpose: a dark cover is still a
            // cover, and the lift below is what makes its colour visible -- while a pixel with no colour
            // at all (or almost no light) would only drag the average towards grey.
            if (high - low < 0.12f || high < 0.12f) continue;

            sumR += r;
            sumG += g;
            sumB += b;
            ++counted;
        }
    }
    if (counted == 0) return accent;   // nothing saturated: the default stands

    float r = static_cast<float>(sumR / counted);
    float g = static_cast<float>(sumG / counted);
    float b = static_cast<float>(sumB / counted);

    // Pushed away from its own mean (more colourful) and lifted (visible against near-black). The
    // numbers are what makes a washed-out cover still produce an accent rather than a smudge.
    const float mean = (r + g + b) / 3.0f;
    constexpr float kSaturation = 1.45f;
    r = mean + (r - mean) * kSaturation;
    g = mean + (g - mean) * kSaturation;
    b = mean + (b - mean) * kSaturation;

    const float high = (r > g ? (r > b ? r : b) : (g > b ? g : b));
    if (high > 0.001f) {
        const float target = high < 0.80f ? 0.80f : 0.95f;
        const float scale = target / high;
        r *= scale;
        g *= scale;
        b *= scale;
    }
    const auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };

    accent.r = clamp01(r);
    accent.g = clamp01(g);
    accent.b = clamp01(b);
    accent.fromArtwork = true;
    return accent;
}

size_t EncodeBgraToPng(const uint8_t* bgra, int width, int height,
                       uint8_t* out, size_t capacity) {
    if (!bgra || width <= 0 || height <= 0 || !out || capacity == 0) return 0;

    IWICImagingFactory* factory = Factory();
    if (!factory) return 0;

    const UINT stride = static_cast<UINT>(width) * 4;
    const UINT bytes = stride * static_cast<UINT>(height);

    IWICBitmap* bitmap = nullptr;
    if (FAILED(factory->CreateBitmapFromMemory(
            static_cast<UINT>(width), static_cast<UINT>(height),
            GUID_WICPixelFormat32bppPBGRA, stride, bytes,
            const_cast<BYTE*>(bgra), &bitmap)) ||
        !bitmap) {
        return 0;
    }

    const size_t written = EncodeFrameToPng(factory, bitmap,
                                            static_cast<UINT>(width),
                                            static_cast<UINT>(height), out, capacity, true);
    bitmap->Release();
    return written;
}

size_t MakeBlurredBackgroundPng(const uint8_t* bytes, size_t size, int width, int height,
                                uint8_t* out, size_t capacity) {
    if (!bytes || size == 0 || width <= 0 || height <= 0 || !out || capacity == 0) {
        return 0;
    }

    IWICImagingFactory* factory = Factory();
    if (!factory) return 0;

    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    IWICBitmapFrameDecode* frame = DecodeFrame(bytes, size, &sourceWidth, &sourceHeight);
    if (!frame || sourceWidth == 0 || sourceHeight == 0) {
        if (frame) frame->Release();
        return 0;
    }

    // Step 1: down to a very small image. Most of the blur comes from here.
    //
    // The scaler is told the target size directly, so a non-square source is
    // stretched to fill -- which is what is wanted: this is an out-of-focus
    // background wash, not a picture, and stretching it cannot be perceived. That
    // also avoids needing a crop step.
    constexpr int kBlurDownscale = 12;
    const UINT tinyWidth = static_cast<UINT>(width) / kBlurDownscale + 1;
    const UINT tinyHeight = static_cast<UINT>(height) / kBlurDownscale + 1;

    IWICBitmapScaler* toTiny = nullptr;
    if (FAILED(factory->CreateBitmapScaler(&toTiny)) || !toTiny ||
        FAILED(toTiny->Initialize(frame, tinyWidth, tinyHeight,
                                  WICBitmapInterpolationModeFant))) {
        if (toTiny) toTiny->Release();
        frame->Release();
        return 0;
    }
    frame->Release();

    // Step 2: back up to the target size. Upscaling a 12x-reduced image with a
    // smooth filter IS the blur; the two steps together need no pixel shader and
    // cost a few milliseconds, once per track change.
    IWICBitmapScaler* toFull = nullptr;
    if (FAILED(factory->CreateBitmapScaler(&toFull)) || !toFull ||
        FAILED(toFull->Initialize(toTiny, static_cast<UINT>(width),
                                  static_cast<UINT>(height),
                                  WICBitmapInterpolationModeFant))) {
        if (toFull) toFull->Release();
        toTiny->Release();
        return 0;
    }
    toTiny->Release();

    const size_t written = EncodeFrameToPng(factory, toFull, static_cast<UINT>(width),
                                            static_cast<UINT>(height), out, capacity, false);
    toFull->Release();
    return written;
}

size_t ReencodeToPng(const uint8_t* bytes, size_t size, int maxDimension,
                     uint8_t* out, size_t capacity) {
    if (!bytes || size == 0 || !out || capacity == 0) return 0;

    IWICImagingFactory* factory = Factory();
    if (!factory) return 0;

    UINT width = 0;
    UINT height = 0;
    IWICBitmapFrameDecode* frame = DecodeFrame(bytes, size, &width, &height);
    if (!frame || width == 0 || height == 0) {
        if (frame) frame->Release();
        return 0;
    }

    // Scale so the LONG side becomes maxDimension, preserving aspect ratio. Art is
    // usually square, but not always, and squashing a non-square cover would look
    // worse than the memory saved.
    UINT targetWidth = width;
    UINT targetHeight = height;
    const UINT longSide = (width > height) ? width : height;
    if (maxDimension > 0 && longSide > static_cast<UINT>(maxDimension)) {
        const double scale = static_cast<double>(maxDimension) / static_cast<double>(longSide);
        targetWidth = static_cast<UINT>(width * scale + 0.5);
        targetHeight = static_cast<UINT>(height * scale + 0.5);
        if (targetWidth == 0) targetWidth = 1;
        if (targetHeight == 0) targetHeight = 1;
    }

    IWICBitmapScaler* scaler = nullptr;
    if (FAILED(factory->CreateBitmapScaler(&scaler)) || !scaler) {
        frame->Release();
        return 0;
    }
    const HRESULT scaleHr = scaler->Initialize(frame, targetWidth, targetHeight,
                                               WICBitmapInterpolationModeFant);
    frame->Release();
    if (FAILED(scaleHr)) {
        scaler->Release();
        return 0;
    }

    // Delegate the encode to the shared helper so both entry points produce a PNG
    // through exactly one code path. The duplicated version that lived here was 75
    // lines of the same encoder setup, and two copies of it is two places for the
    // stream ownership to go wrong.
    const size_t written = EncodeFrameToPng(factory, scaler, targetWidth, targetHeight,
                                            out, capacity, false);
    scaler->Release();
    return written;
}

} // namespace island
