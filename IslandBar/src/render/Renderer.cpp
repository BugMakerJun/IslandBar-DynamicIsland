// =====================================================================
//  Renderer.cpp
// =====================================================================
#include "render/Renderer.h"
#include "render/Ambient.h"   // the island's weather: rain, sunset, night, the slow sun
#include "render/Theme.h"   // the material: two stops, a highlight, a hairline, an accent and an ink
#include "render/GlassMaterial.h"   // the shadow under the shape and the grain over it

#include "core/Log.h"
// Reused rather than reimplemented, so the music card's source icon and a taskbar button's icon
// cannot drift apart: DecodeImage turns the icon PNG into pixels, FetchAppIconForTest is the one
// executable-to-icon loader in the project.
#include "media/MusicSession.h"
#include "media/WicImage.h"
#include "platform/IconRaster.h"   // an HICON to top-down BGRA pixels, shared with media/

#include <wrl/client.h>

#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")

namespace island {
namespace {

template <typename T>
void SafeRelease(T** pp) {
    if (pp && *pp) {
        (*pp)->Release();
        *pp = nullptr;
    }
}

// The swap chain surface is created in DIPs and the composition visual scales it
// to physical pixels. At 100% scaling the two are the same thing, so this costs
// nothing on the common case; on a scaled display the GPU upsamples rather than
// the window being resized, which is the trade that avoids the resize flicker.
//
// The consequence to remember: glyphs are rendered at DIP resolution and
// magnified. If that looks soft on a 150% display, the fix is to create the
// bitmap at physical size and divide the composition transform instead -- the
// layout code, which is all in DIPs, does not have to change.
UINT DipToSurfacePixels(LONG pixels, float scale) {
    if (scale <= 0.0f) return static_cast<UINT>(std::max<LONG>(pixels, 1));
    const float dip = static_cast<float>(pixels) / scale;
    return static_cast<UINT>(std::max(1.0f, std::floor(dip + 0.5f)));
}

// The window the lens samples: the first visible top-level window below ours in the Z order. Windows
// of this process are skipped -- the bar, the settings popup, the backdrop window -- because capturing
// one of them would either show the island itself or an empty surface.
bool IsBehindCandidate(HWND window, DWORD ownPid) {
    if (!window || !IsWindowVisible(window)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == ownPid) return false;
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
        return false;
    }
    if (GetAncestor(window, GA_ROOT) != window) return false;
    RECT box = {};
    if (!GetWindowRect(window, &box)) return false;
    return (box.right - box.left) >= 32 && (box.bottom - box.top) >= 32;
}

HWND FindWindowBehind(HWND self, POINT at, POINT probe) {
    if (!self) return nullptr;
    const DWORD ownPid = GetCurrentProcessId();
    // WHAT IS ACTUALLY ON SCREEN wins over the Z-order walk. `probe` is just below the island, where
    // the island itself is click-through, so WindowFromPoint answers with the window the user sees --
    // including a desktop wallpaper window that sits above a maximised browser. The walk below only
    // knows the Z-order of ordinary windows and picked that browser, which is how the lens ended up
    // showing a different image than the one around it.
    if (probe.x != at.x || probe.y != at.y) {
        HWND hit = GetAncestor(WindowFromPoint(probe), GA_ROOT);
        if (hit && IsBehindCandidate(hit, ownPid)) {
            RECT box = {};
            if (GetWindowRect(hit, &box) && PtInRect(&box, at)) return hit;
        }
    }
    HWND walk = self;
    for (int step = 0; step < 64; ++step) {
        walk = GetWindow(walk, GW_HWNDNEXT);
        if (!walk) break;
        if (!IsBehindCandidate(walk, ownPid)) continue;
        // The first window BELOW us is not necessarily under us -- a maximised editor can start
        // below the bar while the desktop is what the bar actually covers. Only a window whose rect
        // contains the sample point can supply the pixels.
        RECT box = {};
        if (GetWindowRect(walk, &box) && PtInRect(&box, at)) return walk;
    }
    // The desktop is the parent of every top-level window, so it is not in that chain at all: over
    // bare wallpaper the loop above finds nothing. Progman owns the wallpaper and is the one surface
    // left to sample.
    HWND desktop = FindWindowW(L"Progman", nullptr);
    if (desktop && IsBehindCandidate(desktop, ownPid)) {
        RECT box = {};
        if (GetWindowRect(desktop, &box) && PtInRect(&box, at)) return desktop;
    }
    return nullptr;
}

} // namespace

Renderer::~Renderer() {
    Shutdown();
}

bool Renderer::CreateDeviceResources() {
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &d3dDevice_, nullptr, &d3dContext_);

    if (FAILED(hr)) {
        // A remote session or a constrained GPU may have no hardware device.
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
            &d3dDevice_, nullptr, &d3dContext_);
    }
    if (FAILED(hr) || !d3dDevice_) return false;

    IDXGIDevice* dxgiDevice = nullptr;
    hr = d3dDevice_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
    if (FAILED(hr) || !dxgiDevice) return false;

    // DirectComposition wraps the DXGI device. This must happen while the DXGI
    // device is still alive: passing a released pointer here was the original
    // 0xC0000005 crash.
    hr = DCompositionCreateDevice(dxgiDevice, __uuidof(IDCompositionDevice),
                                  reinterpret_cast<void**>(&compDevice_));
    if (FAILED(hr) || !compDevice_) {
        SafeRelease(&dxgiDevice);
        return false;
    }

    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory3),
                           nullptr, reinterpret_cast<void**>(&d2dFactory_));
    if (FAILED(hr) || !d2dFactory_) {
        SafeRelease(&dxgiDevice);
        return false;
    }

    hr = d2dFactory_->CreateDevice(dxgiDevice, &d2dDevice_);
    SafeRelease(&dxgiDevice);   // last use, released exactly once
    if (FAILED(hr) || !d2dDevice_) return false;

    hr = d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext_);
    if (FAILED(hr) || !d2dContext_) return false;

    // The context stays at 96 dpi on purpose so one DIP is one surface pixel;
    // the composition transform applies the monitor scaling.
    d2dContext_->SetDpi(96.0f, 96.0f);
    d2dContext_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    d2dContext_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    // Opacity is set through an effect group because DCompositionCreateDevice
    // returns the first-generation device, whose visuals do not implement
    // IDCompositionVisual3 (see HANDOFF.md 6.3).
    hr = compDevice_->CreateEffectGroup(&effectGroup_);
    if (FAILED(hr)) return false;

    // Optional: present on the compositor's clock. Absent before Windows 10 1809.
    if (HMODULE dcomp = GetModuleHandleW(L"dcomp.dll")) {
        waitForClock_ = reinterpret_cast<WaitForCompositorClockFn>(
            reinterpret_cast<void*>(GetProcAddress(dcomp, "DCompositionWaitForCompositorClock")));
    }

    return true;
}

bool Renderer::CreateSwapChain(UINT widthPx, UINT heightPx) {
    IDXGIDevice* dxgiDevice = nullptr;
    HRESULT hr = d3dDevice_->QueryInterface(__uuidof(IDXGIDevice),
                                            reinterpret_cast<void**>(&dxgiDevice));
    if (FAILED(hr) || !dxgiDevice) return false;

    IDXGIAdapter* adapter = nullptr;
    hr = dxgiDevice->GetAdapter(&adapter);
    SafeRelease(&dxgiDevice);
    if (FAILED(hr) || !adapter) return false;

    // The adapter is kept, not merely used to reach the factory: the memory line asks it how much the
    // driver has charged this process on the GPU. A failure here is not fatal -- an adapter that cannot
    // answer simply leaves the line's VRAM fields at "n/a".
    SafeRelease(&dxgiAdapter3_);
    adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&dxgiAdapter3_));

    IDXGIFactory2* factory = nullptr;
    hr = adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));
    SafeRelease(&adapter);
    if (FAILED(hr) || !factory) return false;

    width_  = DipToSurfacePixels(static_cast<LONG>(widthPx), scale_);
    height_ = DipToSurfacePixels(static_cast<LONG>(heightPx), scale_);

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width              = width_;
    desc.Height             = height_;
    desc.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Stereo             = FALSE;
    desc.SampleDesc.Count   = 1;          // flip-model swap chains require 1
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount        = 2;
    desc.Scaling            = DXGI_SCALING_STRETCH;
    desc.SwapEffect         = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    // The waitable object is the pacing the frame loop uses; the flag has to be here, in the
    // description, because it cannot be added to a live swap chain afterwards.
    desc.Flags              = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    // Premultiplied alpha is what makes the area outside the pill genuinely
    // transparent instead of a black rectangle.
    desc.AlphaMode          = DXGI_ALPHA_MODE_PREMULTIPLIED;

    hr = factory->CreateSwapChainForComposition(d3dDevice_, &desc, nullptr, &swapChain_);
    SafeRelease(&factory);
    if (FAILED(hr) || !swapChain_) return false;

    // One frame in flight, and the object that says when. Both halves matter: the limit without the
    // wait is a queue nobody waits for, and the wait without the limit is a signal that fires while two
    // frames are already queued -- which is the state the loop was in when it presented with sync
    // interval 1 and waited twice per frame.
    if (SUCCEEDED(swapChain_->QueryInterface(__uuidof(IDXGISwapChain2),
                                             reinterpret_cast<void**>(&swapChain2_))) && swapChain2_) {
        swapChain2_->SetMaximumFrameLatency(1);
        frameLatencyWaitable_ = swapChain2_->GetFrameLatencyWaitableObject();
    }

    return true;
}

void Renderer::ReleaseTarget() {
    if (d2dContext_) {
        d2dContext_->SetTarget(nullptr);
    }
    SafeRelease(&d2dTarget_);
}

bool Renderer::Initialize(HWND hwnd, UINT widthPx, UINT heightPx, float scale) {
    Shutdown();

    scale_ = (scale > 0.0f) ? scale : 1.0f;
    dpi_   = static_cast<UINT>(std::lround(scale_ * kDipPerInch));
    window_ = hwnd;

    if (!CreateDeviceResources()) {
        Shutdown();
        return false;
    }
    if (!CreateSwapChain(widthPx, heightPx)) {
        Shutdown();
        return false;
    }
    if (!BindToWindow(hwnd, scale_)) {
        Shutdown();
        return false;
    }

    // THE LENS (theme.lens): the desktop behind the island, displaced at the rim. A failure is not fatal.
    if (CurrentTheme().lens) {
        // The capture itself starts per frame, on the window under the island (see UpdateLensCapture).
        lensActive_ = lens_.Initialize(d2dDevice_);
        if (!lensActive_) LogLine(L"  lens: off -- the island keeps its painted material");
        else LogLine(L"  lens: material ready for theme %s", CurrentTheme().name);
    }

    return true;
}

bool Renderer::TakeLensFrame() {
    if (!lensActive_ || !capture_.HasNewFrame()) return false;
    const ULONGLONG now = GetTickCount64();
    if (now < lensNextAt_) return false;
    lensNextAt_ = now + 80;
    return true;
}

void Renderer::UpdateLensCapture(POINT at, POINT probe) {
    if (!lensActive_ || !window_) return;
    const HWND behind = FindWindowBehind(window_, at, probe);
    const bool lost = lensWindow_ && (!IsWindow(lensWindow_) || capture_.SizeChanged());
    if (behind == lensWindow_ && !lost) return;

    capture_.Stop();
    lensWindow_ = nullptr;
    if (!behind) return;
    const ULONGLONG now = GetTickCount64();
    if (now < lensRetryAt_) return;
    if (capture_.StartForWindow(d3dDevice_, behind)) {
        lensWindow_ = behind;
        lensRetryAt_ = 0;
    } else {
        // A window that refuses capture (protection, a child of the shell, a closed session) must not
        // be retried thirty times a second.
        lensRetryAt_ = now + 2000;
    }
}

