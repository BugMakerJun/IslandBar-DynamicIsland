// =====================================================================
//  dcomp_probe.cpp -- a pill-sized DirectComposition window with acrylic under it
//
//  THE QUESTION, IN TWO PARTS (HANDOFF 8.165.3):
//    1. does the system material show at all on a per-pixel-alpha DComp window whose rectangle is
//       exactly the pill? (8.86 says it does on the bar's full-width strip; this asks the same of a
//       small one.)
//    2. does the material stop at the ROUNDED corners, or does it spill into four square corners of
//       blur? A window region does not clip acrylic (8.91), so the honest candidates are the DWM corner
//       preference and a geometric clip. This probe measures the corner preference; `--region` lets the
//       window region answer for itself too.
//
//  WHAT IT DRAWS: a transparent surface, optionally plus a very faint scrim (`--scrim`, default 9%),
//  because the product would draw a tinted scrim over the material anyway. `--scrim 0` draws nothing at
//  all, which is also worth one photograph: a fully transparent composition surface may be optimized
//  away by the compositor, and that would look like "the material does not work".
//
//  WHAT IS JUDGED: a PHOTOGRAPH over a busy backdrop. Nothing here decides whether the blur is good.
//
//  ASCII-only on purpose. Do not add non-ASCII text to this file.
// =====================================================================

#include "dcomp_probe.h"

#include "glass_common.h"

#include <windows.h>
#include <d2d1_3.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <cstdio>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace {

void Report(const char* what, HRESULT hr) {
    std::printf("glassprobe: %s -> 0x%08lX\n", what, static_cast<unsigned long>(hr));
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    // A material window must never take a click: at rest the bar owns the pill's input, and this window
    // sits underneath it. HTTRANSPARENT hands the hit test to the window below.
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    // Nothing is painted with GDI: every pixel of this window comes from the composition swap chain.
    if (message == WM_ERASEBKGND) return 1;
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

// The D3D11 + D2D + swap chain + composition stack, in the same order and with the same settings as
// src/render/Renderer.cpp. Copied rather than linked on purpose: a probe that shares the bar's renderer
// would inherit exactly the thing being questioned.
struct Comp {
    ComPtr<ID3D11Device> d3d;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDCompositionDevice> comp;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> d2dContext;
    ComPtr<ID2D1Bitmap1> frame;
};

bool CreateDeviceStack(Comp& c, int width, int height) {
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
                                   D3D11_SDK_VERSION, &c.d3d, nullptr, &c.context);
    Report("D3D11CreateDevice", hr);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIDevice> dxgiDevice;
    hr = c.d3d.As(&dxgiDevice);
    Report("QueryInterface(IDXGIDevice)", hr);
    if (FAILED(hr)) return false;

    // Must happen while the DXGI device is alive (HANDOFF 6.3: passing a released pointer here crashed).
    hr = DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&c.comp));
    Report("DCompositionCreateDevice", hr);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    Report("IDXGIDevice::GetAdapter", hr);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIFactory2> factory;
    hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    Report("IDXGIAdapter::GetParent(IDXGIFactory2)", hr);
    if (FAILED(hr)) return false;

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    // Premultiplied alpha is what lets the area outside the pill be genuinely transparent, and what
    // lets the faint scrim composite over the material instead of covering it.
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

    hr = factory->CreateSwapChainForComposition(c.d3d.Get(), &desc, nullptr, &c.swapChain);
    Report("CreateSwapChainForComposition", hr);
    if (FAILED(hr)) return false;

    D2D1_FACTORY_OPTIONS factoryOptions = {};
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                           &factoryOptions, reinterpret_cast<void**>(c.factory.GetAddressOf()));
    Report("D2D1CreateFactory", hr);
    if (FAILED(hr)) return false;

    hr = c.factory->CreateDevice(dxgiDevice.Get(), &c.d2dDevice);
    Report("ID2D1Factory1::CreateDevice", hr);
    if (FAILED(hr)) return false;

    hr = c.d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &c.d2dContext);
    Report("CreateDeviceContext", hr);
    if (FAILED(hr)) return false;

    c.d2dContext->SetDpi(96.0f, 96.0f);
    c.d2dContext->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    c.visual = nullptr;
    hr = c.comp->CreateVisual(&c.visual);
    Report("IDCompositionDevice::CreateVisual", hr);
    if (FAILED(hr)) return false;

    hr = c.visual->SetContent(c.swapChain.Get());
    Report("IDCompositionVisual::SetContent", hr);
    if (FAILED(hr)) return false;

    return true;
}

