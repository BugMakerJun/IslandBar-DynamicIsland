// =====================================================================
//  LensMaterial.cpp -- see the header.
// =====================================================================
#include "render/LensMaterial.h"

#include "core/Log.h"

#include <wrl/client.h>

#include <cmath>
#include <vector>

namespace island {
namespace {

constexpr GUID kDisplacementMap = {0xedc48364, 0x0417, 0x4111,
                                   {0x94, 0x50, 0x43, 0x84, 0x5f, 0xa9, 0xf8, 0x90}};
constexpr GUID kGaussianBlur = {0x1feb6d69, 0x2fe6, 0x4ac9,
                                {0x8c, 0x58, 0x1d, 0x7f, 0x93, 0xe7, 0xa6, 0xa5}};
constexpr UINT32 kDisplaceScale = 0;
constexpr UINT32 kDisplaceXChannel = 1;
constexpr UINT32 kDisplaceYChannel = 2;
constexpr UINT32 kBlurSigma = 0;

constexpr int kMarginPx = 24;
constexpr float kLensScale = 18.0f;
constexpr float kLensBandPx = 16.0f;
constexpr float kLensBlur = 0.8f;

std::vector<unsigned char> BuildMap(int pillW, int pillH) {
    const int wide = pillW + kMarginPx * 2;
    const int tall = pillH + kMarginPx * 2;
    std::vector<unsigned char> map(static_cast<size_t>(wide) * tall * 4, 0);
    const float radius = pillH * 0.5f;
    const float left = static_cast<float>(kMarginPx);
    const float top = static_cast<float>(kMarginPx);
    const float right = left + static_cast<float>(pillW);
    const float bottom = top + static_cast<float>(pillH);
    const float cxL = left + radius;
    const float cxR = right - radius;
    const float cy = top + radius;
    for (int y = 0; y < tall; ++y) {
        for (int x = 0; x < wide; ++x) {
            const float px = x + 0.5f;
            const float py = y + 0.5f;
            float nx = 0.0f;
            float ny = 0.0f;
            float inside = kLensBandPx;
            if (px >= left - 2.0f && px <= right + 2.0f && py >= top - 2.0f && py <= bottom + 2.0f) {
                if (px >= cxL && px <= cxR) {
                    inside = std::min(py - top, bottom - py);
                    ny = (py < cy) ? -1.0f : 1.0f;   // outward
                } else {
                    const float centreX = (px < cxL) ? cxL : cxR;
                    const float dx = px - centreX;
                    const float dy = py - cy;
                    const float length = std::sqrt(dx * dx + dy * dy);
                    if (length > 0.001f) {
                        nx = dx / length;
                        ny = dy / length;
                    }
                    inside = radius - length;
                }
            }
            float shift = 0.0f;
            const float t = inside / kLensBandPx;
            if (t < 1.0f) {
                const float falloff = 1.0f - t;
                shift = falloff * falloff * kLensBandPx * 0.85f;
            }
            const auto encode = [](float offset) {
                const float value = 0.5f + offset / kLensScale;
                const float clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
                return static_cast<unsigned char>(clamped * 255.0f + 0.5f);
            };
            const size_t at = (static_cast<size_t>(y) * wide + x) * 4;
            map[at + 0] = encode(nx * shift);
            map[at + 1] = encode(ny * shift);
            map[at + 2] = 0;
            map[at + 3] = 255;
        }
    }
    return map;
}

}  // namespace

struct LensMaterial::Cache {
    Microsoft::WRL::ComPtr<ID2D1Effect> displace;
    Microsoft::WRL::ComPtr<ID2D1Effect> blur;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> map;
    Microsoft::WRL::ComPtr<ID2D1Layer> layer;
    Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> geometry;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> wrappedTexture;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> backdrop;
    int mapWidth = 0;
    int mapHeight = 0;
};

bool LensMaterial::Initialize(ID2D1DeviceContext* context) {
    Shutdown();
    if (!context) return false;
    cache_ = new Cache();
    if (FAILED(context->CreateEffect(kDisplacementMap, &cache_->displace))) return false;
    cache_->displace->SetValue(kDisplaceScale, kLensScale);
    cache_->displace->SetValue(kDisplaceXChannel, static_cast<UINT32>(D2D1_CHANNEL_SELECTOR_R));
    cache_->displace->SetValue(kDisplaceYChannel, static_cast<UINT32>(D2D1_CHANNEL_SELECTOR_G));
    if (FAILED(context->CreateEffect(kGaussianBlur, &cache_->blur))) return false;
    cache_->blur->SetValue(kBlurSigma, kLensBlur);
    if (FAILED(context->CreateLayer(&cache_->layer))) return false;
    ready_ = true;
    return true;
}

void LensMaterial::Shutdown() {
    delete cache_;
    cache_ = nullptr;
    ready_ = false;
}

bool LensMaterial::Draw(ID2D1DeviceContext* context, ID3D11Texture2D* texture, const RECT& monitor,
                        const D2D1_ROUNDED_RECT& shape, float scale, float opacity) {
    if (!ready_ || !cache_ || !context || !texture || !monitor.right) return false;
    if (opacity <= 0.01f) return false;

    // The captured texture as a D2D bitmap. WGC recycles two textures, so the wrapper is cached against the
    // pointer and rebuilt only when the pool hands back a different one.
    if (cache_->wrappedTexture.Get() != texture) {
        Microsoft::WRL::ComPtr<IDXGISurface> surface;
        if (FAILED(texture->QueryInterface(IID_PPV_ARGS(&surface)))) return false;
        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> wrapped;
        if (FAILED(context->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &wrapped))) {
            return false;
        }
        cache_->backdrop = wrapped.Get();
        cache_->wrappedTexture = texture;
    }