// widthPx and heightPx are not needed here: the swap chain is created once at a
// fixed size and never resized, so binding only has to attach it to the window
// and apply the DIP-to-pixel transform.
bool Renderer::BindToWindow(HWND hwnd, float scale) {
    if (!compDevice_ || !swapChain_) return false;

    scale_ = (scale > 0.0f) ? scale : 1.0f;
    dpi_   = static_cast<UINT>(std::lround(scale_ * kDipPerInch));

    // Each composition target takes exactly one root visual. A second
    // SetRoot on the same target is rejected, so the visual is created once
    // and the target is only created once.
    if (!compVisual_) {
        HRESULT hr = compDevice_->CreateVisual(&compVisual_);
        if (FAILED(hr) || !compVisual_) return false;

        hr = compVisual_->SetContent(swapChain_);
        if (FAILED(hr)) return false;

        hr = compVisual_->SetEffect(effectGroup_);
        if (FAILED(hr)) return false;

        hr = compDevice_->CreateTargetForHwnd(hwnd, TRUE /* topmost */, &compTarget_);
        if (FAILED(hr) || !compTarget_) return false;

        hr = compTarget_->SetRoot(compVisual_);
        if (FAILED(hr)) return false;
    }

    // DIP -> physical pixels. Layout stays in DIPs everywhere upstream.
    D2D1_MATRIX_3X2_F transform = D2D1::Matrix3x2F::Scale(scale_, scale_);
    HRESULT hr = compVisual_->SetTransform(transform);
    if (FAILED(hr)) return false;

    return SUCCEEDED(compDevice_->Commit());
}

void Renderer::Render(const FrameState& frame) {
    RenderFrame(frame);
}

// The application icon row and the left deck, in one place so their draw ORDER can be controlled.
//
// Called after the card. Their geometry comes from the same pure functions the hit tests use, so moving
// the calls cannot move the buttons.
void Renderer::DrawDecks(const RoundedBox& island, const FrameState& frame) {
    // The icon row's buttons, RESOLVED HERE.
    //
    // This work used to sit in RenderFrame, after the card and gated to the short states. Both halves of
    // that were wrong: the row is not a short-state feature (the card is drawn OVER it, not instead of
    // it), and it has to happen before the row is drawn. When the decks moved after the card, the
    // preparation was left behind and the row was laid out from an empty taskButtons_ -- no icons, no
    // background, and no amount to open with, so it drew nothing at all.
    //
    // The icons are resolved here rather than by the caller because this is where the GPU bitmaps and
    // their cache live, so the fetch and decode happen once per window rather than once per button per
    // frame.
    // The task buttons' body is the island's own pill fill -- the same 0.08 the glass pill has. The 0.65
    // average colour that was here was a flat, milky disc: "雾蒙蒙", and not the island. The live wallpaper
    // shows through the same body the island uses; what the row does not get is the pill's WGC warp (that
    // would be a second lens for the row, not a colour).
    for (int i = 0; i < frame.taskButtonCount && i < FrameState::kMaxTaskButtons; ++i) {
        taskButtons_[i] = frame.taskButtons[i];
        taskButtons_[i].icon = IconForWindow(frame.taskButtonWindow[i]);
        taskButtons_[i].fill = frame.pill.fill;

        // Reported once per window, the first time its icon is resolved. "A button has no icon" has
        // three causes a screenshot cannot separate -- the window has none, the icon could not become a
        // bitmap, or the draw skipped it -- and this tells them apart instead of the next change being a
        // guess.
        static HWND reported[64] = {};
        static int reportedCount = 0;
        bool seen = false;
        for (int r = 0; r < reportedCount; ++r) {
            if (reported[r] == frame.taskButtonWindow[i]) seen = true;
        }
        if (!seen && reportedCount < 64) {
            reported[reportedCount++] = frame.taskButtonWindow[i];
            LogLine(L"  taskbar icon: hwnd %p -> %s", reinterpret_cast<void*>(frame.taskButtonWindow[i]),
                    taskButtons_[i].icon ? L"ok" : L"NONE");
        }
    }
    // Every button's OWN opening amount for the icon row, taken from the buttons just resolved. The
    // indicators are NOT part of it: they are on the bar's other deck, so they neither occupy its slots
    // nor move with them.
    float rowAmounts[FrameState::kMaxTaskButtons] = {};
    for (int i = 0; i < frame.taskButtonCount && i < FrameState::kMaxTaskButtons; ++i) {
        rowAmounts[i] = taskButtons_[i].hoverAmount;
    }

    // The card's placement, so the row can start past it rather than under it.
    //
    // Zero when no card is up, which is every state except the system panel's. The card hangs from the
    // system button and spans rightwards over where this row would otherwise begin.
    // Whether a card is up OR ON ITS WAY, taken from the motion so it holds for the whole of both the
    // opening and the closing animation. See FrameState::outerCardUp.
    const float cardLeftDip = frame.outerCardUp ? frame.cardLeftDip : 0.0f;
    const float cardWidthDip = frame.outerCardUp ? kExpandedWidthDip : 0.0f;

    // THE ROW STARTS AFTER THE TRAY BUTTON, which stands immediately to the right of the island. Told to
    // the row as one moved edge rather than as a parameter through every one of its functions -- see
    // RowAnchorWithTray.
    const RoundedBox rowIsland = RowAnchorWithTray(island, frame.trayIconCount > 0);
    text_->DrawTaskIcons(d2dContext_, rowIsland, taskButtons_, frame.taskButtonCount,
                         frame.taskHoveredIndex, frame.contentOpacity, rowAmounts,
                         frame.taskButtonCount, cardLeftDip, cardWidthDip,
                         frame.taskButtonAppearValid ? frame.taskButtonAppear : nullptr);

    // THE TRAY BUTTON, at the far right of the bar: one control for every tray icon, with the icons behind
    // it in the card (see DrawTrayButton). After the application icons, so the two read as "what I have
    // open" then "what is running quietly", which is the order the shell's own taskbar has.
    //
    // Not gated on `fullscreenApp` here: that is handled where the whole deck is skipped (the caller),
    // because while a game has the screen the tray is not the bar either.
    if (frame.trayIconCount > 0) {
        // No fill either, for the reason the deck's buttons have none: it stands on the same surface (see
        // BarClusterFor). Its own hover wash and the three dots are what is drawn.
        text_->DrawTrayButton(d2dContext_, TrayButtonFor(island), frame.trayButtonWash,
                              frame.trayPageOpen,
                              D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f), frame.contentOpacity);
    }

    // What the row was actually asked to draw, and where it put it.
    //
    // Reported on change of the answer, so the log has one line per movement rather than one per frame.
    // This exists because "the row does not make room" survived two rounds of reasoning: the layout
    // arithmetic was verified correct in isolation, the values were verified present in the binary, and
    // the remaining possibilities -- the index never arriving, the amount never rising, the geometry
    // landing in the wrong place -- are indistinguishable from a screenshot. This line names which one.
    //
    // It lives beside the row rather than in the caller because it describes the geometry the row was
    // GIVEN, and the caller no longer knows that the row exists at all.
    {
        std::wstring summary;
        wchar_t piece[96] = {};
        for (int i = 0; i < frame.taskButtonCount; ++i) {
            const RectF rect =
                TaskIconButtonAtAmounts(island, i, frame.taskButtonCount, rowAmounts);
            std::swprintf(piece, 96, L" [%d %.0f..%.0f h%.2f]", i, rect.left, rect.right,
                          rowAmounts[i]);
            summary += piece;
        }
        if (summary != lastRowSummary_) {
            lastRowSummary_ = summary;
            LogLine(L"  row: hovered=%d, %d icons on the right%s", frame.taskHoveredIndex,
                    frame.taskButtonCount, summary.c_str());
        }

        // The property, checked against the same widths the summary above used.
        //
        // Reported once per run: the first overlap is the finding and the remaining hundreds are the
        // same finding repeated. It matters because a button that is still CLOSING overlaps one that is
        // opening, and a layout that only makes room for the hovered button gets exactly that.
        if (!reportedOverlap_) {
            for (int i = 0; i + 1 < frame.taskButtonCount; ++i) {
                const RectF a =
                    TaskIconButtonAtAmounts(island, i, frame.taskButtonCount, rowAmounts);
                const RectF b =
                    TaskIconButtonAtAmounts(island, i + 1, frame.taskButtonCount, rowAmounts);
                if (a.right > b.left + 0.01f) {
                    reportedOverlap_ = true;
                    LogLine(L"  OVERLAP: button %d ends at %.1f but button %d starts at %.1f "
                            L"(amounts %.2f / %.2f, hovered=%d)",
                            i, a.right, i + 1, b.left, taskButtons_[i].hoverAmount,
                            taskButtons_[i + 1].hoverAmount, frame.taskHoveredIndex);
                    break;
                }
            }
        }
    }

    // The system indicators used to be drawn here, out at the window's left margin. They are gone: the
    // request was that the left deck hold the Start and system buttons and nothing else. Their reader,
    // their glyph table and their hit test went with them.

    // The Start button, nearest the island on its left, and the system panel's toggle before it.
    //
    // Both come from the same deck function the hit tests use, so a click cannot land beside the button
    // that was clicked.
    //
    // NO FILL of their own: the surface they stand on is drawn once, wider, by the frame loop
    // (BarClusterFor), and a button that painted its own rounded rectangle inside it would put the beads
    // straight back. What is left is the mark itself and the hover wash, which is what a control should be.
    const D2D1_COLOR_F onTheCluster = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);
    text_->DrawStartButton(d2dContext_, island, frame.startWash, frame.startButtonOpen, onTheCluster,
                           frame.contentOpacity);

    // `open` points the chevron up, which is the one thing the control has to say by itself: whether
    // pressing it again will put away what is already there.
    text_->DrawSystemButton(d2dContext_, SystemButtonFor(island), frame.systemButtonWash,
                            frame.systemButtonOpen, onTheCluster, frame.contentOpacity);
}

