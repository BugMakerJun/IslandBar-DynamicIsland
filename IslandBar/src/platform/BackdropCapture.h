// =====================================================================
//  BackdropCapture.h -- the desktop behind the bar as a GPU texture (Windows.Graphics.Capture).
//
//  PIMPL on purpose: the WinRT capture types stay in the .cpp, so the renderer can include this header
//  without the C++/WinRT projection leaking into every layer above platform/.
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

    // Captures `behind` -- the WINDOW under the island, not the monitor. A window capture never contains
    // the bar itself, so no screenshot exclusion is needed and the lens cannot feed on its own output.
    bool StartForWindow(ID3D11Device* device, HWND behind);
    void Stop();
    HWND CapturedWindow() const;
    // The captured content's screen rect, in physical pixels. WGC's window item is either the window
    // frame or its client area depending on the window; SourceRect() resolves which one from the item
    // size, and follows the window while it moves.
    RECT SourceRect() const;
    bool Running() const;
    // The capture item's size follows the window's size; the frame pool does not, so the session has
    // to be recreated when this says so.
    bool SizeChanged() const;
    bool HasNewFrame() const;
    bool AcquireLatest(ID3D11Texture2D** texture, std::uint64_t* revision);
    // The most recent captured frame, even when it has already been consumed. Used to re-shape the
    // lens when only the pill changed size (the window behind did not, so WGC sends nothing new).
    ID3D11Texture2D* LatestTexture() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace island
