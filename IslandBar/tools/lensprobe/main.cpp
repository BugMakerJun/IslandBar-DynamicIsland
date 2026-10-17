// =====================================================================
//  lensprobe -- can the island get the DESKTOP's pixels, not just a frosted colour?
//
//      lensprobe                       host backdrop brush, rounded clip, 220x36, 20 s
//      lensprobe --mode blur          the same, through a Gaussian blur effect
//      lensprobe --at 1180,300 --size 300x40 --seconds 30
//
//  WHY THIS EXISTS. The second material window (app/GlassWindow) gives a REAL blurred backdrop with
//  ACCENT_ENABLE_ACRYLICBLURBEHIND -- and that is frosted glass, not Liquid Glass: acrylic hands back a
//  blurred COLOUR, never the pixels, so there is nothing to displace. Apple's material bends the backdrop
//  at the rim (a lens), and that needs the backdrop as a texture.
//
//  This probe asks the only question that decides whether a lens is possible at all: does
//  Windows.UI.Composition's CreateHostBackdropBrush actually deliver the desktop behind a per-pixel-alpha
//  Win32 window, and can that brush be clipped to an exact rounded shape? If yes, the same brush can be
//  fed to D2D effects -- a blur and a displacement map -- which is the lens, with no screen capture.
//
//  `--mode blur` is the second question, and it is the risky one: Compositor::CreateEffectFactory takes an
//  IGraphicsEffect, and this project has no Win2D. The effect below is hand-written against
//  IGraphicsEffect + IGraphicsEffectD2D1Interop (the same contract Win2D implements). If the factory
//  accepts it, the rest of the lens is D2D effect parameters.
//
//  ASCII-only on purpose. Do not add non-ASCII text to this file.
// =====================================================================

#include <windows.h>
#include <unknwn.h>   // BEFORE the C++/WinRT headers: the effect below implements a classic COM interface
#include <dispatcherqueue.h>
#include <d2d1effects.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>

#include <windows.graphics.effects.interop.h>
#include <windows.ui.composition.interop.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <functional>