void Renderer::RenderFrame(const FrameState& frame) {
    if (!d2dContext_ || !swapChain_) return;

    const PillAppearance& pill = frame.pill;

    // Opacity through the effect group, never through Visual3 (unavailable on
    // the first-generation device this API returns). When AnimateOpacity owns
    // the value, setting it here as well would fight the animation, so the
    // caller signals that by leaving opacity at 1.0 and letting the group drive
    // it -- see AnimateOpacity.
    if (effectGroup_ && !opacityAnimated_) {
        effectGroup_->SetOpacity(pill.opacity);
    }

    // The D2D target bitmap wraps the swap chain's back buffer and is valid
    // only until the next Present, so it is created and dropped per frame.
    ReleaseTarget();

    IDXGISurface* surface = nullptr;
    HRESULT hr = swapChain_->GetBuffer(0, __uuidof(IDXGISurface),
                                       reinterpret_cast<void**>(&surface));
    if (FAILED(hr) || !surface) return;

    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);

    hr = d2dContext_->CreateBitmapFromDxgiSurface(surface, &props, &d2dTarget_);
    SafeRelease(&surface);
    if (FAILED(hr) || !d2dTarget_) return;

    d2dContext_->SetTarget(d2dTarget_);

    // Everything that touches pixels must be inside the BeginDraw/EndDraw pair --
    // Clear included. D2D silently discards drawing commands issued outside it,
    // which is how an earlier revision of this file produced a perfectly
    // transparent frame while every call returned success: the Clear and the
    // fill both ran outside the pair, so the surface was never written at all.
    LARGE_INTEGER renderStart = {};
    QueryPerformanceCounter(&renderStart);

    d2dContext_->BeginDraw();

    d2dContext_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    // A FULL-SCREEN APPLICATION KEEPS THE ISLAND. It used to draw nothing at all in that state; the
    // request is now the opposite: the pill stays, only the decks and the icon row stand down (see
    // IslandSurfaceShowsDecks and the fullscreenApp tests further down). The pill is the whole point of
    // the bar, and a full-screen game should not make it disappear.
    if (pill.opacity > 0.001f) {
        // The PILL is drawn from the resting box, not from the animated one.
        //
        // The animated box belongs to the CARD: it grows from the pill to the card and back, and while
        // it is card-sized this fill would paint a card-sized rounded rectangle in the pill's near-opaque
        // black -- a dark band across the screen for the whole of every transition. The caller supplies
        // the resting box alongside the animated one so the two cannot be confused here.
        //
        // Named `pillBox`, NOT `box`. It was first written as a reference named `box`, which SHADOWED the
        // outer animated `box` for the rest of the function -- so the card was composed from the pill's
        // resting rectangle and the panel came out 220 x 36 with the pill's own corner radius. A shadowed
        // name is invisible at the point of use.
        // THE ISLAND'S OWN OUTLINE: the anchor box, which is the pill in every state but the notice strip.
        //
        // It used to be the RESTING pill unconditionally, and the reason was sound -- in the card state the
        // animated box is 520x250, and filling that with the pill's near-opaque black painted a dark band
        // across the screen for the whole of a transition. But that rule also meant the island COULD NOT
        // WIDEN, and the notice strip was added later: the fill stayed 220 DIP wide while the text -- drawn
        // from the animated box -- overflowed onto the desktop. Measured on screen: a dark run through the
        // island's centre 220 DIP wide with the message hanging outside it. Reported as "没有动画啊", which is
        // what an island that does not move looks like; what looked like a wide pill in the first screenshots
        // was the text drawn over a dark patch of wallpaper.
        //
        // RowAnchorBox is the same rule the icon row, the deck, the region and the hit tests already use: a
        // shape wider than the pill and shorter than a card is the strip; the card and the full-screen bar are
        // excluded, so the dark band cannot come back.
        const RoundedBox& pillBox =
            (frame.restingPill.rect.right > frame.restingPill.rect.left)
                ? island::RowAnchorBox(pill.box, frame.restingPill)
                : pill.box;

        // `box` is the ANIMATED shape for everything that is not the pill's own fill: the content, the
        // icon row, the indicators and the card all follow the animation, and only the pill's outline
        // must stay the pill. Bound here so the two are named apart and neither can shadow the other.
        const RoundedBox& box = pill.box;

        // Assembled field by field rather than through D2D1::RoundedRect: that
        // helper takes a D2D1_SIZE_F, not a pair of radii, and it is easy to
        // pass two floats to it by mistake.
        // ... AND THE ISLAND IS PART OF ONE SURFACE WITH THE CONTROLS AROUND IT, NOT A BEAD AMONG BEADS.
        //
        // The system button, the Start button, the island and the tray button are drawn as one rounded
        // surface -- see BarClusterFor for why those four and not the icon row -- and the controls are then
        // drawn ON it: their marks, their hover washes, the island's own line. That is why the fills handed
        // to those three buttons further down are transparent now, and why this fill covers their boxes.
        //
        // Gated on the same condition as the decks themselves (content visible, no full-screen application
        // in front), because the surface exists FOR them: with the decks skipped the case would be a wider
        // pill with nothing in it at either end.
        // The surface is assembled by the SAME function the glass window behind it is placed from
        // (TaskStripLayout.h), so the blur cannot end somewhere the drawing does not.
        const bool decksDrawn =
            island::IslandSurfaceShowsDecks(frame.contentOpacity, frame.fullscreenApp);
        RoundedBox shape =
            island::IslandSurfaceFromPillBox(pillBox, decksDrawn, frame.trayIconCount > 0);
        // A wide notice/call strip is drawn beside the island, and the glass surface has to reach its
        // ends: the film and the rim otherwise stop at the pill's edge, which read as a vertical knife
        // cut through the right-hand end of the strip.
        if (frame.statusStripOpacity > 0.01f && CurrentTheme().lens) {
            // Same anchor the strip itself is measured from, so the two cannot drift apart.
            const RoundedBox anchor =
                (frame.restingPill.rect.right > frame.restingPill.rect.left)
                    ? island::RowAnchorBox(box, frame.restingPill)
                    : box;
            const RoundedBox strip = island::StatusStripFor(anchor);
            shape.rect.left = std::min(shape.rect.left, strip.rect.left);
            shape.rect.right = std::max(shape.rect.right, strip.rect.right);
        }

        D2D1_ROUNDED_RECT rounded = {};
        rounded.rect.left    = shape.rect.left;
        rounded.rect.top     = shape.rect.top;
        rounded.rect.right   = shape.rect.right;
        rounded.rect.bottom  = shape.rect.bottom;
        rounded.radiusX      = std::max(0.0f, shape.radiusX);
        rounded.radiusY      = std::max(0.0f, shape.radiusY);

        // THE GLASS: two stops and a highlight, all from the theme (Theme.h).
        //
        // It used to be one flat colour at 92% alpha, which is what a bar with no material looks like --
        // a black rounded rectangle. The two stops are the plan's "reflected light": the surface is
        // lighter where the light is, and the highlight along the top is a narrow gradient rather than a
        // hard band, because a hard edge across the pill reads as a line drawn on it.
        //
        // The brushes are CACHED in GlassMaterial (2026-10-17): each gradient is built in a unit space and
        // moved with a transform, so an animated box costs a matrix rather than a new brush per frame.
        const island::Theme& theme = island::CurrentTheme();
        // The shadow FIRST, so the surface sits on it rather than the shadow on the surface. A theme with
        // shadow 0 (默认) draws nothing here, which is why adding all of this cannot change that one's
        // pixels. 18 DIP of reach is the brief's "blur 20px, opacity 20%", rounded to what the pill's own
        // size can carry without the shadow reading as a second object.
        island::DrawSoftShadow(d2dContext_, rounded, theme.shadow, 18.0f);
        // ... and the WALLPAPER's own colour mixed into the fill, clamped so the ink keeps its contrast
        // (see TintedFill). The bottom stop takes it a little less than the top: the surface is meant to
        // be lighter where the light is, and a wallpaper colour that lightens both ends equally flattens
        // the gradient that gives the pill its depth.
        D2D1_COLOR_F pillTopFill =
            island::TintedFill(theme, theme.pillTop, island::WallpaperTint(), 1.0f);
        D2D1_COLOR_F pillBottomFill =
            island::TintedFill(theme, theme.pillBottom, island::WallpaperTint(), 0.85f);
        // The surface itself: the two-stop tinted fill and its fringe hairline, from the cached material.
        // THE LENS FIRST: the glass theme's surface is the lens, with a 0.08 body just enough to keep the
        // pane from reading as a hole. There is no drawn rim: the body's boundary and the refraction are
        // the edge. Nothing painted sits on top of it except the pointer's light, when there is one.
        // THE NOTICE/CALL STRIP: the strip is wider than the pill, and a lens sized to the pill cut a
        // visible seam where its refraction ended ("the right side looks cut with a knife"). A call does
        // not go through statusStripOpacity, so the notice line and the call flag are part of the test.
        const bool stripOnScreen = frame.statusStripOpacity > 0.01f || frame.callUp ||
                                   (frame.noticeLine && frame.noticeLine[0] != '\0');
        if (lensActive_ && !stripOnScreen) {
            // A size change re-warps the LAST captured frame: the window behind did not change, so WGC
            // sends nothing new, and re-opening the session would hand back a blank first frame.
            const int pillW = static_cast<int>(std::lround((rounded.rect.right - rounded.rect.left) * scale_));
            const int pillH = static_cast<int>(std::lround((rounded.rect.bottom - rounded.rect.top) * scale_));
            bool reshaped = false;
            if (pillW != lensPillW_ || pillH != lensPillH_) {
                lensPillW_ = pillW;
                lensPillH_ = pillH;
                reshaped = true;
            }
            POINT origin = {0, 0};
            if (window_ && ClientToScreen(window_, &origin)) {
                const POINT lensAt = {
                    static_cast<LONG>(static_cast<float>(origin.x) +
                                      (rounded.rect.left + rounded.rect.right) * 0.5f * scale_),
                    static_cast<LONG>(static_cast<float>(origin.y) +
                                      (rounded.rect.top + rounded.rect.bottom) * 0.5f * scale_)};
                const POINT probe = {
                    lensAt.x,
                    static_cast<LONG>(static_cast<float>(origin.y) + rounded.rect.bottom * scale_ + 12.0f)};
                UpdateLensCapture(lensAt, probe);
            }
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            std::uint64_t revision = 0;
            const RECT source = capture_.SourceRect();
            if (capture_.Running() && ClientToScreen(window_, &origin)) {
                // A new captured frame re-warps the lens; without one the last warp is still the right
                // image to draw (WGC sends nothing while the window behind is static).
                if (capture_.AcquireLatest(&texture, &revision)) {
                    lens_.Update(texture.Get(), source, rounded, scale_,
                                 static_cast<float>(origin.x) + rounded.rect.left * scale_,
                                 static_cast<float>(origin.y) + rounded.rect.top * scale_);
                } else if (reshaped) {
                    if (ID3D11Texture2D* last = capture_.LatestTexture()) {
                        lens_.Update(last, source, rounded, scale_,
                                     static_cast<float>(origin.x) + rounded.rect.left * scale_,
                                     static_cast<float>(origin.y) + rounded.rect.top * scale_);
                    }
                }
                // Fed every frame from the cached reading, so the crossover finishes even when the
                // window behind has gone still and WGC is sending nothing.
                island::SetPillInkBackdrop(lens_.BackdropLuminance());
                lens_.Fill(d2dContext_, rounded, scale_, 1.0f);
            }
        }
        // The film lifts a touch while the pointer is on the island: the material answers the pointer
        // before anything moves, which is the cheap half of "the glass notices you".
        const float hoverLift = (frame.pointerOnIsland && CurrentTheme().lens) ? 1.12f : 1.0f;
        island::DrawSmokedBase(d2dContext_, rounded, theme, pillTopFill, pillBottomFill, hoverLift);

        // NO WASHES on the clear lens: the wide top ellipse, the blobs and the grain all left a veil over
        // the background. What is left is the refracted image, the 2.5 DIP rim band and its words.
        if (!theme.lens) {
            island::DrawSurfaceLighting(d2dContext_, rounded, theme, 1.0f);
            island::DrawTopSheen(d2dContext_, rounded, theme, frame.pillPointerFraction, 1.0f);
        } else {
            // The 2.5 DIP rim band: it follows the rounded ends (the horizontal top wash did not, which made
            // the caps look cut); bright top, almost nothing at the sides, a soft dark at the bottom.
            island::DrawLensRimBand(d2dContext_, rounded, theme, 1.0f);
            // ... and the light the glass gathers just inside its bottom edge, which is the thickness.
            island::DrawLensBottomCaustic(d2dContext_, rounded, theme, 1.0f);
        }

        // The rim: 0 in 玻璃 -- the drawn 1 DIP line read as a ring around the pill. The painted themes
        // still opt in.
        island::DrawThinRim(d2dContext_, rounded, theme, frame.pointerOnIsland ? 1.10f : 1.0f);

        // THE LIQUID-GLASS LAYERS (round 53, Theme.h): a specular highlight that follows the pointer along
        // the top edge, and the double edge -- a faint dark line outside, a bright one inside -- that reads
        // as the thickness of the glass. Both are drawn from the theme's own fields, so 默认 is unchanged.
        // The pointer's own light. Only while the pointer is ON the island: at rest the bar must be still,
        // and a highlight that stays wherever the pointer left it is a decoration, not a reflection.
        if (frame.pointerOnIsland) {
            island::DrawSpecularSweep(d2dContext_, rounded, theme, frame.pillPointerFraction, 1.0f);
        }

        // The end tints, for a theme that asks for them. 玻璃 sets `tint` to 0: the reference has no
        // coloured fringe.
        island::DrawEdgeTints(d2dContext_, rounded, theme, 1.0f);

        // NO EDGE AT ALL, at the user's request: no outside line, no dark thickness, no inner curvature
        // lines. The island is the light on the surface and its content, nothing drawn round it.

        // ... and the grain over the whole surface, which is what keeps a flat translucent colour from
        // reading as plastic. Same rule as the shadow: a theme that asks for none gets none.
        if (!theme.lens) island::DrawGrain(d2dContext_, rounded, theme.grain);

        // Content last, so it sits on top of the fill. It is drawn only when the
        // frame asks for it: at zero content opacity the text is skipped rather
        // than drawn transparent, which would still pay the layout cost.
        if (text_ && frame.contentOpacity > 0.01f) {
            // WHERE the pill's one line goes: the pill's own box, ALWAYS -- never the animated one.
            //
            // `box` is the ANIMATED shape, and there is a frame range where it is card-sized while no
            // card is up: an INTERRUPTED retraction. A pointer crossing the island quickly does exactly
            // that -- off, then back on three milliseconds later (the user's own log: `state:
            // music-strip` at 01:07:07.455 and `state: hover` at .458) -- and the retarget is a pill
            // state, so `outerCardUp` ("the shape now or its target is a card") goes FALSE while the
            // shape is still 250 DIP tall. The line is drawn VERTICALLY CENTRED in the box it is given,
            // so it dropped into the middle of the card and climbed back out over the remaining 0.16 s.
            // Reported as "播放音乐的时候 鼠标移入显示卡片 鼠标快速横移 就会出现这种情况 歌词会上下闪一下".
            //
            // pillBox is the pill BY DEFINITION -- RowAnchorBox of the animated box and the resting pill
            // -- and in every state that shares the pill's rectangle it IS that rectangle, so this
            // changes nothing except the frames in question. (`box` is still what the NOTICE STRIP's
            // message is drawn into: there the strip is the bar's own shape, and its widening is the
            // whole point.)
            const bool intoPill = frame.outerCardUp;
            const RoundedBox& lineBox = pillBox;

            // A CANARY: the bar's own line must never be drawn into a box taller than the bar.
            //
            // Reported ONCE per run, because the defect it watches for lasts one or two frames and a
            // screenshot at any sane cadence photographs the ends of it and calls it clean. It fired
            // before this rule existed: `outerCardUp` ("the shape now or its target is a card") went
            // false while the shape was still card-sized -- the pointer left the island and came back
            // three milliseconds later, retargeting a card's exit to a pill state -- and the line, which
            // is VERTICALLY CENTRED in its box, dropped into the middle of the card and climbed back out.
            // Reported as "播放音乐的时候 鼠标移入显示卡片 鼠标快速横移 就会出现这种情况 歌词会上下闪一下".
            {
                const float lineHeight = lineBox.rect.bottom - lineBox.rect.top;
                const float barHeight = frame.restingPill.rect.bottom - frame.restingPill.rect.top;
                static bool reportedOutOfPlace = false;
                if (!reportedOutOfPlace && barHeight > 0.0f && lineHeight > barHeight + 1.0f) {
                    reportedOutOfPlace = true;
                    LogLine(L"  WARNING: the bar's own line is being drawn in a box taller than the bar: "
                            L"[%.0f,%.0f %.0f,%.0f] against the bar [%.0f,%.0f %.0f,%.0f]",
                            lineBox.rect.left, lineBox.rect.top, lineBox.rect.right, lineBox.rect.bottom,
                            frame.restingPill.rect.left, frame.restingPill.rect.top,
                            frame.restingPill.rect.right, frame.restingPill.rect.bottom);
                }
            }

            // WHAT it says, in the order of what the user would rather not miss: a call, then a message,
            // then the lyric, then the time.
            //
            // The music case is keyed on the RESTING state and NOT on `state`.
            //
            // `state` is the target, so it is Expanded for the whole of a card: pressing the system
            // button (or hovering, which opens the music card) replaced the lyric with the clock while
            // the music kept playing. Reported as "我现在正在播放音乐 但是点击系统按钮的时候 灵动岛就不显示
            // 歌词了", with the pill showing 01:01 over a panel that has nothing to do with the music.
            // The resting state is the one thing a card does not change, so it decides now -- for the
            // whole of the travel in both directions, with no flicker at either end.
            // The lyric strip shows the current lyric INSTEAD of the clock: the strip
            // is the same height as the resting pill, so there is room for one line
            // and the time is the less useful of the two while music plays.
            // Keyed on WHAT IS BEING ANNOUNCED rather than on the state, which is the same correction the
            // music branch below already carries and for the same reason: `state` is the TARGET, so it is
            // Expanded for the whole of a card. This branch used to test `state == NoticeStrip`, so opening
            // the Start card (or the panel, or the tray page) while a call was running dropped the call out
            // of the strip and let the lyric take it -- the island looked like the ordinary playing bar for
            // as long as the card was up. Reported with two photographs of one call: the call strip before
            // the press, and the lyric in its place with the card open.
            //
            // The priority is the one this whole block is documented as having -- a call, then a message,
            // then the lyric, then the time -- and it now holds whatever the card is doing.
            const bool announcing = frame.callUp || (frame.noticeLine && frame.noticeLine[0] != '\0');

            // WHICH OF THE FOUR THINGS THE BAR SAYS, ONCE PER CHANGE, IN THE LOG.
            //
            // The bar has one line and four possible sources for it, and from a screenshot the four are
            // indistinguishable from a mistake in choosing between them -- "the island says the wrong thing"
            // has cost a round of reading this file every time it has been reported. So the choice is
            // written down: which source, the two states it was made from, whether a card is up, and the box
            // the line went into.
            {
                const char* source = announcing
                                         ? (frame.callUp ? "call" : "message")
                                         : (frame.state == IslandState::NoticeStrip
                                                ? "clock-in-strip"
                                                : (frame.restingState == IslandState::MusicStrip ? "lyric"
                                                                                                 : "date"));
                static const char* lastSource = nullptr;
                static bool lastIntoPill = false;
                static float lastBoxLeft = 0.0f;
                static float lastBoxRight = 0.0f;
                // The box is part of the change test on purpose: the first version watched only the source
                // and the pill flag, so a strip that COLLAPSED from 420 DIP to the pill's 220 while the
                // source stayed "call" printed nothing -- the one thing the report was about was invisible
                // in the line written to make it visible.
                if (source != lastSource || intoPill != lastIntoPill ||
                    box.rect.left != lastBoxLeft || box.rect.right != lastBoxRight) {
                    lastSource = source;
                    lastIntoPill = intoPill;
                    lastBoxLeft = box.rect.left;
                    lastBoxRight = box.rect.right;
                    LogLine(L"  strip says: %hs (state %ls, resting %ls, cardUp %d, intoPill %d, "
                            L"box %.0f..%.0f)",
                            source, StateName(frame.state), StateName(frame.restingState),
                            frame.outerCardUp ? 1 : 0, intoPill ? 1 : 0, box.rect.left, box.rect.right);
                }
            }

            if (announcing) {
                // WHERE: the strip while the strip is the bar's own shape, and the PILL once a card is up.
                // The same rule the lyric below follows with `lineBox`, and for the same reason -- once a
                // card is up, `box` IS the card, and the bar's own line belongs in the pill that is still
                // on screen beneath it.
                const RoundedBox& noticeBox = intoPill ? lineBox : box;
                if (frame.callUp) {
                    // A CALL first: same strip, different thing to say -- and it says it with a timer and a
                    // level meter rather than with words, because that is what the reference does.
                    text_->DrawCallStrip(d2dContext_, noticeBox, frame.callStrip, frame.contentOpacity);
                } else {
                    // The message, in place of the clock -- the same trade the lyric strip makes, and for the
                    // same reason: while the strip is up, the time is the less useful of the two things the
                    // one line could hold.
                    //
                    // The track-line draw is reused rather than a new one written: it already scrolls a line
                    // that is too long for its box, which is exactly what a long title needs in a 420 DIP strip.
                    text_->DrawTrackLine(d2dContext_, noticeBox, frame.noticeLine, 0.0f,
                                         frame.contentOpacity);
                }
            } else if (frame.state == IslandState::NoticeStrip) {
                // The strip is up with nothing to announce -- the clock is the only thing it can still say,
                // and it keeps the strip's own box rather than the pill's, which is what it did before this
                // branch was split.
                text_->DrawClock(d2dContext_, box, frame.clock, frame.contentOpacity);
            } else if (frame.restingState == IslandState::MusicStrip) {
                // The wave first, so the words are on top of it. Only while music is what the bar is
                // showing: a wave is a reading of the music, not of the bar.
                if (frame.musicLevelKnown && frame.contentOpacity > 0.01f) {
                    text_->DrawAudioWave(d2dContext_, lineBox, frame.musicWavePhase, frame.musicLevel,
                                         frame.musicAccent, frame.contentOpacity);
                }
                if (frame.lyricLine && frame.lyricLine[0] != '\0') {
                    TextRenderer::LineRoll fade;
                    fade.previous = frame.lyricLinePrev;
                    fade.progress = frame.lyricLineRoll;
                    fade.previousScrollDip = frame.lyricLinePrevScrollDip;
                    text_->DrawLyricLine(d2dContext_, lineBox, frame.lyricLine,
                                         frame.lyricScrollDip, frame.contentOpacity, fade);
                } else if (frame.fallbackLine && frame.fallbackLine[0] != '\0') {
                    // No lyrics available: the track name is the honest fallback.
                    TextRenderer::LineRoll fade;
                    fade.previous = frame.lyricLinePrev;
                    fade.progress = frame.lyricLineRoll;
                    fade.previousScrollDip = frame.lyricLinePrevScrollDip;
                    text_->DrawTrackLine(d2dContext_, lineBox, frame.fallbackLine,
                                         frame.lyricScrollDip, frame.contentOpacity, fade);
                } else {
                    text_->DrawClock(d2dContext_, lineBox,
                                     intoPill ? ClockStyle::Compact : frame.clock,
                                     frame.contentOpacity);
                }
            } else {
                // THE DATE AND THE TIME, IN THE PILL, WHICH IS STILL ON SCREEN BENEATH A CARD.
                //
                // This is the state the island is in when it has NOTHING ELSE TO SAY -- no call, no
                // message, no lyric -- which is what makes it the home of the date / weekday / time /
                // weather line. The strip beside the deck takes it the moment the pill's line is
                // taken, and only then; the two are exclusive by construction (StatusPlacementFor).
                //
                // WHAT USED TO BE HERE, because both are still live rules:
                //
                //   * the pill's own box, never the animated `box`. Drawn into `box` this branch put a
                //     floating clock and date at the CARD's centred position while the panel hung from
                //     its button -- two card-like boxes on screen, reported four times as "a second box
                //     under the island".
                //   * the pill is drawn from its RESTING box in every state, so a card puts a hole in
                //     the middle of the bar where the time was, and skipping this draw in the card
                //     states left that hole. Reported as "after clicking the popup the island's time is
                //     gone". `lineBox` is the resting pill, so the line is where it has always been and
                //     the card hangs below it.
                //
                // `intoPill` and `frame.clock` are no longer consulted: they existed to choose between
                // the card's two-line clock style and the pill's one-line one, and the date line is now
                // part of this one line rather than a second row under it.
                //
                // NO SURFACE: the pill has already been filled with exactly the colour the strip paints
                // for itself.
                // THE ISLAND'S OWN SKY. Under the words, inside the resting pill, fading with the
                // content: rain that falls, a sunset that glows from the bottom, stars at night, a
                // slow sun in the afternoon. The kind comes from the real weather code and the
                // site's own sunrise and sunset (render/Ambient.h).
                {
                    SYSTEMTIME localNow = {};
                    GetLocalTime(&localNow);
                    const int nowMinutes = static_cast<int>(localNow.wHour) * 60 +
                                           static_cast<int>(localNow.wMinute);
                    const float phase = static_cast<float>(GetTickCount64() % 3600000ull) / 1000.0f;
                    const island::AmbientKind ambient = island::AmbientKindFor(
                        frame.statusCode, nowMinutes, frame.sunriseMinutes, frame.sunsetMinutes);
                    // THE WHOLE ISLAND SURFACE, from the same IslandSurfaceFromPillBox the glass and
                    // the rim are built from: the system button and the Start button on the left, the
                    // status pill in the middle and the tray button on the right are ONE shape. Drawing
                    // into RowAnchorBox stopped at the status pill and left the two button ends bare --
                    // "左侧系统按钮 开始按钮和右侧的托盘按钮没有覆盖上".
                    const D2D1_ROUNDED_RECT ambientShape = {
                        D2D1::RectF(shape.rect.left, shape.rect.top, shape.rect.right,
                                    shape.rect.bottom),
                        shape.radiusX, shape.radiusY};
                    island::DrawAmbient(d2dContext_, ambientShape, ambient, phase,
                                        frame.contentOpacity, island::CurrentTheme());
                }
                text_->DrawStatusLine(d2dContext_, lineBox, frame.contentOpacity, frame.statusSky,
                                      frame.statusTemperature, frame.statusCode,
                                      D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
            }


            // THE SAME LINE, BESIDE THE DECK, while the pill's own line is taken -- by the lyric, a
            // message, a call. This is the half of StatusPlacementFor that the pill cannot hold: the
            // date and the time have to be somewhere, and the island's one line is busy.
            //
            // THE ANCHOR BOX, not `box`. In the card states `box` is a 520x250 rectangle, and while
            // the notice strip is up the island is 200 DIP wider than the pill -- and the strip stands
            // beside the deck, which is laid out from this same box. RowAnchorBox and the deck's own
            // note below say which shape that is and why; the part that matters here is that the strip
            // and the button it stands beside cannot drift apart, because both are measured from one
            // rectangle (this is why `barIsland` is computed here and reused by the decks below).
            //
            // Drawn with the bar's own content rather than with the decks at the end of the function.
            // The decks are drawn last because a card would otherwise cover them (see DrawDecks), and
            // this band is not one a card reaches -- every card's top is moved to just under the bar --
            // so the strip belongs to the bar's own layer, where its fade follows the bar's strength.
            const RoundedBox barIsland =
                (frame.restingPill.rect.right > frame.restingPill.rect.left)
                    ? island::RowAnchorBox(box, frame.restingPill)
                    : box;
            // ... and NOT while a full-screen application is in front: then only the island is the bar
            // ("只要显示灵动岛就行 左右两侧的不用显示"), and this line stands in the band the island's left.
            if (frame.statusStripOpacity > 0.01f && !frame.fullscreenApp) {
                // Multiplied by the BAR's own strength, so a card opening beneath the strip cannot dim
                // it -- the same rule the deck is drawn under -- and on the island's own colour, so the
                // words stand on the bar rather than on the wallpaper.
                //
                // ... AND IT SLIDES OUT OF THE ISLAND AS IT FADES IN, rather than appearing in place:
                // the strip exists because the island's own line was taken, so it belongs to the island
                // and should come FROM it. The offset is derived from the fade -- no second animation and
                // no second piece of state -- and it is 6 DIP, which is small enough to read as motion
                // rather than as a thing that moved. Nothing hit-tests the strip, so a drawn position that
                // differs from its final one cannot disagree with a click.
                RoundedBox strip = island::StatusStripFor(barIsland);
                const float slide = (1.0f - frame.statusStripOpacity) * 6.0f;
                strip.rect.left += slide;
                strip.rect.right += slide;
                text_->DrawStatusLine(d2dContext_, strip,
                                      frame.statusStripOpacity * frame.contentOpacity,
                                      frame.statusSky, frame.statusTemperature, frame.statusCode,
                                      // The lens already IS the surface: its own film and rim cover
                                      // the strip. A second rectangular fill here showed up as a hard
                                      // box with vertical edges sitting on the glass.
                                      island::CurrentTheme().lens ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f)
                                                                  : frame.pill.fill);
            }

            // The card hangs from the control that was pressed, so its LEFT edge is moved to where that
            // control is before anything is composed into it. The height and width stay the animated
            // ones, so only the horizontal position is pinned -- the grow-downward motion is untouched.
            //
            // Done here rather than in the layout functions because every one of them (the music card,
            // the system panel, the notification) takes a box and lays itself out inside it; moving the
            // box is the single change that moves all three.
            RoundedBox card = box;
            if (frame.cardLeftDip > 0.0f) {
                const float cardWidth = card.rect.right - card.rect.left;
                card.rect.left = frame.cardLeftDip;
                card.rect.right = frame.cardLeftDip + cardWidth;
            }

            // Where a card's CONTENT is composed, when that differs from where the card itself is.
            //
            // Only the outer cards (the system panel and the notification) separate the two: they are
            // laid out at their final size while their card grows. Everything else -- the music card
            // included -- composes into `card` itself, so this starts as a copy of it.
            RoundedBox panelCard = card;

            // How tall the VISIBLE part of an outer card is, 0 when none of it is showing.
            //
            // Declared out here because the panel's draw has to skip itself at zero; see the assignment
            // in the outer-card block and OuterCardRevealDip in Motion.h.
            float outerCardReveal = island::kExpandedHeightDip;

            // The OUTER cards hang BELOW the bar rather than growing out of it.
            //
            // The motion box starts at the pill's top and grows downward, which is right for a shape that
            // IS the pill becoming larger -- but the system panel and the notification are flyouts beside
            // the bar, not the bar itself. Drawn at the animated position they covered the Start button,
            // the panel's own toggle and the application icons, and the user reported exactly that: the
            // panel should open UNDER the button, not over it, which is also what the shell's own
            // quick-settings flyout does.
            //
            // Only the TOP is moved. The animated height is kept, so the box still grows downward and the
            // card still slides out from behind the bar -- it simply starts below it.
            //
            // The music card is not moved: it is the pill's own expanded form, and moving it would detach
            // it from the island it belongs to.
            const bool outerCard = frame.outerCardUp;
            if (outerCard) {
                // THE PANEL IS LAID OUT AT ITS FINAL SIZE, AND THE CARD GROWS TO REVEAL IT.
                //
                // Two separate rectangles, and keeping them separate is the whole point:
                //
                //   `card`       the card's own rectangle, which GROWS -- this is what is painted and
                //                what the window region follows, so the panel still emerges from under
                //                the bar.
                //   `panelCard`  the FINAL rectangle, which never changes -- this is what the content
                //                is laid out in.
                //
                // The first version of this fix collapsed the two into one and pinned the card's height
                // to the final value on every frame. That removed the reflow, but it also removed the
                // animation: the card was full size from the first frame, and the reveal clamp below
                // held its top at the resting place, so the card simply appeared. Measured: ten captures
                // taken across the transition were byte-identical.
                //
                // SystemPanelFor positions everything from the box it is given -- the gear at
                // `box.bottom - pad`, the sliders stacked up from the gear, the tile grid filling what
                // is left -- so feeding it the GROWING card re-laid the whole panel out in a box that
                // was 36 DIP tall and then 50, and 80: the gear and sliders were computed into negative
                // space and every element moved at a different rate. That was the original jitter.
                const float cardTop = frame.restingPill.rect.bottom + kCardTopGapDip;
                const float animatedHeight = card.rect.bottom - card.rect.top;

                // The panel is uncovered in proportion to how far the box has grown PAST the pill, which
                // is what makes it fully hidden at the near end instead of leaving a 36 DIP band of tile
                // tops under the bar for the whole of the retraction's slow tail.
                outerCardReveal = island::OuterCardRevealDip(animatedHeight);

                // The card's own rectangle: full width, top at its resting place, and as tall as what is
                // REVEALED of it -- not as tall as the box.
                //
                // This is the rectangle the clip is taken from, so it is the one that decides how much of
                // the card is on screen. Using the box height instead left 36 DIP of card under the bar at
                // both ends of the movement (see OuterCardRevealDip).
                card.rect.top = cardTop;
                card.rect.bottom = cardTop + outerCardReveal;

                // The rectangle the content is composed in: fixed at the final size.
                //
                // Its TOP is placed at the card's resting top, so the panel's upper contents (the tile
                // rows) appear first as the card grows past them, which reads as the panel sliding out
                // from behind the bar.
                panelCard = card;
                panelCard.rect.top = cardTop;
                panelCard.rect.bottom = cardTop + island::kExpandedHeightDip;
                if (frame.cardLeftDip > 0.0f) {
                    panelCard.rect.left = frame.cardLeftDip;
                    panelCard.rect.right = frame.cardLeftDip + island::kExpandedWidthDip;
                }
            }

            // THE CARD'S INK IS DECIDED UNDER THE CARD.
            //
            // The system panel and the Start menu hang BELOW the bar, over a different part of the
            // desktop, and they were taking the island's ink -- black words over whatever dark wallpaper
            // was under the card. Sample the same captured frame the lens uses, at the card's own centre,
            // once per frame, and let CardInk cross over exactly as PillInk does. The fallback is the
            // island's own reading, which is the answer when the capture does not cover the card.
            {
                float cardLuminance = lens_.BackdropLuminance();
                if (capture_.Running()) {
                    ID3D11Texture2D* texture = capture_.LatestTexture();
                    POINT origin = {0, 0};
                    ClientToScreen(window_, &origin);
                    const RECT area = {
                        origin.x + static_cast<LONG>(std::lround(panelCard.rect.left * scale_)),
                        origin.y + static_cast<LONG>(std::lround(panelCard.rect.top * scale_)),
                        origin.x + static_cast<LONG>(std::lround(panelCard.rect.right * scale_)),
                        origin.y + static_cast<LONG>(std::lround(panelCard.rect.bottom * scale_))};
                    float sampled = -1.0f;
                    if (texture &&
                        lens_.SampleLuminance(texture, capture_.SourceRect(), area, &sampled) &&
                        sampled >= 0.0f) {
                        cardLuminance = sampled;
                    }
                }
                island::SetCardInkBackdrop(cardLuminance);
            }

            // ONE CARD AT A TIME, WHICHEVER OWNS IT -- and every one of the three is drawn the same way.
            //
            // The owner is a property of the frame (see NextCardOwner): who it is does not change while the
            // card is putting itself away, which is what makes the exit visible. It used to be
            // `state == Expanded` -- the TARGET -- so a retraction drew nothing at all and the card
            // disappeared in one frame.
            //
            // `outerCardReveal > 0.5f` as well as the ownership: at the near end of a transition there is
            // nothing of the card to show, and an empty clip rectangle is not a clip -- D2D fails to create
            // it, no layer is pushed, and the card would be composed at FULL size, which is the opposite of
            // hiding it. See OuterCardRevealDip.
            if (frame.cardOwner != CardOwner::None && outerCardReveal > 0.5f) {
                // The panel is composed at its final size and CLIPPED to the card's growing rectangle.
                //
                // The card cannot do this by being small, because the card's rectangle is not what
                // limits what is painted: the window REGION is, and the region is set from the card's
                // final rectangle (it has to be, or the panel's lower half would be unclickable). So the
                // panel was composed at full size and nothing clipped it -- the reveal was invisible and
                // the panel simply appeared.
                //
                // Measured: eight captures taken across a 12x-slowed transition showed the panel fully
                // present in every one, with only the desktop behind it changing.
                //
                // A rectangular layer is the clip. Pushed and popped around the one draw, so it cannot
                // affect anything else on the bar -- and the round corners still come from the panel's
                // own rounded fill, which the rectangle does not cut because the rectangle is the card's
                // full width and the corners are inside it.
                ID2D1RectangleGeometry* clipGeometry = nullptr;
                if (d2dFactory_) {
                    // The reveal's WIDTH is the panel's final width, not the animating box's.
                    //
                    // The box's width animates too (220 -> 520 on the same curve as the height), and a
                    // clip taken straight from it uncovered the panel outwards from its TOP-LEFT CORNER:
                    // the three tile columns appeared one after another and each was cut vertically as
                    // well, which reads as the panel being wiped in diagonally rather than sliding out
                    // from behind the bar. Taking the width from the card's own left edge -- where the
                    // content is composed -- leaves exactly one edge moving, the bottom one.
                    //
                    // Scoped to the clip rather than written onto `card` itself, so that the rectangle the
                    // CONTENT is composed in can stay at its final size while this one grows.
                    const float clipRight = (frame.cardLeftDip > 0.0f)
                                                ? frame.cardLeftDip + island::kExpandedWidthDip
                                                : card.rect.right;
                    // The BOTTOM edge is the reveal, not the box height: the box is 36 DIP taller than the
                    // panel can be revealed by, so using it left the band of tile tops described in
                    // OuterCardRevealDip.
                    const D2D1_RECT_F clipRect = D2D1::RectF(card.rect.left, card.rect.top,
                                                             clipRight,
                                                             card.rect.top + outerCardReveal);
                    d2dFactory_->CreateRectangleGeometry(clipRect, &clipGeometry);
                }
                if (clipGeometry) {
                    d2dContext_->PushLayer(
                        D2D1::LayerParameters1(D2D1::InfiniteRect(), clipGeometry,
                                               D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                        nullptr);
                }

                // Every card is composed at its FINAL size, in `panelCard`, and the clip above is what
                // shows only as much of it as has been revealed. Composing the music card in `card` --
                // which is what it used to do -- laid its cover, title and controls out again on every
                // frame of its own growth, which is the reflow 7.77 chased in the panel.
                if (frame.cardOwner == CardOwner::SystemPanel) {
                    // The panel has THREE levels, and which one is showing is the frame's business: the
                    // tiles, the 声音输出 page the output-device button opens, or the tray page the tray
                    // button at the other end of the bar opens. Same card, same place, same animation.
                    if (frame.trayPageOpen) {
                        text_->DrawTrayPage(d2dContext_, panelCard, frame.trayPageTitle,
                                            frame.trayIcons, frame.trayIconCount, frame.trayRevision,
                                            frame.cardOpacity, frame.trayHoveredIndex,
                                            frame.cardSurfaceMoving);
                    } else if (frame.soundPageOpen) {
                        text_->DrawSoundPage(d2dContext_, panelCard, frame.soundPage,
                                             frame.cardOpacity);
                    } else if (frame.startPageOpen) {
                        // The Start card, which is a LEVEL of this card rather than a card of its own: the
                        // same rectangle, the same animation, and the same dismiss rules.
                        text_->DrawStartPage(d2dContext_, panelCard, frame.startCard, frame.cardOpacity);
                    } else {
                        text_->DrawSystemPanel(d2dContext_, panelCard, frame.systemPanel,
                                               frame.cardOpacity);
                    }

                    // ... and the tray's own context menu OVER whatever level is showing: it is opened by a
                    // right click on a cell, and the page underneath it stays exactly where it was -- the
                    // menu is a thing in front, not a level of its own (see TrayMenuFor).
                    if (frame.trayMenuOpen) {
                        text_->DrawTrayMenu(d2dContext_, frame.trayMenuRows, frame.trayMenuRowCount,
                                            frame.trayMenuLabels, frame.trayMenuHovered,
                                            frame.cardOpacity);
                    }
                } else if (frame.cardOwner == CardOwner::Notification && frame.notification) {
                    text_->DrawNotification(d2dContext_, panelCard,
                                            frame.notification->app,
                                            frame.notification->title,
                                            frame.notification->body,
                                            frame.notificationIconPixels,
                                            frame.notificationIconWidth,
                                            frame.notificationIconHeight,
                                            frame.notificationIconRevision,
                                            frame.cardOpacity);
                } else if (frame.cardOwner == CardOwner::Music && frame.music &&
                           frame.music->present) {
                    // The media card, in one call: background wash, scrim, cover, text and controls, so the
                    // layers cannot be drawn out of order.
                    TextRenderer::MusicCardText text;
                    text.title = frame.music->title;
                    text.artist = frame.music->artist;
                    text.album = frame.music->album;
                    // The name a USER knows the player by, not its executable.
                    //
                    // `app` is the SMTC app id -- "QQMusic.exe" -- and the card printed that, while the
                    // resolved display name ("QQ音乐", the executable's ProductName) was already sitting in
                    // `appName`, filled by the same code and printed in the log. Measured with a real
                    // session: the card said `QQMusic.exe` while `icon: app='QQMusic.exe' name='QQ音乐'`
                    // was on the line above it. The fallback is `app` for the case where the resolution
                    // could not find any version information.
                    text.app = frame.music->appName[0] ? frame.music->appName : frame.music->app;
                    text.playing = frame.transportGlyph && frame.transportGlyph[0] == '>';
                    // The projected position, not the raw snapshot: the caller advances it between polls so
                    // the printed time matches the bar.
                    text.positionMs = frame.musicPositionMs > 0 ? frame.musicPositionMs
                                                                : frame.music->positionMs;
                    // The total comes from the CALLER when it has one: a session that publishes no
                    // timeline has none, and the lyric provider read the track's real length out of the
                    // catalogue it searched (see Lyrics::trackDurationMs). Zero means nobody knows, and
                    // the card prints `--:--`.
                    text.durationMs = frame.musicDurationMs > 0 ? frame.musicDurationMs
                                                                : frame.music->durationMs;
                    text.progressFraction = frame.progressFraction;
                    // The album's own brightness, for the card's black/white words. Recomputed only when
                    // the blurred background changes (once per track), not per frame.
                    if (frame.backgroundPixels && frame.backgroundWidth > 0 && frame.backgroundHeight > 0 &&
                        frame.backgroundRevision != musicBackgroundRevision_) {
                        musicBackgroundRevision_ = frame.backgroundRevision;
                        musicBackgroundLuma_ = AverageLuminance(
                            frame.backgroundPixels, frame.backgroundWidth, frame.backgroundHeight);
                    }
                    text.backgroundLuminance = musicBackgroundLuma_;
                    // The cover's accent, so the progress bar and the play button follow the album.
                    text.accent = frame.musicAccent;

                    text_->DrawMusicCard(d2dContext_, panelCard, text,
                                         CachedBitmap(frame.artworkPixels, frame.artworkWidth,
                                                      frame.artworkHeight,
                                                      frame.artworkRevision, BitmapSlot::Artwork),
                                         frame.artworkWidth, frame.artworkHeight,
                                         CachedBitmap(frame.backgroundPixels,
                                                      frame.backgroundWidth,
                                                      frame.backgroundHeight,
                                                      frame.backgroundRevision,
                                                      BitmapSlot::Background),
                                         CachedBitmap(frame.iconPixels, frame.iconWidth,
                                                      frame.iconHeight, frame.iconRevision,
                                                      BitmapSlot::AppIcon),
                                         frame.cardOpacity);
                }

                if (clipGeometry) {
                    d2dContext_->PopLayer();
                    clipGeometry->Release();
                }
            }

        // THE DECKS LAST, so nothing composed into the card can cover them.
        //
        // The card is the tallest thing on the bar and it hangs from a control on the left deck, which
        // puts its rectangle directly over the Start button, the panel's own toggle, the indicators and
        // the application icon row. Drawn before the card -- which is where these calls used to be --
        // opening the panel made all of them disappear, and they stayed gone until it was closed.
        //
        // Nothing about their positions changed and their hit tests kept working, so every layout
        // assertion passed: the buttons were there, clickable, and invisible. The Windows quick-settings
        // flyout opens below the taskbar and never over its buttons; drawing the decks last is what
        // makes that true here.
        //
        // Gated on the BAR's strength, not the card's. The deck belongs to the bar: a card opening
        // beneath it must not dim it. (It used to: one number served both, and the whole bar blinked on
        // every card transition -- see ContentStrengthsFor.)
        //
        // The anchor box, not `box` and not the resting pill.
        //
        // `box` alone is wrong: in the card state it is a 520x250 rectangle, and the icon row and the Start
        // button hang off its RIGHT edge and its vertical centre -- measured, that put them at x=1540,
        // visible, and y=116, inside the panel's own footprint. The decks belong to the bar, so they are
        // laid out from the bar's own rectangle.
        //
        // The resting pill ALONE is wrong too, and that is the bug this replaces: the notice strip is a wider
        // island, so a deck laid out from the pill was drawn UNDER it -- the Start button under the strip's
        // left end, and the two leftmost application icons under its right end. The window's region and hit
        // tests had already been moved to RowAnchorBox (so those icons were clickable where they were no
        // longer drawn), and a row that is drawn in one place and pressed in another is the one thing this
        // project has paid for most often.
        //
        // `barIsland`, computed once above, is exactly that box -- the status strip stands beside this deck
        // and is measured from the same rectangle, so the two are the same shape by construction.
        const RoundedBox& deckIsland = barIsland;
        // ... and not while a full-screen application is in front: the deck and the application icon row
        // are the "left and right sides" that must stay off screen with it.
        if (frame.contentOpacity > 0.01f && !frame.fullscreenApp) {
            DrawDecks(deckIsland, frame);
        }
    }
}

hr = d2dContext_->EndDraw();

// How long one frame's drawing took, in microseconds.
//
// This exists because a slow card was reported as "very laggy" and the cost turned
// out to be entirely in here: the card's album art and its blurred background were
// being rebuilt as GPU bitmaps on EVERY frame, which re-uploads about 150 KB of
// pixels per frame. Nothing about the picture changes between frames, so the fix is
// a cache -- but the measurement has to exist first, or "it feels laggy" is not
// something a change can be shown to improve.
{
    LARGE_INTEGER renderEnd = {};
    LARGE_INTEGER frequency = {};
    QueryPerformanceCounter(&renderEnd);
    QueryPerformanceFrequency(&frequency);
    if (frequency.QuadPart > 0) {
        lastFrameMicroseconds_ =
            static_cast<double>(renderEnd.QuadPart - renderStart.QuadPart) * 1000000.0 /
            static_cast<double>(frequency.QuadPart);
        frameCount_ += 1;
        frameMicrosecondsTotal_ += lastFrameMicroseconds_;
    }
}

if (hr == D2DERR_RECREATE_TARGET) {
    ReleaseTarget();
    // The cached bitmaps belong to the device that was just lost. Keeping them would
    // mean drawing a bitmap created by a device that no longer exists, which is at
    // best a stale image and at worst a device-removed loop that never recovers: the
    // cache key would match, so nothing would ever rebuild them.
    ReleaseBitmapCache();
ReleaseWindowIcons();
    return;
}

// A failure that is NOT "recreate the target" used to be presented as an empty frame, which looks like
// the bar vanished. Keep the last good frame on screen instead, and say so once.
if (FAILED(hr)) {
    static bool reportedEndDrawFailure = false;
    if (!reportedEndDrawFailure) {
        reportedEndDrawFailure = true;
        LogLine(L"  render: EndDraw failed (0x%08lX); the frame was skipped", 
                static_cast<unsigned long>(hr));
    }
    d2dContext_->SetTarget(nullptr);
    SafeRelease(&d2dTarget_);
    return;
}

d2dContext_->SetTarget(nullptr);
SafeRelease(&d2dTarget_);

// Present with NO sync interval -- interval 0, and that is now what the argument says.
//
// This line read Present(1, 0) with this same comment above it for as long as it has existed: the
// parameter and the sentence disagreed, and the effect was to wait twice for every frame -- once in the
// loop (on the compositor clock, or on a 4 ms timer) and once here, on vsync. Two waits where one is
// intended is at least a frame of latency, and on this machine it showed up as the ~100 ms tails in the
// loop's own interval report and as "slow frame" lines whose work was "none logged".
//
// The wait is now the swap chain's frame-latency waitable object, which the loop blocks on before
// drawing (see IslandWindow/main.cpp). Presenting without waiting again is what makes that the ONLY
// throttle, which is the property the throttle line in the log is there to demonstrate.
swapChain_->Present(0, 0);
framesPresented_ += 1;

if (compDevice_) {
    compDevice_->Commit();
}
}

