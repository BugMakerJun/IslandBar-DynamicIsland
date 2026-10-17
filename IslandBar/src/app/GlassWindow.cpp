// =====================================================================
//  GlassWindow.cpp -- the bare material window. See the header for what was measured first.
//
//  There is no renderer here on purpose. The material is DWM's, drawn behind the window's content; an
//  empty per-pixel-alpha window shows it and costs one HWND. That is the whole implementation.
// =====================================================================

#include "app/GlassWindow.h"

#include "core/Log.h"

#include <dwmapi.h>

#include <cmath>

#pragma comment(lib, "dwmapi.lib")

namespace island {
namespace {

// The composition attribute setter is not declared in any SDK header; the structures are the ones every
// acrylic sample uses.
enum AccentState {
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
};

struct AccentPolicy {
    AccentState state;
    int flags;
    unsigned int colour;   // ABGR. The alpha is how much tint sits on the blurred backdrop.
    int animationId;
};

struct WindowCompositionAttributeData {
    int attribute;
    void* data;
    size_t size;
};

constexpr int kAccentPolicyAttribute = 19;
constexpr int kCornerPreferenceAttribute = 33;   // DWMWA_WINDOW_CORNER_PREFERENCE
constexpr int kCornerRound = 2;                  // DWMWCP_ROUND
constexpr int kBorderColorAttribute = 34;        // DWMWA_BORDER_COLOR
constexpr unsigned int kNoBorder = 0xFFFFFFFEu;  // DWMWA_COLOR_NONE

// 12.5% black. The theme's own scrim (glass: 0.66-0.82) is drawn by the BAR on top of this, so the
// material's tint stays low: any more and the backdrop's colours are washed out before the scrim is even
// applied. The probe's 25% WHITE tint is a measuring colour, not this one.
constexpr unsigned int kMaterialTint = 0x20000000;

using SetWindowCompositionAttributeFn = BOOL(WINAPI*)(HWND, WindowCompositionAttributeData*);

LRESULT CALLBACK GlassProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_NCHITTEST:
        // The bar above owns every click on the island; this window exists to be looked through.
        return HTTRANSPARENT;
    case WM_ERASEBKGND:
        // Nothing is painted here. Letting the default erase run would paint the frame's own colour and
        // hide the material -- the exact mistake the first probe made (HANDOFF 8.165.1).
        return 1;
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

bool ApplyMaterial(HWND hwnd) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    auto setAttribute = reinterpret_cast<SetWindowCompositionAttributeFn>(
        user32 ? reinterpret_cast<void*>(GetProcAddress(user32, "SetWindowCompositionAttribute"))
               : nullptr);
    if (!setAttribute) {
        LogAndPrint("  glass: SetWindowCompositionAttribute is not exported; the island stays painted");
        return false;
    }

    AccentPolicy policy = {};
    policy.state = ACCENT_ENABLE_ACRYLICBLURBEHIND;
    policy.colour = kMaterialTint;
    WindowCompositionAttributeData data = {kAccentPolicyAttribute, &policy, sizeof(policy)};
    if (!setAttribute(hwnd, &data)) {
        LogAndPrint("  glass: acrylic was refused (error %lu); the island stays painted", GetLastError());
        return false;
    }
    return true;
}

}  // namespace

GlassWindow::~GlassWindow() {
    Destroy();
}

bool GlassWindow::Create(HWND mainWindow) {
    if (hwnd_) return true;
    main_ = mainWindow;

    WNDCLASSW wc = {};
    wc.lpfnWndProc = GlassProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    wc.hbrBackground = nullptr;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        LogAndPrint("  glass: RegisterClass failed (error %lu)", GetLastError());
        return false;
    }

    // One pixel at a time, hidden until there is an island to sit behind.
    const DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                          WS_EX_NOREDIRECTIONBITMAP;
    hwnd_ = CreateWindowExW(exStyle, kClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                            GetModuleHandleW(nullptr), nullptr);
    if (!hwnd_) {
        LogAndPrint("  glass: CreateWindowExW failed (error %lu)", GetLastError());
        return false;
    }

    // The material is the point of this window; if it fails the bar still runs with painted glass, and the
    // reason is in the log rather than in a mystery.
    if (!ApplyMaterial(hwnd_)) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return true;
    }

    // The same rounded-rectangle clip the probe measured (a fixed ~8 px, not the island's 18 DIP).
    const HRESULT corner =
        DwmSetWindowAttribute(hwnd_, kCornerPreferenceAttribute, &kCornerRound, sizeof(kCornerRound));
    if (FAILED(corner)) {
        LogAndPrint("  glass: DWMWA_WINDOW_CORNER_PREFERENCE refused (0x%08lX); the material will be square",
                    static_cast<unsigned long>(corner));
    }

    // NO WINDOW BORDER. DWM draws a 1 px frame around a top-level popup, and behind the island it pokes
    // out along the bottom edge as a straight line -- which reads as a box under the glass rather than as
    // part of it. DWMWA_COLOR_NONE removes the frame without touching the acrylic.
    DwmSetWindowAttribute(hwnd_, kBorderColorAttribute, &kNoBorder, sizeof(kNoBorder));
    return true;
}

void GlassWindow::Destroy() {
    if (hwnd_ && IsWindow(hwnd_)) {
        DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;
    main_ = nullptr;
    shown_ = false;
    rect_ = {};
}

void GlassWindow::ShowAt(const RECT& rectPixels) {
    if (!hwnd_) return;

    const LONG width = rectPixels.right - rectPixels.left;
    const LONG height = rectPixels.bottom - rectPixels.top;
    if (width < 1 || height < 1) {
        Hide();
        return;
    }
    if (shown_ && EqualRect(&rect_, &rectPixels)) return;

    LARGE_INTEGER frequency = {};
    LARGE_INTEGER before = {};
    LARGE_INTEGER after = {};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&before);

    // Inserted directly BELOW the bar (the product's z-order, measured in HANDOFF 8.167.4): an insert-after
    // a topmost window both keeps this one topmost and puts it under that window.
    const HWND insertAfter = main_ ? main_ : HWND_TOPMOST;
    const UINT flags = SWP_NOACTIVATE | (shown_ ? 0u : SWP_SHOWWINDOW);
    const BOOL ok = SetWindowPos(hwnd_, insertAfter, rectPixels.left, rectPixels.top, width, height, flags);

    QueryPerformanceCounter(&after);
    const double ms = frequency.QuadPart
                          ? 1000.0 * static_cast<double>(after.QuadPart - before.QuadPart) /
                                static_cast<double>(frequency.QuadPart)
                          : 0.0;
    if (ms > maxPlacementMs_) maxPlacementMs_ = ms;

    ++placements_;
    if (!ok) {
        ++failures_;
        if (failures_ == 1) {
            LogAndPrint("  glass: SetWindowPos failed (error %lu)", GetLastError());
        }
        return;
    }
    rect_ = rectPixels;
    shown_ = true;
}

void GlassWindow::Hide() {
    if (!hwnd_ || !shown_) return;
    ShowWindow(hwnd_, SW_HIDE);
    shown_ = false;
    rect_ = {};
}

}  // namespace island