bool BindTarget(Comp& c, HWND hwnd) {
    HRESULT hr = c.comp->CreateTargetForHwnd(hwnd, TRUE, &c.target);
    Report("CreateTargetForHwnd", hr);
    if (FAILED(hr)) return false;

    hr = c.target->SetRoot(c.visual.Get());
    Report("IDCompositionTarget::SetRoot", hr);
    if (FAILED(hr)) return false;

    hr = c.comp->Commit();
    Report("IDCompositionDevice::Commit", hr);
    return SUCCEEDED(hr);
}

bool AcquireFrame(Comp& c) {
    ComPtr<IDXGISurface> surface;
    HRESULT hr = c.swapChain->GetBuffer(0, IID_PPV_ARGS(&surface));
    if (FAILED(hr)) {
        Report("IDXGISwapChain1::GetBuffer", hr);
        return false;
    }

    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);

    hr = c.d2dContext->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &c.frame);
    if (FAILED(hr)) {
        Report("CreateBitmapFromDxgiSurface", hr);
        return false;
    }
    c.d2dContext->SetTarget(c.frame.Get());
    return true;
}

}  // namespace

int RunDCompProbe(const DCompOptions& options) {
    const wchar_t* className = L"IslandBar.GlassProbe.DComp";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    if (!RegisterClassW(&wc)) {
        std::printf("glassprobe: RegisterClassW failed (error %lu)\n", GetLastError());
        return 2;
    }

    // The bar's own window kind, on purpose: WS_EX_NOREDIRECTIONBITMAP + DirectComposition, with no
    // GDI redirection surface and no opaque content. This is the only kind of window in which 8.86
    // measured the material actually appearing.
    const DWORD style = WS_POPUP;
    const DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE |
                          WS_EX_NOREDIRECTIONBITMAP;
    HWND hwnd = CreateWindowExW(exStyle, className, L"glassprobe-dcomp", style, options.x, options.y,
                                options.width, options.height, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        std::printf("glassprobe: CreateWindowExW failed (error %lu)\n", GetLastError());
        return 2;
    }

    Comp comp;
    // `--bare` asks the cheapest possible question: does a window with NO composition content at all
    // still show the material? If it does, the product's glass window needs no renderer.
    if (!options.bare) {
        if (!CreateDeviceStack(comp, options.width, options.height)) {
            DestroyWindow(hwnd);
            return 3;
        }
        if (!BindTarget(comp, hwnd)) {
            DestroyWindow(hwnd);
            return 3;
        }
    }

    HRESULT acrylic = E_NOTIMPL;
    const char* material = "none (--plain)";
    if (!options.plain) {
        acrylic = glassprobe::SetAcrylic(hwnd, options.tint);
        material = "ACRYLIC (ACCENT_ENABLE_ACRYLICBLURBEHIND)";
    }

    const HRESULT corner = glassprobe::SetRoundCorners(hwnd);

    if (options.region) {
        const int radius = options.height / 2;
        HRGN region = CreateRoundRectRgn(0, 0, options.width + 1, options.height + 1, radius * 2,
                                         radius * 2);
        const int applied = SetWindowRgn(hwnd, region, TRUE);
        std::printf("glassprobe: SetWindowRgn -> %d (the region now owns the HRGN)\n", applied);
    }

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    // The product's z-order, measured rather than assumed: a topmost window inserted AFTER the bar's
    // HWND sits directly below it. `--under-bar` does exactly that, so the bar's own scrim, rim and
    // shadow are drawn over the real material -- the only way to see the final picture without changing
    // product code.
    if (options.underBar) {
        HWND bar = FindWindowW(L"IslandBar.IslandWindow", nullptr);
        if (bar) {
            RECT barRect = {};
            GetWindowRect(bar, &barRect);
            const BOOL placed = SetWindowPos(hwnd, bar, options.x, options.y, options.width,
                                             options.height, SWP_NOACTIVATE);
            std::printf("glassprobe: under bar %p (bar rect %ld,%ld %ldx%ld) -> %d\n",
                        reinterpret_cast<void*>(bar), barRect.left, barRect.top,
                        barRect.right - barRect.left, barRect.bottom - barRect.top, placed ? 1 : 0);
        } else {
            std::printf("glassprobe: --under-bar found no IslandBar.IslandWindow\n");
        }
    }

    RECT rect = {};
    GetWindowRect(hwnd, &rect);
    std::printf("glassprobe: window-kind=%s (WS_EX_NOREDIRECTIONBITMAP)\n",
                options.bare ? "bare, no D3D/DComp content" : "DirectComposition");
    std::printf("glassprobe: material=%s\n", material);
    std::printf("glassprobe: tint=0x%08lX scrim-alpha=%d\n",
                static_cast<unsigned long>(options.tint), options.scrimAlpha);
    std::printf("glassprobe: acrylic -> 0x%08lX\n", static_cast<unsigned long>(acrylic));
    std::printf("glassprobe: DWMWA_WINDOW_CORNER_PREFERENCE=ROUND -> 0x%08lX\n",
                static_cast<unsigned long>(corner));
    std::printf("glassprobe: window %ld,%ld %ldx%ld, visible=%d, on screen for %d s\n", rect.left,
                rect.top, rect.right - rect.left, rect.bottom - rect.top,
                IsWindowVisible(hwnd) ? 1 : 0, options.seconds);
    std::fflush(stdout);

    // WHY THIS EXISTS: the product's pill grows 220 -> ~720 on hover, so a second material window
    // would have to follow that animation frame by frame. Neither model could say whether a per-frame
    // SetWindowPos flickers, so it is measured: one call per step, timed, with every failure counted.
    if (options.animateTo > options.width) {
        LARGE_INTEGER frequency = {};
        QueryPerformanceFrequency(&frequency);
        const int steps = 60;
        double totalMs = 0.0;
        double maxMs = 0.0;
        int failures = 0;
        for (int step = 1; step <= steps; ++step) {
            const double t = static_cast<double>(step) / steps;
            const int w = options.width +
                          static_cast<int>((options.animateTo - options.width) * t);
            LARGE_INTEGER before = {};
            QueryPerformanceCounter(&before);
            const BOOL ok = SetWindowPos(hwnd, nullptr, options.x, options.y, w, options.height,
                                         SWP_NOACTIVATE | SWP_NOZORDER);
            LARGE_INTEGER after = {};
            QueryPerformanceCounter(&after);
            const double ms =
                1000.0 * static_cast<double>(after.QuadPart - before.QuadPart) / frequency.QuadPart;
            totalMs += ms;
            if (ms > maxMs) maxMs = ms;
            if (!ok) ++failures;
            Sleep(static_cast<DWORD>(options.animateMs / steps));
        }
        std::printf("glassprobe: animate %dx%d -> %dx%d over %d ms: steps=%d failures=%d "
                    "avg=%.3f ms max=%.3f ms\n",
                    options.width, options.height, options.animateTo, options.height,
                    options.animateMs, steps, failures, totalMs / steps, maxMs);
        std::fflush(stdout);
    }

    const DWORD until = GetTickCount() + static_cast<DWORD>(options.seconds) * 1000;
    int frames = 0;
    HRESULT present = S_OK;
    while (GetTickCount() < until) {
        MSG message = {};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        if (!options.bare) {
            if (!AcquireFrame(comp)) break;

            const float alpha = static_cast<float>(options.scrimAlpha) / 255.0f;
            comp.d2dContext->BeginDraw();
            // WHITE is the probing scrim; BLACK is what the product's glass theme actually draws
            // (pillTop 0.20/0.24/0.32 at 0.66, pillBottom 0.06/0.08/0.14 at 0.82). `--scrim-dark`
            // therefore photographs the shape question the way the pill will really be seen.
            comp.d2dContext->Clear(options.scrimDark ? D2D1::ColorF(0.0f, 0.0f, 0.0f, alpha)
                                                     : D2D1::ColorF(1.0f, 1.0f, 1.0f, alpha));
            const HRESULT draw = comp.d2dContext->EndDraw();
            if (frames == 0) Report("first EndDraw", draw);

            comp.d2dContext->SetTarget(nullptr);
            comp.frame.Reset();

            present = comp.swapChain->Present(1, 0);
            if (frames == 0) Report("first Present", present);
            ++frames;
        } else {
            Sleep(30);
        }
    }

    std::printf("glassprobe: frames presented=%d, last present=0x%08lX\n", frames,
                static_cast<unsigned long>(present));
    std::fflush(stdout);

    DestroyWindow(hwnd);
    return 0;
}