bool Renderer::VideoMemory(unsigned long long* localBytes, unsigned long long* nonLocalBytes) {
    if (localBytes) *localBytes = 0;
    if (nonLocalBytes) *nonLocalBytes = 0;
    if (!dxgiAdapter3_) return false;

    const UINT node = 0;   // one GPU node: this bar never spans an adapter
    DXGI_QUERY_VIDEO_MEMORY_INFO local = {};
    if (FAILED(dxgiAdapter3_->QueryVideoMemoryInfo(node, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))) {
        return false;
    }

    // The non-local segment is optional: a driver may legitimately answer nothing for it, and a
    // failure there must not throw away the local figure, which is the one that matters.
    DXGI_QUERY_VIDEO_MEMORY_INFO nonLocal = {};
    const bool haveNonLocal =
        SUCCEEDED(dxgiAdapter3_->QueryVideoMemoryInfo(node, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,
                                                      &nonLocal));

    if (localBytes) *localBytes = static_cast<unsigned long long>(local.CurrentUsage);
    if (nonLocalBytes) *nonLocalBytes = haveNonLocal
                                            ? static_cast<unsigned long long>(nonLocal.CurrentUsage)
                                            : 0ull;
    return true;
}

void Renderer::CacheCounts(int* bitmaps, int* windowIcons) const {
    if (bitmaps) {
        int count = 0;
        for (int i = 0; i < static_cast<int>(BitmapSlot::Count); ++i) {
            if (bitmapCache_[i].bitmap) ++count;
        }
        *bitmaps = count;
    }
    if (windowIcons) *windowIcons = windowIconCount_;
}

