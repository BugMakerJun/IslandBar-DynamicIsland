// =====================================================================
//  LensMaterial.cpp -- see the header.
//
//  The lens is an optical mapping, not a painted effect: every pixel of the pill samples the captured
//  backdrop a little further IN near the rim, and not at all in the flat centre, which is how a thick
//  piece of glass bends what is behind it without warping the date and the time. The mapping runs on the
//  pill itself (about 8k output pixels), then Direct2D draws that bitmap back into the rounded shape.
//
//  It is CPU-side on purpose: Direct2D's DisplacementMap effect produced an empty image here for every
//  input form that was tried (a render target, a bitmap copy, a CPU bitmap), while the same effect in the
//  standalone probe did not bend anything either. A 12k-pixel bilinear remap is far cheaper than the
//  capture itself and has no per-frame allocation.
// =====================================================================
#include "render/LensMaterial.h"

#include "core/Log.h"
#include "render/LensProfile.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include <wrl/client.h>

namespace island {
namespace {

// Inward sampling never leaves the pill, so the crop only needs a hairline of slack for the bilinear
// fetch. It used to be 32 px because the old mapping sampled OUTWARD, outside the pill.
constexpr int kMarginPx = 2;

// THE GRADE AND THE INNER SHADE, from the reference (the displacement itself is LensProfile.h). The
// shade is the CSS `inset 0 -10px 25px rgba(0,0,0,.15)` scaled to a 36 DIP pill: one soft dark band
// above the bottom edge, put into the refracted pixels rather than drawn over them, so it cannot become
// the ring the user rejected.
constexpr float kLensContrast = 1.20f;
constexpr float kLensBrightness = 1.05f;
constexpr float kLensSaturation = 1.10f;
constexpr float kLensInnerShade = 0.14f;
constexpr float kLensInnerShadeBand = 0.30f;

// The lens's own edge fades into the wallpaper over this many DIP. A hard bitmap edge is the seam the
// user photographed as "a line" along the top, even after the drawn strokes were gone.
constexpr float kLensEdgeFeatherDip = 1.5f;

ULONGLONG g_lastLensLog = 0;
void LensFail(const wchar_t* why) {
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastLensLog < 1000) return;
    g_lastLensLog = now;
    LogLine(L"  lens: %s", why);
}

// Bilinear sample of a BGRA buffer, clamped at the edges.
inline void Sample(const std::vector<unsigned char>& src, int w, int h, float x, float y,
                   unsigned char* out) {
    x = std::clamp(x, 0.0f, static_cast<float>(w - 1));
    y = std::clamp(y, 0.0f, static_cast<float>(h - 1));
    const int x0 = static_cast<int>(x);
    const int y0 = static_cast<int>(y);
    const int x1 = std::min(x0 + 1, w - 1);
    const int y1 = std::min(y0 + 1, h - 1);
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);
    const auto* p00 = &src[(static_cast<size_t>(y0) * w + x0) * 4];
    const auto* p10 = &src[(static_cast<size_t>(y0) * w + x1) * 4];
    const auto* p01 = &src[(static_cast<size_t>(y1) * w + x0) * 4];
    const auto* p11 = &src[(static_cast<size_t>(y1) * w + x1) * 4];
    for (int c = 0; c < 4; ++c) {
        const float top = static_cast<float>(p00[c]) + (static_cast<float>(p10[c]) - p00[c]) * fx;
        const float bottom = static_cast<float>(p01[c]) + (static_cast<float>(p11[c]) - p01[c]) * fx;
        out[c] = static_cast<unsigned char>(top + (bottom - top) * fy + 0.5f);
    }
}

// dst is the pill only (pillW x pillH); src is the crop, whose pill starts at (margin, margin).
// The displacement is LensOffsetFor (render/LensProfile.h); this function adds the fetch and the
// material's colour. The offset is applied to the pixel INDEX, not to a centre: Sample() treats 0 as
// the centre of the first pixel, so a zero offset on a flat centre fetches exactly the same pixel.
inline unsigned char ClampByte(float value) {
    const int rounded = static_cast<int>(value * 255.0f + 0.5f);
    return static_cast<unsigned char>(rounded < 0 ? 0 : (rounded > 255 ? 255 : rounded));
}

