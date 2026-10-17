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

    bool Start(ID3D11Device* device, HWND exclude, HMONITOR monitor);
    void Stop();
    bool Running() const;
    bool HasNewFrame() const;
    bool AcquireLatest(ID3D11Texture2D** texture, std::uint64_t* revision);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace island