ID2D1Bitmap1* Renderer::IconForWindow(HWND window) {
    if (!window || !d2dContext_) return nullptr;

    for (int i = 0; i < windowIconCount_; ++i) {
        if (windowIcons_[i].window == window) return windowIcons_[i].bitmap;
    }
    // ... and only now is a window that has CLOSED refused.
    //
    // The cache is asked first on purpose: an icon that is leaving the row is drawn for the length of its
    // exit, and by then its window is usually gone -- `IsWindow` refusing before the lookup (as this did)
    // meant a departing icon was drawn as an empty slot, so the row's departures had no picture to fade.
    if (!IsWindow(window)) return nullptr;
    if (windowIconCount_ >= kMaxWindowIcons) return nullptr;   // full; see the header for why

    WindowIcon& slot = windowIcons_[windowIconCount_];
    slot.window = window;
    slot.bitmap = nullptr;
    ++windowIconCount_;

    // The window's OWN icon, in preference to its executable's.
    //
    // WM_GETICON with ICON_BIG is what the window itself set, so it is both the icon the user
    // associates with this window and one that needs no permission over another process. The
    // process-executable route needs OpenProcess and QueryFullProcessImageName to succeed, and both
    // are refused for a protected process -- which left those windows showing a generic folder.
    //
    // The icon arrives through the OUTPUT PARAMETER, not the return value.
    //
    // `SendMessageTimeoutW` returns whether the call succeeded; the message's own result is
    // written to the LRESULT* it is given. The first version read the return value and passed
    // nullptr for the output, so every icon came back as "no icon" while the call reported success
    // -- and the code fell through to the executable's icon, which for a console host IS the
    // generic folder. That is exactly what was on screen.
    const auto windowIcon = [window](WPARAM which) -> HICON {
        // DWORD_PTR, because that is what SendMessageTimeoutW writes its result into -- it is
        // declared with PDWORD_PTR and not with an LRESULT*, and passing the wrong pointer type
        // does not compile.
        DWORD_PTR result = 0;
        const LRESULT sent = SendMessageTimeoutW(window, WM_GETICON, which, 0, SMTO_ABORTIFHUNG,
                                                 100, &result);
        if (sent == 0 || result == 0) return nullptr;
        return reinterpret_cast<HICON>(result);
    };

    HICON icon = windowIcon(ICON_BIG);
    if (!icon) {
        icon = reinterpret_cast<HICON>(GetClassLongPtrW(window, GCLP_HICON));
    }
    if (!icon) {
        // The small icon is often the only one a window sets.
        icon = windowIcon(ICON_SMALL);
    }

    if (!icon) {
        // Nothing on the window itself: fall back to the executable's icon, which is where most
        // applications' icons actually live.
        wchar_t path[MAX_PATH] = {};
        DWORD pathLength = MAX_PATH;
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        HANDLE process = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
        if (process) {
            const BOOL havePath = QueryFullProcessImageNameW(process, 0, path, &pathLength);
            CloseHandle(process);
            if (havePath && path[0] != L'\0') {
                uint8_t png[kMusicArtworkCapacity] = {};
                const size_t pngSize = FetchAppIconForTest(path, png, sizeof(png));
                if (pngSize == 0) return nullptr;
                DecodedImage decoded;
                if (!DecodeImage(png, pngSize, &decoded) || !decoded.pixels) return nullptr;

                const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
                    D2D1_BITMAP_OPTIONS_NONE,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                      D2D1_ALPHA_MODE_PREMULTIPLIED),
                    96.0f, 96.0f);
                const UINT32 stride = static_cast<UINT32>(decoded.width) * 4;
                d2dContext_->CreateBitmap(
                    D2D1::SizeU(static_cast<UINT32>(decoded.width),
                                static_cast<UINT32>(decoded.height)),
                    decoded.pixels, stride, &props, &slot.bitmap);
            }
        }
        return slot.bitmap;
    }

    // An HICON has to be turned into pixels. The shared rasterizer draws it into a top-down BGRA buffer
    // (platform/IconRaster.h) and this uploads what it made -- one place that knows how to get from an icon
    // to a bitmap, which is what this comment claimed while being the second copy of it.
    std::vector<uint8_t> pixels;
    if (RasterizeIcon32(icon, pixels)) {
        const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        d2dContext_->CreateBitmap(D2D1::SizeU(kIconRasterSide, kIconRasterSide), pixels.data(),
                                  kIconRasterSide * 4, &props, &slot.bitmap);
    }

    return slot.bitmap;
}