void Warp(const std::vector<unsigned char>& src, int srcW, int srcH, int pillW, int pillH,
          float scale, std::vector<unsigned char>& dst) {
    for (int y = 0; y < pillH; ++y) {
        for (int x = 0; x < pillW; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            const float py = static_cast<float>(y) + 0.5f;
            const LensOffset offset =
                LensOffsetFor(px, py, static_cast<float>(pillW), static_cast<float>(pillH), scale);
            unsigned char* out = &dst[(static_cast<size_t>(y) * pillW + x) * 4];
            Sample(src, srcW, srcH, static_cast<float>(kMarginPx) + static_cast<float>(x) + offset.dx,
                   static_cast<float>(kMarginPx) + static_cast<float>(y) + offset.dy, out);

            // THE MATERIAL GRADE, from the same reference: contrast, a little more light, a little
            // saturation. It works on the refracted pixels themselves, so it adds no veil.
            float r = static_cast<float>(out[2]) / 255.0f;
            float g = static_cast<float>(out[1]) / 255.0f;
            float b = static_cast<float>(out[0]) / 255.0f;
            r = (r - 0.5f) * kLensContrast + 0.5f;
            g = (g - 0.5f) * kLensContrast + 0.5f;
            b = (b - 0.5f) * kLensContrast + 0.5f;
            r *= kLensBrightness;
            g *= kLensBrightness;
            b *= kLensBrightness;
            const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            r = luma + (r - luma) * kLensSaturation;
            g = luma + (g - luma) * kLensSaturation;
            b = luma + (b - luma) * kLensSaturation;

            // THE INNER SHADE: the bottom edge absorbs a little light, which is the reference's inset
            // shadow. It is part of the refracted image, so it can never become the ring the user
            // rejected, and the content drawn on top is not shaded.
            const float bottom = static_cast<float>(pillH) - py;
            const float shadeBand = static_cast<float>(pillH) * kLensInnerShadeBand;
            const float shadeT = std::clamp(bottom / shadeBand, 0.0f, 1.0f);
            const float shadeW = 1.0f - shadeT;
            const float shade = kLensInnerShade * shadeW * shadeW * (3.0f - 2.0f * shadeW);
            r *= 1.0f - shade;
            g *= 1.0f - shade;
            b *= 1.0f - shade;

            // THE EDGE FEATHER: the refracted image fades into the wallpaper over the last 1.5 DIP, so
            // its edge is not a hard seam where it meets the desktop. The bitmap is PREMULTIPLIED, so the
            // colour is scaled by the same alpha.
            const float feather = kLensEdgeFeatherDip * scale;
            const float edge = std::clamp(offset.inside / feather, 0.0f, 1.0f);
            out[2] = ClampByte(r * edge);
            out[1] = ClampByte(g * edge);
            out[0] = ClampByte(b * edge);
            out[3] = static_cast<unsigned char>(edge * 255.0f + 0.5f);
        }
    }
}

}  // namespace

struct LensMaterial::Cache {
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> cropGpu;
    // ONE readback target, mapped on the frame it was copied to. The two-target version used to read the
    // PREVIOUS copy, which deadlocked the lens on a static window: WGC sends one frame and then stops,
    // so the second update that would have consumed it never came, and the pill kept the old window's
    // picture. The stall that buffering avoided was under half a millisecond -- the stale image was far
    // more expensive.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> warped;
    Microsoft::WRL::ComPtr<ID2D1BitmapBrush> brush;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> brushContext;
    std::vector<unsigned char> crop;
    std::vector<unsigned char> image;
    int cropW = 0;
    int cropH = 0;
    int pillW = 0;
    int pillH = 0;
};

bool LensMaterial::Initialize(ID2D1Device* device) {
    Shutdown();
    if (!device) return false;
    cache_ = new Cache();
    if (FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &cache_->context))) {
        Shutdown();
        return false;
    }
    cache_->context->SetDpi(96.0f, 96.0f);
    ready_ = true;
    return true;
}

void LensMaterial::Shutdown() {
    delete cache_;
    cache_ = nullptr;
    ready_ = false;
}

