// =====================================================================
//  lensprobe -- what the island looks like when the backdrop is BENT
//
//      lensprobe --at 1180,300 --size 220x36 --scale 22 --blur 1.2
//
//  The drawn material can fake thickness, highlights and tint, but not refraction: that needs the pixels
//  behind the island. This probe takes ONE screenshot of the region (BitBlt, before its own window is
//  shown), then draws that image back through a displacement map -- the background is squeezed toward the
//  rim, exactly the way a thick piece of glass bends it -- followed by a small blur. It is the static
//  version of the lens: the same D2D effect chain a live capture would feed.
//
//  WHAT THIS IS FOR: deciding whether the refraction is worth a capture subsystem. It captures the screen
//  once, stays on the GPU afterwards, and holds a single frame.
//
//  ASCII-only on purpose. Do not add non-ASCII text to this file.
// =====================================================================

#include <windows.h>

#include <d2d1_3.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace {

// The effect CLSIDs are DEFINE_GUID declarations whose definitions live in a GUID library this probe does
// not otherwise need.
constexpr GUID kGaussianBlur = {0x1feb6d69, 0x2fe6, 0x4ac9,
                                {0x8c, 0x58, 0x1d, 0x7f, 0x93, 0xe7, 0xa6, 0xa5}};
constexpr GUID kDisplacementMap = {0xedc48364, 0x0417, 0x4111, {0x94, 0x50, 0x43, 0x84, 0x5f, 0xa9, 0xf8, 0x90}};

constexpr UINT32 kDisplaceScale = 0;
constexpr UINT32 kDisplaceXChannel = 1;
constexpr UINT32 kDisplaceYChannel = 2;
constexpr UINT32 kBlurSigma = 0;

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_ERASEBKGND) return 1;
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

// The pixels behind the island, grabbed once. `pixels` is BGRA, top-down, fully opaque.
bool GrabScreen(int x, int y, int w, int h, std::vector<unsigned char>* pixels) {
    if (!pixels || w < 1 || h < 1) return false;
    HDC screen = GetDC(nullptr);
    if (!screen) return false;

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;   // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = memory ? CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    bool ok = false;
    if (memory && bitmap && bits) {
        HGDIOBJ previous = SelectObject(memory, bitmap);
        ok = BitBlt(memory, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT) != FALSE;
        SelectObject(memory, previous);
        if (ok) {
            pixels->assign(static_cast<unsigned char*>(bits),
                           static_cast<unsigned char*>(bits) + static_cast<size_t>(w) * h * 4);
        }
    }
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return ok;
}

// THE LENS. For every pixel of a stadium, the displacement map says "sample from further inside" near the
// rim, fading to zero in the middle. Values are 0.5 = no shift; the effect multiplies (value-0.5) by the
// scale.
std::vector<unsigned char> BuildLensMap(int pillW, int pillH, int margin, float scale, float bandDip) {
    const int wide = pillW + margin * 2;
    const int tall = pillH + margin * 2;
    std::vector<unsigned char> map(static_cast<size_t>(wide) * tall * 4, 0);
    const float radius = pillH * 0.5f;
    const float left = static_cast<float>(margin);
    const float top = static_cast<float>(margin);
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
            float inside = 0.0f;   // distance from the rim, in pixels
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
            // Outside the pill there is no lens at all.
            if (px < left - 2.0f || px > right + 2.0f || py < top - 2.0f || py > bottom + 2.0f) {
                inside = bandDip;
            }
            const float t = inside / bandDip;
            float shift = 0.0f;
            if (t < 1.0f) {
                const float falloff = 1.0f - t;
                shift = falloff * falloff * bandDip * 0.85f;
            }
            const float offsetX = nx * shift;
            const float offsetY = ny * shift;
            const size_t at = (static_cast<size_t>(y) * wide + x) * 4;
            const auto encode = [scale](float offset) {
                const float value = 0.5f + offset / scale;
                const float clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
                return static_cast<unsigned char>(clamped * 255.0f + 0.5f);
            };
            map[at + 0] = encode(offsetX);
            map[at + 1] = encode(offsetY);
            map[at + 2] = 0;
            map[at + 3] = 255;
        }
    }
    return map;
}

struct Comp {
    ComPtr<ID3D11Device> d3d;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDCompositionDevice> comp;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<ID2D1Factory3> factory;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> d2dContext;
    ComPtr<ID2D1Bitmap1> frame;
    ComPtr<ID2D1Layer> layer;
    ComPtr<ID2D1Bitmap> backdrop;
    ComPtr<ID2D1Bitmap> lensMap;
    ComPtr<ID2D1Effect> displace;
    ComPtr<ID2D1Effect> blur;
    ComPtr<ID2D1Geometry> pillGeometry;
};