void Renderer::ReleaseWindowIcons() {
    for (int i = 0; i < windowIconCount_; ++i) {
        SafeRelease(&windowIcons_[i].bitmap);
    }
    windowIconCount_ = 0;
}

ID2D1Bitmap1* Renderer::CachedBitmap(const uint8_t* pixels, int width, int height,
                                     unsigned long long revision, BitmapSlot slot) {
    if (!d2dContext_ || !pixels || width <= 0 || height <= 0) return nullptr;

    BitmapCache& cache = bitmapCache_[static_cast<int>(slot)];

    // Rebuild only when the image actually changes.
    //
    // This is the whole point: creating a D2D bitmap COPIES the pixels to the GPU, so
    // rebuilding these per frame re-uploaded about 150 KB for a picture that had not
    // changed -- which is what made hovering the card feel laggy. The revision is the
    // provider's own change counter, so the cache cannot go stale silently.
    //
    // ... AND THE REVISION IS NOT ENOUGH ON ITS OWN, which cost this project a whole round: the media
    // session's artwork revision used to RESET to 1 whenever a track published no cover, so a cover that
    // arrived late for the next track carried the same number as the previous track's -- and this returned
    // the previous track's album art while the log said the new one had been decoded ("歌曲切换了 但是封面图
    // 没有换"). The provider now counts up and never repeats a number, and the POINTER is compared as well,
    // because a cache that assumes its key is unique is a cache that draws the wrong picture when it is not.
    if (cache.bitmap && cache.revision == revision && cache.width == width &&
        cache.height == height && cache.pixels == pixels) {
        return cache.bitmap;
    }

    SafeRelease(&cache.bitmap);
    cache.pixels = pixels;

    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);

    const UINT32 stride = static_cast<UINT32>(width) * 4;
    if (FAILED(d2dContext_->CreateBitmap(
            D2D1::SizeU(static_cast<UINT32>(width), static_cast<UINT32>(height)), pixels,
            stride, &properties, &cache.bitmap)) ||
        !cache.bitmap) {
        cache.bitmap = nullptr;
        return nullptr;
    }

    cache.revision = revision;
    cache.width = width;
    cache.height = height;
    return cache.bitmap;
}

