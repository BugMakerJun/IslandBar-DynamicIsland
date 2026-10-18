// =====================================================================
//  LensMaterial.h -- the captured desktop, bent at the island's rim.
//
//  Its own D2D context renders the effect into an offscreen the size of the island; the renderer fills the
//  island's shape with that bitmap. (Drawing the chain into the frame needs a nested BeginDraw, which D2D
//  forbids, or a transformed layer, which emptied the whole frame -- HANDOFF 8.176/8.177.)
// =====================================================================
#pragma once

#include <windows.h>

#include <d2d1_3.h>
#include <d3d11.h>
#include <wrl/client.h>

namespace island {

class LensMaterial {
public:
    bool Initialize(ID2D1Device* device);
    void Shutdown();
    bool Ready() const { return ready_; }

    // `source` is the captured window's screen rect in physical pixels; `pillLeft`/`pillTop` are the
    // island's top-left on screen, so the crop follows the pill wherever the captured window sits.
    bool Update(ID3D11Texture2D* texture, const RECT& source, const D2D1_ROUNDED_RECT& shape, float scale,
                float pillLeft, float pillTop);
    bool Fill(ID2D1DeviceContext* target, const D2D1_ROUNDED_RECT& shape, float scale, float opacity);
    // Average luminance (0..1) of the refracted backdrop under the pill, which is what the status
    // line's ink has to stay readable against.
    float BackdropLuminance() const { return backdropLuma_; }
    // The average COLOUR of the same area. The hover preview's card uses it as its body, so that card is
    // the island's glass colour rather than a flat white or black fill.
    D2D1_COLOR_F BackdropColour() const { return backdropColour_; }

private:
    struct Cache;
    Cache* cache_ = nullptr;
    bool ready_ = false;
    float backdropLuma_ = 1.0f;
    D2D1_COLOR_F backdropColour_ = D2D1::ColorF(0.5f, 0.5f, 0.5f, 1.0f);
};

}  // namespace island