bool CreateStack(Comp& c, HWND hwnd, int w, int h) {
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
                                   D3D11_SDK_VERSION, &c.d3d, nullptr, &c.context);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(c.d3d.As(&dxgiDevice))) return false;
    if (FAILED(DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&c.comp)))) return false;

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter))) return false;
    ComPtr<IDXGIFactory2> dxgiFactory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&dxgiFactory)))) return false;

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = static_cast<UINT>(w);
    desc.Height = static_cast<UINT>(h);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (FAILED(dxgiFactory->CreateSwapChainForComposition(c.d3d.Get(), &desc, nullptr, &c.swapChain))) {
        return false;
    }

    D2D1_FACTORY_OPTIONS options = {};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory3), &options,
                                 reinterpret_cast<void**>(c.factory.GetAddressOf())))) {
        return false;
    }
    if (FAILED(c.factory->CreateDevice(dxgiDevice.Get(), &c.d2dDevice))) return false;
    if (FAILED(c.d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &c.d2dContext))) {
        return false;
    }
    c.d2dContext->SetDpi(96.0f, 96.0f);
    c.d2dContext->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    if (FAILED(c.comp->CreateVisual(&c.visual))) return false;
    if (FAILED(c.visual->SetContent(c.swapChain.Get()))) return false;
    if (FAILED(c.comp->CreateTargetForHwnd(hwnd, TRUE, &c.target))) return false;
    if (FAILED(c.target->SetRoot(c.visual.Get()))) return false;
    if (FAILED(c.comp->Commit())) return false;

    // The dimensions are the window's, so the composition transform is identity.
    return true;
}

bool CreateResources(Comp& c, const std::vector<unsigned char>& pixels, int w, int h, int margin,
                     float scale, float band, float blurSigma) {
    const int wide = w + margin * 2;
    const int tall = h + margin * 2;
    const D2D1_BITMAP_PROPERTIES bitmapProperties = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(c.d2dContext->CreateBitmap(D2D1::SizeU(wide, tall), pixels.data(), wide * 4, bitmapProperties,
                                          &c.backdrop))) {
        return false;
    }
    std::printf("lensprobe:   backdrop bitmap\n");
    std::fflush(stdout);
    const std::vector<unsigned char> lens = BuildLensMap(w, h, margin, scale, band);
    std::printf("lensprobe:   lens map built\n");
    std::fflush(stdout);
    const D2D1_BITMAP_PROPERTIES mapProperties = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    if (FAILED(c.d2dContext->CreateBitmap(D2D1::SizeU(wide, tall), lens.data(), wide * 4, mapProperties,
                                          &c.lensMap))) {
        return false;
    }
    std::printf("lensprobe:   lens bitmap\n");
    std::fflush(stdout);
    if (FAILED(c.d2dContext->CreateEffect(kDisplacementMap, &c.displace))) return false;
    c.displace->SetValue(kDisplaceScale, scale);
    c.displace->SetValue(kDisplaceXChannel, static_cast<UINT32>(D2D1_CHANNEL_SELECTOR_R));
    c.displace->SetValue(kDisplaceYChannel, static_cast<UINT32>(D2D1_CHANNEL_SELECTOR_G));
    c.displace->SetInput(0, c.backdrop.Get());
    c.displace->SetInput(1, c.lensMap.Get());
    std::printf("lensprobe:   displacement effect\n");
    std::fflush(stdout);

    if (FAILED(c.d2dContext->CreateEffect(kGaussianBlur, &c.blur))) return false;
    c.blur->SetValue(kBlurSigma, blurSigma);
    c.blur->SetInputEffect(0, c.displace.Get());
    std::printf("lensprobe:   blur effect\n");
    std::fflush(stdout);

    if (FAILED(c.d2dContext->CreateLayer(&c.layer))) return false;
    std::printf("lensprobe:   layer\n");
    std::fflush(stdout);
    const D2D1_ROUNDED_RECT rounded = {
        D2D1::RectF(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)), h * 0.5f, h * 0.5f};
    ComPtr<ID2D1RoundedRectangleGeometry> roundedGeometry;
    if (FAILED(c.factory->CreateRoundedRectangleGeometry(rounded, &roundedGeometry))) return false;
    c.pillGeometry = roundedGeometry;
    std::printf("lensprobe:   geometry\n");
    std::fflush(stdout);
    return true;
}