void Renderer::ReleaseBitmapCache() {
    for (BitmapCache& cache : bitmapCache_) {
        SafeRelease(&cache.bitmap);
        cache.revision = 0;
        cache.width = 0;
        cache.height = 0;
    }
}

bool Renderer::WaitForCompositorClock(DWORD timeoutMs) {
    if (!waitForClock_) return false;

    // The API returns the count of signaled objects, or WAIT_TIMEOUT. Passing no
    // handles makes it a pure "wait for the next composition tick".
    const DWORD result = waitForClock_(0, nullptr, timeoutMs);
    return result != WAIT_TIMEOUT && result != WAIT_FAILED;
}

bool Renderer::ReadBackFrame(float insideXDip, float insideYDip,
                             float outsideXDip, float outsideYDip,
                             BYTE* insideAlpha, BYTE* outsideAlpha,
                             BYTE* insideRgb) {
    return ReadBackFrameVerbose(insideXDip, insideYDip, outsideXDip, outsideYDip,
                                insideAlpha, outsideAlpha, insideRgb, nullptr, 0);
}

bool Renderer::ReadBackFrameVerbose(float insideXDip, float insideYDip,
                                    float outsideXDip, float outsideYDip,
                                    BYTE* insideAlpha, BYTE* outsideAlpha, BYTE* insideRgb,
                                    wchar_t* step, size_t stepCapacity) {
    const auto note = [&](const wchar_t* text, HRESULT hr) {
        if (step && stepCapacity) {
            std::swprintf(step, stepCapacity, L"%s (0x%08lX)", text,
                          static_cast<unsigned long>(hr));
        }
    };

    if (!d3dDevice_ || !d3dContext_ || !swapChain_) {
        note(L"renderer not initialized", E_FAIL);
        return false;
    }

    IDXGISurface* surface = nullptr;
    HRESULT hr = swapChain_->GetBuffer(0, __uuidof(IDXGISurface),
                                       reinterpret_cast<void**>(&surface));
    if (FAILED(hr) || !surface) {
        note(L"swapChain GetBuffer(0)", hr);
        return false;
    }

    // The staging copy needs the D3D resource behind the swap-chain buffer.
    ID3D11Resource* resource = nullptr;
    hr = surface->QueryInterface(__uuidof(ID3D11Resource), reinterpret_cast<void**>(&resource));
    SafeRelease(&surface);
    if (FAILED(hr) || !resource) {
        note(L"IDXGISurface -> ID3D11Resource", hr);
        return false;
    }

    // A staging texture is required because the back buffer is not CPU-readable.
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width            = width_;
    desc.Height           = height_;
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;

    ID3D11Texture2D* staging = nullptr;
    hr = d3dDevice_->CreateTexture2D(&desc, nullptr, &staging);
    if (FAILED(hr) || !staging) {
        note(L"CreateTexture2D(staging)", hr);
        SafeRelease(&resource);
        return false;
    }

    // Copy on the immediate context, then flush so Map sees the result: a staging
    // read issued straight after a copy without a flush can observe a half-filled
    // texture in a multithreaded driver.
    d3dContext_->CopyResource(staging, resource);
    d3dContext_->Flush();

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = d3dContext_->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        note(L"Map(staging)", hr);
        SafeRelease(&staging);
        SafeRelease(&resource);
        return false;
    }

    const auto sample = [&](float xDip, float yDip, BYTE* a, BYTE* rgb) -> bool {
        const float sx = xDip * scale_;
        const float sy = yDip * scale_;
        const LONG x = static_cast<LONG>(sx);
        const LONG y = static_cast<LONG>(sy);
        if (x < 0 || y < 0 || x >= static_cast<LONG>(width_) || y >= static_cast<LONG>(height_)) {
            return false;
        }
        const BYTE* row = static_cast<const BYTE*>(mapped.pData) +
                          static_cast<size_t>(y) * mapped.RowPitch;
        const BYTE* px = row + static_cast<size_t>(x) * 4;   // BGRA
        if (a) *a = px[3];
        if (rgb) {
            rgb[0] = px[2];
            rgb[1] = px[1];
            rgb[2] = px[0];
        }
        return true;
    };

    const bool insideOk = sample(insideXDip, insideYDip, insideAlpha, insideRgb);
    const bool outsideOk = sample(outsideXDip, outsideYDip, outsideAlpha, nullptr);

    d3dContext_->Unmap(staging, 0);
    SafeRelease(&staging);
    SafeRelease(&resource);

    if (!insideOk || !outsideOk) {
        if (step && stepCapacity) {
            std::swprintf(step, stepCapacity,
                          L"sample out of bounds: surface %ux%u, scale %.2f, "
                          L"inside=(%.1f,%.1f) outside=(%.1f,%.1f)",
                          width_, height_, scale_, insideXDip, insideYDip,
                          outsideXDip, outsideYDip);
        }
        return false;
    }

    if (step && stepCapacity) {
        std::swprintf(step, stepCapacity, L"ok (surface %ux%u, scale %.2f)",
                      width_, height_, scale_);
    }
    return true;
}