    const float widthDip = shape.rect.right - shape.rect.left;
    const float heightDip = shape.rect.bottom - shape.rect.top;
    const int mapWidth = static_cast<int>(std::lround(widthDip * scale)) + kMarginPx * 2;
    const int mapHeight = static_cast<int>(std::lround(heightDip * scale)) + kMarginPx * 2;
    if (mapWidth < 8 || mapHeight < 8) return false;

    if (!cache_->map || cache_->mapWidth != mapWidth || cache_->mapHeight != mapHeight) {
        const int pillW = mapWidth - kMarginPx * 2;
        const int pillH = mapHeight - kMarginPx * 2;
        const std::vector<unsigned char> map = BuildMap(pillW, pillH);
        const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        if (FAILED(context->CreateBitmap(D2D1::SizeU(mapWidth, mapHeight), map.data(), mapWidth * 4,
                                         properties, &bitmap))) {
            return false;
        }
        cache_->map = bitmap;
        cache_->mapWidth = mapWidth;
        cache_->mapHeight = mapHeight;

        const D2D1_ROUNDED_RECT geometryShape = {
            D2D1::RectF(0.0f, 0.0f, static_cast<float>(pillW), static_cast<float>(pillH)),
            pillH * 0.5f, pillH * 0.5f};
        cache_->geometry.Reset();
        // The renderer's factory is not reachable here; the geometry is built on the context's factory.
        Microsoft::WRL::ComPtr<ID2D1Factory> factory;
        context->GetFactory(&factory);
        if (factory) factory->CreateRoundedRectangleGeometry(geometryShape, &cache_->geometry);
    }
    if (!cache_->map || !cache_->geometry) return false;

    cache_->displace->SetInput(0, cache_->backdrop.Get());
    cache_->displace->SetInput(1, cache_->map.Get());
    cache_->blur->SetInputEffect(0, cache_->displace.Get());

    // Work in PHYSICAL pixels for this pass: the texture is one pixel per screen pixel, and the shape is
    // given in DIPs. The transform is restored before anything else is drawn.
    D2D1_MATRIX_3X2_F previous = {};
    context->GetTransform(&previous);
    context->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));

    D2D1_LAYER_PARAMETERS1 layer = {};
    layer.contentBounds = D2D1::InfiniteRect();
    layer.geometricMask = cache_->geometry.Get();
    layer.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
    layer.maskTransform = D2D1::Matrix3x2F::Identity();
    layer.opacity = opacity;
    context->PushLayer(layer, cache_->layer.Get());
    const float pillX = shape.rect.left * scale;
    const float pillY = shape.rect.top * scale;
    context->DrawImage(cache_->blur.Get(),
                       D2D1::Point2F(-pillX + static_cast<float>(kMarginPx),
                                     -pillY + static_cast<float>(kMarginPx)));
    context->PopLayer();

    context->SetTransform(previous);
    return true;
}

}  // namespace island