bool RenderFrame(Comp& c, int w, int h, int margin) {
    ComPtr<IDXGISurface> surface;
    if (FAILED(c.swapChain->GetBuffer(0, IID_PPV_ARGS(&surface)))) return false;
    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    c.frame.Reset();
    if (FAILED(c.d2dContext->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &c.frame))) {
        return false;
    }
    c.d2dContext->SetTarget(c.frame.Get());
    c.d2dContext->BeginDraw();
    c.d2dContext->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    D2D1_LAYER_PARAMETERS1 layer = {};
    layer.contentBounds = D2D1::InfiniteRect();
    layer.geometricMask = c.pillGeometry.Get();
    layer.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
    layer.maskTransform = D2D1::Matrix3x2F::Identity();
    layer.opacity = 1.0f;
    c.d2dContext->PushLayer(layer, c.layer.Get());
    c.d2dContext->DrawImage(c.blur.Get(), D2D1::Point2F(-static_cast<float>(margin), -static_cast<float>(margin)));
    c.d2dContext->PopLayer();

    // The scrim over the refracted backdrop: the same idea as the bar's own fill, but much lighter, because
    // the point is to see the bend through it.
    ComPtr<ID2D1SolidColorBrush> brush;
    const D2D1_ROUNDED_RECT rounded = {
        D2D1::RectF(0.5f, 0.5f, static_cast<float>(w) - 0.5f, static_cast<float>(h) - 0.5f),
        (h - 1.0f) * 0.5f, (h - 1.0f) * 0.5f};
    if (SUCCEEDED(c.d2dContext->CreateSolidColorBrush(D2D1::ColorF(0.04f, 0.06f, 0.12f, 0.10f), &brush))) {
        c.d2dContext->FillRoundedRectangle(&rounded, brush.Get());
        brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.55f));
        c.d2dContext->DrawRoundedRectangle(&rounded, brush.Get(), 1.0f);
    }

    const HRESULT hr = c.d2dContext->EndDraw();
    c.d2dContext->SetTarget(nullptr);
    c.frame.Reset();
    if (FAILED(hr)) return false;
    return SUCCEEDED(c.swapChain->Present(1, 0));
}

}  // namespace

int main(int argc, char** argv) {
    int x = 1180, y = 300, w = 220, h = 36, seconds = 20;
    float scale = 22.0f, band = 14.0f, blur = 1.2f;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--at") == 0 && i + 1 < argc) std::sscanf(argv[++i], "%d,%d", &x, &y);
        else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &w, &h);
        else if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc) scale = static_cast<float>(std::atof(argv[++i]));
        else if (std::strcmp(argv[i], "--band") == 0 && i + 1 < argc) band = static_cast<float>(std::atof(argv[++i]));
        else if (std::strcmp(argv[i], "--blur") == 0 && i + 1 < argc) blur = static_cast<float>(std::atof(argv[++i]));
        else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) seconds = std::atoi(argv[++i]);
    }

    // The backdrop is captured with a margin: the lens samples OUTWARD at the rim, and those samples have
    // to be real pixels from outside the pill.
    const int margin = 24;
    std::vector<unsigned char> pixels;
    if (!GrabScreen(x - margin, y - margin, w + margin * 2, h + margin * 2, &pixels)) {
        std::printf("lensprobe: could not grab the screen behind the pill\n");
        return 2;
    }
    std::printf("lensprobe: grabbed %dx%d\n", w + margin * 2, h + margin * 2);
    std::fflush(stdout);

    const wchar_t* className = L"IslandBar.LensProbe";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 2;

    const DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP;
    HWND hwnd = CreateWindowExW(exStyle, className, L"lensprobe", WS_POPUP, x, y, w, h, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return 2;
    std::printf("lensprobe: window created\n");
    std::fflush(stdout);

    Comp comp;
    if (!CreateStack(comp, hwnd, w, h)) {
        std::printf("lensprobe: composition setup failed\n");
        return 3;
    }
    std::printf("lensprobe: stack ready\n");
    std::fflush(stdout);
    if (!CreateResources(comp, pixels, w, h, margin, scale, band, blur)) {
        std::printf("lensprobe: the lens resources could not be created\n");
        return 3;
    }
    std::printf("lensprobe: lens resources ready\n");
    std::fflush(stdout);

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    std::printf("lensprobe: %dx%d at %d,%d, scale %.1f, band %.1f, blur %.2f\n", w, h, x, y, scale, band,
                blur);
    std::fflush(stdout);

    const DWORD until = GetTickCount() + static_cast<DWORD>(seconds) * 1000;
    while (GetTickCount() < until) {
        MSG message = {};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        RenderFrame(comp, w, h, margin);
    }

    DestroyWindow(hwnd);
    return 0;
}
