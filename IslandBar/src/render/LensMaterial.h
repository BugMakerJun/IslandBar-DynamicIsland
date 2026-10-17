// =====================================================================
//  LensMaterial.h
//  The refracted backdrop: the captured desktop, bent at the island's rim.
//
//  WHAT IT IS. A D2D effect chain -- DisplacementMap with a map built for a stadium, then a small Gaussian
//  blur -- fed by a Windows.Graphics.Capture texture. The map samples OUTWARD at the rim, so the edge
//  compresses what is around the island into a thick-glass band, which is the one part of Apple's material
//  that cannot be drawn from colour alone (HANDOFF 8.170/8.176).
//
//  WHERE IT SITS. In render/, because it is drawing; the texture comes from platform/BackdropCapture.
//  Everything is clipped to the island's own rounded rectangle, so nothing of it can reach the desktop.
// =====================================================================
#pragma once

#include <windows.h>

#include <d2d1_3.h>
#include <d3d11.h>
#include <wrl/client.h>

namespace island {

class LensMaterial {
public:
    bool Initialize(ID2D1DeviceContext* context);
    void Shutdown();
    bool Ready() const { return ready_; }

    // `texture` is the monitor's capture; `monitor` its screen rect in pixels; `shape` the island in
    // window-local DIPs; `scale` the window's DPI scale. Returns false when nothing was drawn.
    bool Draw(ID2D1DeviceContext* context, ID3D11Texture2D* texture, const RECT& monitor,
              const D2D1_ROUNDED_RECT& shape, float scale, float opacity);

private:
    struct Cache;
    Cache* cache_ = nullptr;
    bool ready_ = false;
};

}  // namespace island