namespace {

using winrt::Windows::UI::Composition::Compositor;
using winrt::Windows::UI::Composition::CompositionBrush;
using winrt::Windows::UI::Composition::CompositionEffectBrush;
using winrt::Windows::UI::Composition::CompositionEffectFactory;
using winrt::Windows::UI::Composition::CompositionEffectSourceParameter;
using winrt::Windows::UI::Composition::CompositionGeometricClip;
using winrt::Windows::UI::Composition::CompositionRoundedRectangleGeometry;
using winrt::Windows::UI::Composition::CompositionSpriteShape;
using winrt::Windows::UI::Composition::SpriteVisual;
using winrt::Windows::Graphics::Effects::IGraphicsEffect;
using winrt::Windows::Graphics::Effects::IGraphicsEffectSource;

int g_seconds = 20;
std::function<void()> g_setup;

// The effect CLSIDs in d2d1effects.h are DEFINE_GUID declarations whose definitions live in a GUID
// library this probe does not otherwise need, so the one it uses is written out here.
constexpr GUID kGaussianBlurEffect = {0x1feb6d69, 0x2fe6, 0x4ac9,
                                      {0x8c, 0x58, 0x1d, 0x7f, 0x93, 0xe7, 0xa6, 0xa5}};

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_TIMER) {
        if (wparam == 2) {
            KillTimer(hwnd, 2);
            if (g_setup) {
                auto setup = std::move(g_setup);
                g_setup = nullptr;
                setup();
            }
        } else {
            PostQuitMessage(0);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

// A Gaussian blur described BY HAND. This is the piece Win2D normally supplies: the compositor reads the
// effect graph through IGraphicsEffectD2D1Interop (which D2D effect id, which properties, in which order,
// and how a named property maps onto that order).
struct BlurEffect : winrt::implements<BlurEffect, IGraphicsEffect, IGraphicsEffectSource,
                                      ABI::Windows::Graphics::Effects::IGraphicsEffectD2D1Interop> {
    winrt::hstring name_{L"Blur"};
    IGraphicsEffectSource source_{nullptr};
    float sigma_ = 12.0f;

    // --- IGraphicsEffect ---
    winrt::hstring Name() { return name_; }
    void Name(winrt::hstring const& value) { name_ = value; }
    IGraphicsEffectSource GetSource(uint32_t index) {
        return index == 0 ? source_ : IGraphicsEffectSource{nullptr};
    }
    void SetSource(uint32_t index, IGraphicsEffectSource const& source) {
        if (index == 0) source_ = source;
    }

    // --- IGraphicsEffectD2D1Interop ---
    HRESULT STDMETHODCALLTYPE GetEffectId(GUID* id) override {
        if (!id) return E_POINTER;
        *id = kGaussianBlurEffect;
        std::printf("lensprobe:   effect id asked for\n");
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetNamedPropertyMapping(
        LPCWSTR name, UINT* index,
        ABI::Windows::Graphics::Effects::GRAPHICS_EFFECT_PROPERTY_MAPPING* mapping) override {
        if (!name || !index || !mapping) return E_POINTER;
        if (std::wcscmp(name, L"StandardDeviation") == 0) {
            *index = 0;
            *mapping = ABI::Windows::Graphics::Effects::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT;
            return S_OK;
        }
        if (std::wcscmp(name, L"Optimization") == 0) {
            *index = 1;
            *mapping = ABI::Windows::Graphics::Effects::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT;
            return S_OK;
        }
        if (std::wcscmp(name, L"BorderMode") == 0) {
            *index = 2;
            *mapping = ABI::Windows::Graphics::Effects::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT;
            return S_OK;
        }
        return E_INVALIDARG;
    }

    HRESULT STDMETHODCALLTYPE GetPropertyCount(UINT* count) override {
        if (!count) return E_POINTER;
        *count = 3;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetProperty(UINT index,
                                          ABI::Windows::Foundation::IPropertyValue** value) override {
        std::printf("lensprobe:   GetProperty(%u)\n", index);
        if (!value) return E_POINTER;
        *value = nullptr;
        if (index == 0) {
            auto boxed = winrt::box_value(sigma_).as<winrt::Windows::Foundation::IPropertyValue>();
            *value = reinterpret_cast<ABI::Windows::Foundation::IPropertyValue*>(
                winrt::detach_abi(boxed));
            return S_OK;
        }
        if (index == 1) {
            // D2D1_BLUR_OPTIMIZATION_BALANCED
            auto boxed = winrt::box_value(static_cast<uint32_t>(1))
                             .as<winrt::Windows::Foundation::IPropertyValue>();
            *value = reinterpret_cast<ABI::Windows::Foundation::IPropertyValue*>(
                winrt::detach_abi(boxed));
            return S_OK;
        }
        if (index == 2) {
            // D2D1_BORDER_MODE_HARD (= 1): the blur must not fade out at the edge of the backdrop.
            auto boxed = winrt::box_value(static_cast<uint32_t>(1))
                             .as<winrt::Windows::Foundation::IPropertyValue>();
            *value = reinterpret_cast<ABI::Windows::Foundation::IPropertyValue*>(
                winrt::detach_abi(boxed));
            return S_OK;
        }
        return E_INVALIDARG;
    }

    HRESULT STDMETHODCALLTYPE GetSource(
        UINT index, ABI::Windows::Graphics::Effects::IGraphicsEffectSource** source) override {
        std::printf("lensprobe:   GetSource(%u)\n", index);
        if (!source) return E_POINTER;
        *source = nullptr;
        if (index != 0) return E_INVALIDARG;
        if (!source_) return S_OK;
        auto* raw = reinterpret_cast<ABI::Windows::Graphics::Effects::IGraphicsEffectSource*>(
            winrt::get_abi(source_));
        raw->AddRef();
        *source = raw;
        std::printf("lensprobe:   -> source %p\n", static_cast<void*>(raw));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSourceCount(UINT* count) override {
        if (!count) return E_POINTER;
        *count = 1;
        std::printf("lensprobe:   GetSourceCount\n");
        return S_OK;
    }
};

void Report(const char* what, HRESULT hr) {
    std::printf("lensprobe: %s -> 0x%08lX\n", what, static_cast<unsigned long>(hr));
}

}  // namespace

int main(int argc, char** argv) {
    bool blur = false;
    bool colour = false;
    bool clip = true;
    bool normalWindow = false;
    bool targetBelow = false;
    float sigma = 12.0f;
    int x = 1180, y = 300, width = 220, height = 36;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            const char* mode = argv[++i];
            blur = std::strcmp(mode, "blur") == 0;
            colour = std::strcmp(mode, "colour") == 0;
        } else if (std::strcmp(argv[i], "--sigma") == 0 && i + 1 < argc) {
            sigma = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(argv[i], "--no-clip") == 0) {
            clip = false;
        } else if (std::strcmp(argv[i], "--normal") == 0) {
            normalWindow = true;
        } else if (std::strcmp(argv[i], "--target-below") == 0) {
            targetBelow = true;
        } else if (std::strcmp(argv[i], "--at") == 0 && i + 1 < argc) {
            std::sscanf(argv[++i], "%d,%d", &x, &y);
        } else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            std::sscanf(argv[++i], "%dx%d", &width, &height);
        } else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            g_seconds = std::atoi(argv[++i]);
        } else {
            std::printf("lensprobe: unknown argument '%s'\n", argv[i]);
        }
    }

    const winrt::apartment_type apartment = winrt::apartment_type::single_threaded;
    winrt::init_apartment(apartment);

    // The dispatcher and the compositor BEFORE the window: the window's composition association is made
    // when it is created, and a window created first was the one configuration that drew nothing.
    PDISPATCHERQUEUECONTROLLER dispatcher = nullptr;
    {
        DispatcherQueueOptions options = {sizeof(DispatcherQueueOptions), DQTYPE_THREAD_CURRENT,
                                          DQTAT_COM_ASTA};
        Report("CreateDispatcherQueueController",
               CreateDispatcherQueueController(options, &dispatcher));
    }
    Compositor compositor{nullptr};
    try {
        compositor = Compositor();
    } catch (const winrt::hresult_error& error) {
        std::printf("lensprobe: Compositor threw 0x%08lX\n",
                    static_cast<unsigned long>(error.code().value));
        return 3;
    }

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"IslandBar.LensProbe";
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        std::printf("lensprobe: RegisterClassW failed (error %lu)\n", GetLastError());
        return 2;
    }

    DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
    if (!normalWindow) exStyle |= WS_EX_NOREDIRECTIONBITMAP;
    HWND hwnd = CreateWindowExW(exStyle, wc.lpszClassName, L"lensprobe", WS_POPUP, x, y, width,
                                height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        std::printf("lensprobe: CreateWindowExW failed (error %lu)\n", GetLastError());
        return 2;
    }

    // Shown BEFORE the target exists: a composition root attached to a window that has never been shown
    // may never be composed (the control colour below was invisible until this order was tried).
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    // ... and let the window LIVE for a moment before the target is attached: a HWND that has never
    // processed a message is not the same thing as a HWND DWM knows about.
    {
        const DWORD warmupUntil = GetTickCount() + 200;
        while (GetTickCount() < warmupUntil) {
            MSG warm = {};
            while (PeekMessageW(&warm, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&warm);
                DispatchMessageW(&warm);
            }
            Sleep(10);
        }
    }

    // THE ATTACHMENT RUNS INSIDE THE MESSAGE LOOP (timer id 2), not before it. A composition tree attached
    // to a window that has never been pumped is the configuration that drew nothing.
    g_setup = [&]() {
    try {
        // NOTE the namespace: the interop factory lives in ...Composition::Desktop, beside
        // IDesktopWindowTarget -- not directly under ...Composition (the SDK header's nesting).
        auto interop =
            compositor.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
        // The ABI interface IDesktopWindowTarget is only forward-declared by the interop header, so the
        // result is received as IInspectable and asked for the projected class afterwards.
        winrt::Windows::Foundation::IInspectable targetInspectable{nullptr};
        const HRESULT targetHr = interop->CreateDesktopWindowTarget(
            hwnd, targetBelow ? FALSE : TRUE,
            reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget**>(
                winrt::put_abi(targetInspectable)));
        Report("CreateDesktopWindowTarget", targetHr);
        if (FAILED(targetHr)) return;
        auto target = targetInspectable.as<winrt::Windows::UI::Composition::Desktop::DesktopWindowTarget>();

        CompositionBrush brush{nullptr};
        if (colour) {
            // THE CONTROL: if a plain colour does not show either, the composition target is not drawing at
            // all and any "the backdrop is empty" conclusion would be wrong.
            brush = compositor.CreateColorBrush(
                winrt::Windows::UI::ColorHelper::FromArgb(255, 220, 30, 30));
        } else if (blur) {
            auto effect = winrt::make_self<BlurEffect>();
            effect->sigma_ = sigma;
            CompositionEffectSourceParameter parameter(L"backdrop");
            effect->SetSource(0, parameter);
            std::printf("lensprobe: parameter=%d, effect source=%d\n", parameter ? 1 : 0,
                        effect->GetSource(0) ? 1 : 0);

            CompositionEffectFactory factory{nullptr};
            try {
                factory = compositor.CreateEffectFactory(
                    effect.as<IGraphicsEffect>());
            } catch (const winrt::hresult_error& error) {
                std::printf("lensprobe: CreateEffectFactory threw 0x%08lX (%ls)\n",
                            static_cast<unsigned long>(error.code().value),
                            error.message().c_str());
                return;
            }
            CompositionEffectBrush effectBrush = factory.CreateBrush();
            effectBrush.SetSourceParameter(L"backdrop", compositor.CreateHostBackdropBrush());
            brush = effectBrush;
        } else {
            brush = compositor.CreateHostBackdropBrush();
        }

        SpriteVisual visual = compositor.CreateSpriteVisual();
        visual.Brush(brush);
        visual.Size({static_cast<float>(width), static_cast<float>(height)});
        visual.RelativeSizeAdjustment({1.0f, 1.0f});
        visual.Offset({0.0f, 0.0f, 0.0f});

        // The EXACT shape, not DWM's fixed ~8 px: the clip is a rounded rectangle whose radius is half
        // the height, i.e. the island's stadium.
        if (clip) {
            CompositionRoundedRectangleGeometry geometry = compositor.CreateRoundedRectangleGeometry();
            geometry.Size({static_cast<float>(width), static_cast<float>(height)});
            geometry.CornerRadius({height * 0.5f, height * 0.5f});
            CompositionGeometricClip geometricClip = compositor.CreateGeometricClip();
            geometricClip.Geometry(geometry);
            visual.Clip(geometricClip);
        }

        target.Root(visual);
        SetWindowPos(hwnd, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        RECT client = {};
        GetClientRect(hwnd, &client);
        std::printf("lensprobe: client %ldx%ld, visual %.0fx%.0f, root %.0fx%.0f\n",
                    client.right - client.left, client.bottom - client.top, visual.Size().x,
                    visual.Size().y, visual.Size().x, visual.Size().y);
        std::printf("lensprobe: mode=%s sigma=%.1f, window %d,%d %dx%d\n",
                    colour ? "colour-control" : (blur ? "host-backdrop+blur" : "host-backdrop"),
                    sigma, x, y, width, height);
        std::printf("lensprobe: visible=%d, on screen for %d s\n", IsWindowVisible(hwnd) ? 1 : 0,
                    g_seconds);
        std::fflush(stdout);
        SetTimer(hwnd, 1, static_cast<UINT>(g_seconds) * 1000, nullptr);
    } catch (const winrt::hresult_error& error) {
        std::printf("lensprobe: WinRT threw 0x%08lX (%ls)\n",
                    static_cast<unsigned long>(error.code().value), error.message().c_str());
        DestroyWindow(hwnd);
        PostQuitMessage(0);
    }
    };
    SetTimer(hwnd, 2, 100, nullptr);

    // A BLOCKING GetMessage loop: the compositor's commit work is delivered through this thread's queue.
    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    DestroyWindow(hwnd);
    return 0;
}
