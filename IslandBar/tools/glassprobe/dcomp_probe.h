// =====================================================================
//  dcomp_probe.h -- the pill-sized window of the SAME KIND the bar itself owns
//
//  Why this exists: the first version of glassprobe used an ordinary opaque window and measured that
//  acrylic is drawn BEHIND the window's content, where an opaque window hides it completely. The bar's
//  own window is the per-pixel-alpha kind (WS_EX_NOREDIRECTIONBITMAP + DirectComposition), and 8.86
//  measured acrylic blurring THERE. So the remaining question has to be asked on that kind of window.
//
//  ASCII-only on purpose. Do not add non-ASCII text to this file.
// =====================================================================

#pragma once

struct DCompOptions {
    int x = 1180;
    int y = 140;
    int width = 220;
    int height = 36;
    int seconds = 20;
    bool plain = false;        // no material: the control
    bool region = false;       // rounded window REGION instead of the DWM corner preference
    unsigned int tint = 0x40FFFFFF;   // ABGR for the accent policy
    int scrimAlpha = 24;       // 0..255, the faint fill drawn ON TOP of the material
};

int RunDCompProbe(const DCompOptions& options);
