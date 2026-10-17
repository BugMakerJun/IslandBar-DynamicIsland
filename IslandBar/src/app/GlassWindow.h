// =====================================================================
//  GlassWindow.h
//  The window BEHIND the island that carries the system's material and nothing else.
//
//  WHY IT EXISTS (HANDOFF 8.163-8.168). Apple's "liquid glass" is the desktop seen through the island.
//  This project's one window is a full-width strip, and the only material Windows actually blurs --
//  ACCENT_ENABLE_ACRYLICBLURBEHIND -- blurs the WHOLE window rectangle, so on that window it would smear
//  a band across the top of the screen. A window whose rectangle IS the island has no band to leak into.
//
//  WHAT WAS MEASURED BEFORE THIS CLASS WAS WRITTEN (tools/glassprobe --dcomp, scripts/probe-glass-dcomp.ps1):
//
//    * that window kind really blurs: edge energy of a 4 px stripe pattern 57.75 -> 1.14;
//    * DWM clips the material to a rounded rectangle, but its radius is a fixed ~8 px, not the island's
//      18 DIP -- the four corner crescents stay sharp under the bar's own scrim, which is the accepted
//      trade of option A in that round;
//    * the window needs NO renderer at all: no D3D11 device, no swapchain, no content. A bare
//      WS_EX_NOREDIRECTIONBITMAP window with the accent policy shows the material (`--bare`, same 1.14);
//    * one SetWindowPos per frame to follow a 220 -> 720 growth cost 0.977 ms on average, 1.451 ms at
//      worst, with no failures and no hole in ten sampled frames.
//
//  INPUT: the window never takes a click. WM_NCHITTEST answers HTTRANSPARENT, and it is inserted directly
//  BELOW the bar's window, so the bar (which owns the pill's input region) stays the window that is hit.
// =====================================================================
#pragma once

#include <windows.h>

namespace island {

class GlassWindow {
public:
    static constexpr const wchar_t* kClassName = L"IslandBar.GlassWindow";

    GlassWindow() = default;
    ~GlassWindow();

    GlassWindow(const GlassWindow&) = delete;
    GlassWindow& operator=(const GlassWindow&) = delete;

    // Registers the class, creates the window hidden, and applies the material.
    // `mainWindow` is the bar: the glass window is always placed directly below it.
    // Returns false only when the window cannot exist at all; a material that could not be applied still
    // returns true and logs why, so the bar keeps running (the island simply stays painted).
    bool Create(HWND mainWindow);
    void Destroy();

    // Puts the window exactly over `rectPixels` (screen pixels) and shows it. A call with the same
    // rectangle as last time does nothing, so an idle bar costs nothing.
    void ShowAt(const RECT& rectPixels);
    void Hide();

    bool IsShown() const { return shown_; }
    HWND Handle() const { return hwnd_; }

    // The numbers the acceptance line is made of (HANDOFF 8.168.3).
    unsigned long long Placements() const { return placements_; }
    unsigned long long Failures() const { return failures_; }
    double MaxPlacementMs() const { return maxPlacementMs_; }

private:
    HWND hwnd_ = nullptr;
    HWND main_ = nullptr;
    RECT rect_ = {};
    bool shown_ = false;
    unsigned long long placements_ = 0;
    unsigned long long failures_ = 0;
    double maxPlacementMs_ = 0.0;
};

}  // namespace island
