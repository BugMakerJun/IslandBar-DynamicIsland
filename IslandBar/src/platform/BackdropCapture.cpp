// =====================================================================
//  BackdropCapture.cpp -- see the header.
// =====================================================================
#include "platform/BackdropCapture.h"

#include "core/Log.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <mutex>
#include <wrl/client.h>

using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

namespace island {

struct BackdropCapture::Impl {
    Direct3D11CaptureFramePool framePool{nullptr};
    GraphicsCaptureSession session{nullptr};
    GraphicsCaptureItem item{nullptr};
    Direct3D11CaptureFrame latestFrame{nullptr};
    winrt::event_token token{};
    mutable std::mutex mutex;
    std::uint64_t revision = 0;
    std::uint64_t acquired = 0;
    bool running = false;
};

BackdropCapture::BackdropCapture() : impl_(std::make_unique<Impl>()) {}
BackdropCapture::~BackdropCapture() { Stop(); }

bool BackdropCapture::Start(ID3D11Device* device, HWND exclude, HMONITOR monitor) {
    Impl& impl = *impl_;
    if (impl.running) return true;
    if (!device || !monitor) return false;
    if (exclude && !SetWindowDisplayAffinity(exclude, WDA_EXCLUDEFROMCAPTURE)) {
        LogLine(L"  lens: the bar could not be excluded from capture (error %lu)", GetLastError());
        return false;
    }
    try {
        const auto interop = winrt::get_activation_factory<GraphicsCaptureItem, ::IGraphicsCaptureItemInterop>();
        if (FAILED(interop->CreateForMonitor(monitor, winrt::guid_of<GraphicsCaptureItem>(),
                                             winrt::put_abi(impl.item)))) {
            return false;
        }
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi)))) return false;
        winrt::com_ptr<::IInspectable> inspectable;
        if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()))) return false;
        const IDirect3DDevice direct3d = inspectable.as<IDirect3DDevice>();

        impl.framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            direct3d, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, impl.item.Size());
        impl.session = impl.framePool.CreateCaptureSession(impl.item);
        impl.session.IsCursorCaptureEnabled(false);
        try { impl.session.IsBorderRequired(false); } catch (const winrt::hresult_error&) {}
        try { impl.session.MinUpdateInterval(std::chrono::milliseconds(33)); }
        catch (const winrt::hresult_error&) {}
        impl.token = impl.framePool.FrameArrived(
            [&impl](const Direct3D11CaptureFramePool& sender,
                    const winrt::Windows::Foundation::IInspectable&) {
                auto frame = sender.TryGetNextFrame();
                if (!frame) return;
                std::lock_guard<std::mutex> lock(impl.mutex);
                if (impl.latestFrame) impl.latestFrame.Close();
                impl.latestFrame = frame;
                ++impl.revision;
            });
        impl.session.StartCapture();
        impl.running = true;
        LogLine(L"  lens: capture started (%dx%d)", impl.item.Size().Width, impl.item.Size().Height);
        return true;
    } catch (const winrt::hresult_error& error) {
        LogLine(L"  lens: capture failed (0x%08lX)", static_cast<unsigned long>(error.code().value));
        Stop();
        return false;
    }
}

void BackdropCapture::Stop() {
    Impl& impl = *impl_;
    if (impl.framePool) { try { impl.framePool.FrameArrived(impl.token); } catch (const winrt::hresult_error&) {} }
    if (impl.session) { try { impl.session.Close(); } catch (const winrt::hresult_error&) {} }
    if (impl.framePool) { try { impl.framePool.Close(); } catch (const winrt::hresult_error&) {} }
    { std::lock_guard<std::mutex> lock(impl.mutex); if (impl.latestFrame) impl.latestFrame.Close(); impl.latestFrame = nullptr; }
    impl.session = nullptr;
    impl.framePool = nullptr;
    impl.item = nullptr;
    impl.running = false;
}

bool BackdropCapture::Running() const { return impl_->running; }

bool BackdropCapture::HasNewFrame() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->revision != impl_->acquired;
}

bool BackdropCapture::AcquireLatest(ID3D11Texture2D** texture, std::uint64_t* revision) {
    if (!texture) return false;
    *texture = nullptr;
    Impl& impl = *impl_;
    std::lock_guard<std::mutex> lock(impl.mutex);
    if (!impl.latestFrame) return false;
    const auto surface = impl.latestFrame.Surface();
    const auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    Microsoft::WRL::ComPtr<ID3D11Texture2D> latest;
    if (FAILED(access->GetInterface(IID_PPV_ARGS(&latest))) || !latest) return false;
    latest.CopyTo(texture);
    if (revision) *revision = impl.revision;
    impl.acquired = impl.revision;
    return true;
}

}  // namespace island
