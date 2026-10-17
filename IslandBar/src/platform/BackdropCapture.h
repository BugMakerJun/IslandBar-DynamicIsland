// =====================================================================
//  BackdropCapture.h
//  The screen behind the bar, as a GPU texture -- Windows.Graphics.Capture (WGC).
//
//  WHY THIS EXISTS. A lens needs the pixels behind the glass, and Windows does not hand them to an app:
//  the composition backdrops blur but never expose a texture (HANDOFF 8.171). WGC is the supported way to
//  get them, and it stays on the GPU -- frames are D3D11 textures, nothing is read back to the CPU.
//
//  A PIMPL, deliberately: the WinRT capture types live in the .cpp, so this header can be included by the
//  renderer without dragging the C++/WinRT projection (and its /await) into every translation unit above
//  platform/. See the layering note in ARCHITECTURE.md.
// =====================================================================
#pragma once

#include <windows.h>

#include <d3d11.h>

#include <cstdint>
#include <memory>

namespace island {

class BackdropCapture {
public:
    BackdropCapture();
    ~BackdropCapture();

    BackdropCapture(const BackdropCapture&) = delete;
    BackdropCapture& operator=(const BackdropCapture&) = delete;

    // `device` is the renderer's D3D11 device: the frames must be textures it can wrap with D2D.
    // `exclude` is the bar's window, hidden from the capture so the lens cannot feed on itself.
    bool Start(ID3D11Device* device, HWND exclude, HMONITOR monitor);
    void Stop();

    bool Running() const;

    // True when a frame newer than the last one taken has arrived. The renderer polls this to know whether
    // the backdrop changed, which is also what makes the bar redraw only while something behind it moves.
    bool HasNewFrame() const;

    // The latest frame's texture, AddRef'd into `texture`. The frame is kept open until the next one
    // replaces it, so its texture stays valid while the renderer draws.
    bool AcquireLatest(ID3D11Texture2D** texture, std::uint64_t* revision);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace island