bool Renderer::AnimateOpacity(float from, float to, double seconds) {
    if (!compDevice_ || !effectGroup_ || seconds <= 0.0) return false;

    // The animation is created fresh each time: reusing one object and re-adding
    // curves to it would append to the existing timeline instead of replacing it.
    SafeRelease(&opacityAnimation_);

    if (FAILED(compDevice_->CreateAnimation(&opacityAnimation_)) || !opacityAnimation_) {
        return false;
    }

    // A linear ramp. An eased opacity is barely distinguishable at these
    // durations, and the curve shape is not worth the extra complexity here --
    // the geometry, which is what the eye actually tracks, is eased on the CPU.
    const float slope = static_cast<float>((to - from) / seconds);
    opacityAnimation_->AddCubic(0.0, from, slope, 0.0f, 0.0f);
    // End takes both the offset and the value it lands on; passing only the
    // offset does not compile.
    opacityAnimation_->End(seconds, to);

    if (FAILED(effectGroup_->SetOpacity(opacityAnimation_))) {
        SafeRelease(&opacityAnimation_);
        return false;
    }

    opacityAnimated_ = true;

    if (FAILED(compDevice_->Commit())) {
        // The value never reached the compositor, so do not leave the renderer
        // believing the group owns opacity: it would then stop updating it.
        opacityAnimated_ = false;
        SafeRelease(&opacityAnimation_);
        return false;
    }
    return true;
}

bool Renderer::RegionLuminance(float leftDip, float topDip, float rightDip, float bottomDip,
                               float* outLuminance) {
    if (!outLuminance || !d3dDevice_ || !d3dContext_ || !swapChain_) return false;

    const LONG left = static_cast<LONG>(leftDip * scale_);
    const LONG top = static_cast<LONG>(topDip * scale_);
    const LONG right = static_cast<LONG>(rightDip * scale_);
    const LONG bottom = static_cast<LONG>(bottomDip * scale_);

    if (left < 0 || top < 0 || right > static_cast<LONG>(width_) ||
        bottom > static_cast<LONG>(height_) || right <= left || bottom <= top) {
        return false;
    }

    // Same staging copy as ReadBackFrame, for the same reason: the back buffer is
    // not CPU-readable.
    IDXGISurface* surface = nullptr;
    if (FAILED(swapChain_->GetBuffer(0, __uuidof(IDXGISurface),
                                     reinterpret_cast<void**>(&surface))) ||
        !surface) {
        return false;
    }

    ID3D11Resource* resource = nullptr;
    const HRESULT hr = surface->QueryInterface(__uuidof(ID3D11Resource),
                                               reinterpret_cast<void**>(&resource));
    SafeRelease(&surface);
    if (FAILED(hr) || !resource) return false;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width            = width_;
    desc.Height           = height_;
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;

    ID3D11Texture2D* staging = nullptr;
    if (FAILED(d3dDevice_->CreateTexture2D(&desc, nullptr, &staging)) || !staging) {
        SafeRelease(&resource);
        return false;
    }

    d3dContext_->CopyResource(staging, resource);
    d3dContext_->Flush();

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(d3dContext_->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        SafeRelease(&staging);
        SafeRelease(&resource);
        return false;
    }

    // Mean of the per-pixel luminance, weighted by alpha.
    //
    // The weight matters: the card is translucent, and outside it the surface is
    // fully transparent. Averaging colour without regard to alpha would let whatever
    // is behind the bar sway the result.
    double sum = 0.0;
    double weight = 0.0;
    for (LONG y = top; y < bottom; ++y) {
        const BYTE* row = static_cast<const BYTE*>(mapped.pData) +
                          static_cast<size_t>(y) * mapped.RowPitch;
        for (LONG x = left; x < right; ++x) {
            const BYTE* px = row + static_cast<size_t>(x) * 4;   // BGRA
            const double alpha = px[3] / 255.0;
            const double luma = 0.114 * px[0] + 0.587 * px[1] + 0.299 * px[2];
            sum += luma * alpha;
            weight += alpha;
        }
    }

    d3dContext_->Unmap(staging, 0);
    SafeRelease(&staging);
    SafeRelease(&resource);

    *outLuminance = (weight > 0.0) ? static_cast<float>(sum / weight) : 0.0f;
    return true;
}

// Renders an ASCII map of alpha over a region. This exists to answer "is anything
// drawn here" with a picture instead of a number: a mean luminance that matches the
// fill says nothing about whether glyphs are present, and a map makes an empty area
// and a sparsely-texted one immediately distinguishable.
void Renderer::DescribeRegionAlpha(float leftDip, float topDip, float rightDip,
                                   float bottomDip, int columns, int rows,
                                   wchar_t* out, size_t capacity) {
    if (!out || capacity == 0) return;
    out[0] = L'\0';
    if (columns <= 0 || rows <= 0) return;

    IDXGISurface* surface = nullptr;
    if (FAILED(swapChain_->GetBuffer(0, __uuidof(IDXGISurface),
                                     reinterpret_cast<void**>(&surface))) ||
        !surface) {
        return;
    }
    ID3D11Resource* resource = nullptr;
    const HRESULT hr = surface->QueryInterface(__uuidof(ID3D11Resource),
                                               reinterpret_cast<void**>(&resource));
    SafeRelease(&surface);
    if (FAILED(hr) || !resource) return;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width_;
    desc.Height = height_;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ID3D11Texture2D* staging = nullptr;
    if (FAILED(d3dDevice_->CreateTexture2D(&desc, nullptr, &staging)) || !staging) {
        SafeRelease(&resource);
        return;
    }

    d3dContext_->CopyResource(staging, resource);
    d3dContext_->Flush();

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(d3dContext_->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        SafeRelease(&staging);
        SafeRelease(&resource);
        return;
    }

    const int left = static_cast<int>(leftDip * scale_);
    const int top = static_cast<int>(topDip * scale_);
    const int right = static_cast<int>(rightDip * scale_);
    const int bottom = static_cast<int>(bottomDip * scale_);

    size_t used = 0;
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < columns; ++col) {
            // Sample the middle of each cell rather than summing: with only 20x8
            // cells over 260x60 DIP, averaging would wash out thin glyph strokes,
            // which is exactly what needs to show up.
            const int x = left + (right - left) * col / columns + (right - left) / (columns * 2);
            const int y = top + (bottom - top) * row / rows + (bottom - top) / (rows * 2);
            wchar_t glyph = L'.';
            if (x >= 0 && y >= 0 && x < static_cast<int>(width_) &&
                y < static_cast<int>(height_)) {
                const BYTE* rowPtr = static_cast<const BYTE*>(mapped.pData) +
                                     static_cast<size_t>(y) * mapped.RowPitch;
                const BYTE* px = rowPtr + static_cast<size_t>(x) * 4;
                // Alpha 235 is the card fill; anything brighter means a glyph.
                const int alpha = px[3];
                glyph = (alpha < 10) ? L' '
                        : (alpha > 245) ? L'#'
                        : (alpha > 240) ? L'+'
                        : (alpha > 236) ? L'-'
                                        : L'.';
            }
            if (used + 2 < capacity) { out[used++] = glyph; }
        }
        if (used + 2 < capacity) { out[used++] = L'\n'; }
    }
    out[used] = L'\0';

    d3dContext_->Unmap(staging, 0);
    SafeRelease(&staging);
    SafeRelease(&resource);
}

void Renderer::Shutdown() {
    ReleaseTarget();

    // The cached bitmaps belong to the device context, so they go before it does.
    ReleaseBitmapCache();
    ReleaseWindowIcons();

    SafeRelease(&opacityAnimation_);
    opacityAnimated_ = false;

    SafeRelease(&compTarget_);
    SafeRelease(&compVisual_);
    capture_.Stop();
    window_ = nullptr;
    lensWindow_ = nullptr;
    lens_.Shutdown();
    lensActive_ = false;
    SafeRelease(&effectGroup_);
    SafeRelease(&compDevice_);
    SafeRelease(&d2dContext_);
    SafeRelease(&d2dDevice_);
    SafeRelease(&d2dFactory_);
    if (frameLatencyWaitable_) {
        // The swap chain owns the object's lifetime; this only releases our handle to it, and it goes
        // before the swap chain does.
        CloseHandle(frameLatencyWaitable_);
        frameLatencyWaitable_ = nullptr;
    }
    SafeRelease(&swapChain2_);
    SafeRelease(&dxgiAdapter3_);
    SafeRelease(&swapChain_);
    SafeRelease(&d3dContext_);
    SafeRelease(&d3dDevice_);

    waitForClock_ = nullptr;
    width_ = height_ = 0;
}

} // namespace island