bool LensMaterial::Update(ID3D11Texture2D* texture, const RECT& source, const D2D1_ROUNDED_RECT& shape,
                          float scale, float pillLeft, float pillTop) {
    if (!ready_ || !cache_ || !texture || source.right <= source.left) {
        LensFail(L"not ready, or no source");
        return false;
    }
    const int pillW = static_cast<int>(std::lround((shape.rect.right - shape.rect.left) * scale));
    const int pillH = static_cast<int>(std::lround((shape.rect.bottom - shape.rect.top) * scale));
    if (pillW < 8 || pillH < 8) {
        LensFail(L"the pill is too small to sample");
        return false;
    }
    const int cropW = pillW + kMarginPx * 2;
    const int cropH = pillH + kMarginPx * 2;
    D3D11_TEXTURE2D_DESC frame = {};
    texture->GetDesc(&frame);
    if (!cache_->cropGpu || cache_->cropW != cropW || cache_->cropH != cropH) {
        D3D11_TEXTURE2D_DESC gpu = {};
        gpu.Width = static_cast<UINT>(cropW);
        gpu.Height = static_cast<UINT>(cropH);
        gpu.MipLevels = 1;
        gpu.ArraySize = 1;
        gpu.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        gpu.SampleDesc.Count = 1;
        gpu.Usage = D3D11_USAGE_DEFAULT;
        gpu.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_TEXTURE2D_DESC readback = gpu;
        readback.Usage = D3D11_USAGE_STAGING;
        readback.BindFlags = 0;
        readback.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Device* device = nullptr;
        texture->GetDevice(&device);
        if (!device) {
            LensFail(L"the captured texture has no device");
            return false;
        }
        cache_->cropGpu.Reset();
        cache_->staging.Reset();
        const HRESULT createdGpu = device->CreateTexture2D(&gpu, nullptr, &cache_->cropGpu);
        const HRESULT createdRead = device->CreateTexture2D(&readback, nullptr, &cache_->staging);
        device->Release();
        if (FAILED(createdGpu) || FAILED(createdRead)) {
            LensFail(L"creating the readback textures failed");
            return false;
        }
        cache_->cropW = cropW;
        cache_->cropH = cropH;
    }
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d;
    {
        ID3D11Device* device = nullptr;
        texture->GetDevice(&device);
        if (device) {
            device->GetImmediateContext(&d3d);
            device->Release();
        }
    }
    if (!d3d) {
        LensFail(L"no immediate context for the readback");
        return false;
    }
    const size_t cropBytes = static_cast<size_t>(cropW) * cropH * 4;
    if (cache_->crop.size() != cropBytes) cache_->crop.assign(cropBytes, 0);
    // Where the crop sits in the captured frame, and which part of it the frame actually covers.
    const float cropX = pillLeft - static_cast<float>(source.left) - static_cast<float>(kMarginPx);
    const float cropY = pillTop - static_cast<float>(source.top) - static_cast<float>(kMarginPx);
    const float clampedLeft = std::max(0.0f, cropX);
    const float clampedTop = std::max(0.0f, cropY);
    const float clampedRight = std::min(static_cast<float>(frame.Width), cropX + static_cast<float>(cropW));
    const float clampedBottom = std::min(static_cast<float>(frame.Height), cropY + static_cast<float>(cropH));
    if (clampedRight - clampedLeft < 4.0f || clampedBottom - clampedTop < 4.0f) {
        LensFail(L"the island is outside the captured window");
        return false;
    }
    const int srcX = static_cast<int>(std::lround(clampedLeft));
    const int srcY = static_cast<int>(std::lround(clampedTop));
    const int takeW = static_cast<int>(std::lround(clampedRight - clampedLeft));
    const int takeH = static_cast<int>(std::lround(clampedBottom - clampedTop));
    const int atX = static_cast<int>(std::lround(clampedLeft - cropX));
    const int atY = static_cast<int>(std::lround(clampedTop - cropY));
    const D3D11_BOX box = {static_cast<UINT>(srcX), static_cast<UINT>(srcY), 0,
                           static_cast<UINT>(srcX + takeW), static_cast<UINT>(srcY + takeH), 1};
    d3d->CopySubresourceRegion(cache_->cropGpu.Get(), 0, 0, 0, 0, texture, 0, &box);
    d3d->CopyResource(cache_->staging.Get(), cache_->cropGpu.Get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(d3d->Map(cache_->staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        LensFail(L"mapping the readback failed");
        return false;
    }
    const auto* rows = static_cast<const unsigned char*>(mapped.pData);
    for (int y = 0; y < takeH; ++y) {
        std::memcpy(cache_->crop.data() + (static_cast<size_t>(atY + y) * cropW + atX) * 4,
                    rows + static_cast<size_t>(y) * mapped.RowPitch,
                    static_cast<size_t>(takeW) * 4);
    }
    d3d->Unmap(cache_->staging.Get(), 0);

    // The crop's margin can fall outside the captured frame (the bar sits on the screen's top edge).
    // Those rows were never written and stay black, which the rim's outward sampling then paints as a
    // hard dark line along the top. Replicate the nearest covered row/column into the empty margin: the
    // lens then bends the screen's edge instead of a hole.
    const auto rowAt = [&](int y) { return cache_->crop.data() + static_cast<size_t>(y) * cropW * 4; };
    for (int y = 0; y < atY; ++y) {
        std::memcpy(rowAt(y), rowAt(std::min(atY, cropH - 1)), static_cast<size_t>(cropW) * 4);
    }
    const int lastRow = std::min(atY + takeH - 1, cropH - 1);
    for (int y = atY + takeH; y < cropH; ++y) {
        std::memcpy(rowAt(y), rowAt(lastRow), static_cast<size_t>(cropW) * 4);
    }
    for (int x = 0; x < atX; ++x) {
        for (int y = 0; y < cropH; ++y) {
            std::memcpy(cache_->crop.data() + (static_cast<size_t>(y) * cropW + x) * 4,
                        cache_->crop.data() + (static_cast<size_t>(y) * cropW + atX) * 4, 4);
        }
    }
    const int lastCol = std::min(atX + takeW - 1, cropW - 1);
    for (int x = atX + takeW; x < cropW; ++x) {
        for (int y = 0; y < cropH; ++y) {
            std::memcpy(cache_->crop.data() + (static_cast<size_t>(y) * cropW + x) * 4,
                        cache_->crop.data() + (static_cast<size_t>(y) * cropW + lastCol) * 4, 4);
        }
    }

    const size_t imageBytes = static_cast<size_t>(pillW) * pillH * 4;
    const bool resized = cache_->image.size() != imageBytes || cache_->pillW != pillW ||
                         cache_->pillH != pillH;
    if (resized) {
        cache_->image.assign(imageBytes, 0);
        cache_->pillW = pillW;
        cache_->pillH = pillH;
    }
    Warp(cache_->crop, cropW, cropH, pillW, pillH, scale, cache_->image);
    {
        // The middle of the pill is where the date and time will sit.
        double sum = 0.0;
        int count = 0;
        const int x0 = pillW / 5;
        const int x1 = pillW - x0;
        const int y0 = pillH / 4;
        const int y1 = pillH - y0;
        for (int y = y0; y < y1; y += 2) {
            for (int x = x0; x < x1; x += 2) {
                const unsigned char* p = cache_->image.data() + (static_cast<size_t>(y) * pillW + x) * 4;
                sum += 0.0722 * p[0] + 0.7152 * p[1] + 0.2126 * p[2];
                ++count;
            }
        }
        backdropLuma_ = count ? static_cast<float>(sum / count / 255.0) : 1.0f;
    }
    if (resized || !cache_->warped) {
        const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        if (FAILED(cache_->context->CreateBitmap(D2D1::SizeU(pillW, pillH), cache_->image.data(), pillW * 4,
                                                 properties, &bitmap))) {
            LensFail(L"creating the lens bitmap failed");
            return false;
        }
        cache_->warped = bitmap;
        cache_->brush.Reset();
    } else if (FAILED(cache_->warped->CopyFromMemory(nullptr, cache_->image.data(), pillW * 4))) {
        LensFail(L"updating the lens bitmap failed");
        return false;
    }
    return true;
}

bool LensMaterial::Fill(ID2D1DeviceContext* target, const D2D1_ROUNDED_RECT& shape, float scale, float opacity) {
    if (!ready_ || !cache_ || !target || !cache_->warped) return false;
    const int pillW = static_cast<int>(std::lround((shape.rect.right - shape.rect.left) * scale));
    const int pillH = static_cast<int>(std::lround((shape.rect.bottom - shape.rect.top) * scale));
    if (pillW < 8 || pillH < 8 || cache_->pillW < 8 || cache_->pillH < 8) return false;
    if (cache_->brushContext.Get() != target || !cache_->brush) {
        cache_->brush.Reset();
        const D2D1_BITMAP_BRUSH_PROPERTIES properties = D2D1::BitmapBrushProperties(
            D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        if (FAILED(target->CreateBitmapBrush(cache_->warped.Get(), properties, &cache_->brush))) {
            LensFail(L"creating the lens brush failed");
            return false;
        }
        cache_->brushContext = target;
    }
    // A bitmap brush is anchored at the target's origin, and this pill starts around a thousand DIPs to
    // the right; without moving the brush the whole fill samples the bitmap's clamped edge. Bitmap
    // pixels -> DIPs first (undo the DPI scale), then to the pill's top-left. While the pill is still
    // growing the previous warp is stretched to the new box; the capture is reopened when it settles.
    const float stretchX = static_cast<float>(pillW) / static_cast<float>(cache_->pillW);
    const float stretchY = static_cast<float>(pillH) / static_cast<float>(cache_->pillH);
    cache_->brush->SetTransform(
        D2D1::Matrix3x2F::Scale(stretchX / scale, stretchY / scale) *
        D2D1::Matrix3x2F::Translation(shape.rect.left, shape.rect.top));
    cache_->brush->SetOpacity(opacity);
    target->FillRoundedRectangle(&shape, cache_->brush.Get());
    return true;
}

}  // namespace island
