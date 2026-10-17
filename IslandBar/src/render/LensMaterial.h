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

    bool Update(ID3D11Texture2D* texture, const RECT& monitor, const D2D1_ROUNDED_RECT& shape, float scale);
    bool Fill(ID2D1DeviceContext* target, const D2D1_ROUNDED_RECT& shape, float scale, float opacity);

private:
    struct Cache;
    Cache* cache_ = nullptr;
    bool ready_ = false;
};

}  // namespace island
