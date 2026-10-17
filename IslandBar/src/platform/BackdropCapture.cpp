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

#include <dwmapi.h>

#include <cstdlib>
#include <mutex>
#include <wrl/client.h>

#pragma comment(lib, "dwmapi.lib")

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
    HWND behind = nullptr;
    // Whether the capture item covers the window frame rather than only its client area. Resolved in
    // StartForWindow by matching the item size against both, because WGC does not say which one it uses.
    bool frameSpace = false;
    UINT startWidth = 0;
    UINT startHeight = 0;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    // The frame pool's textures are not reliably drawable by D2D. Every frame is copied into this
    // private texture, which is: same size, BGRA, shader-resource + render-target.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> stable;
    UINT stableWidth = 0;
    UINT stableHeight = 0;
};

BackdropCapture::BackdropCapture() : impl_(std::make_unique<Impl>()) {}
BackdropCapture::~BackdropCapture() { Stop(); }

bool BackdropCapture::StartForWindow(ID3D11Device* device, HWND behind) {
    Impl& impl = *impl_;
    if (impl.running) return true;
    if (!device || !behind) return false;
    if (behind == GetDesktopWindow()) {
        LogLine(L"  lens: the desktop window itself cannot be captured");
        return false;
    }
    try {
        const auto interop = winrt::get_activation_factory<GraphicsCaptureItem, ::IGraphicsCaptureItemInterop>();
        if (FAILED(interop->CreateForWindow(behind, winrt::guid_of<GraphicsCaptureItem>(),
                                            winrt::put_abi(impl.item)))) {
            return false;
        }
        impl.behind = behind;
        impl.context.Reset();
        device->GetImmediateContext(&impl.context);
        impl.stable.Reset();
        impl.stableWidth = 0;
        impl.stableHeight = 0;
        RECT client = {};
        GetClientRect(behind, &client);
        RECT frame = {};
        const bool haveFrame = SUCCEEDED(DwmGetWindowAttribute(behind, DWMWA_EXTENDED_FRAME_BOUNDS, &frame,
                                                               sizeof(frame)));
        const auto size = impl.item.Size();
        impl.startWidth = size.Width;
        impl.startHeight = size.Height;
        impl.frameSpace = haveFrame &&
                          std::abs(static_cast<int>(size.Width) - (frame.right - frame.left)) <= 1 &&
                          std::abs(static_cast<int>(size.Height) - (frame.bottom - frame.top)) <= 1;
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
        wchar_t cls[64] = {};
        GetClassNameW(behind, cls, 64);
        LogLine(L"  lens: capture started (%ux%u, %s) on %s", impl.startWidth, impl.startHeight,
                impl.frameSpace ? L"frame" : L"client", cls);
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

HWND BackdropCapture::CapturedWindow() const { return impl_->behind; }

RECT BackdropCapture::SourceRect() const {
    Impl& impl = *impl_;
    if (!impl.running || !impl.behind || !impl.item) return {};
    RECT at = {};
    if (impl.frameSpace &&
        SUCCEEDED(DwmGetWindowAttribute(impl.behind, DWMWA_EXTENDED_FRAME_BOUNDS, &at, sizeof(at)))) {
        // The extended frame bounds carry the window's own position already.
    } else {
        POINT origin = {0, 0};
        if (!ClientToScreen(impl.behind, &origin)) return {};
        at = {origin.x, origin.y, origin.x, origin.y};
    }
    const auto size = impl.item.Size();
    return {at.left, at.top, at.left + static_cast<LONG>(size.Width),
            at.top + static_cast<LONG>(size.Height)};
}

bool BackdropCapture::SizeChanged() const {
    const Impl& impl = *impl_;
    if (!impl.running || !impl.item) return false;
    const auto size = impl.item.Size();
    return static_cast<UINT>(size.Width) != impl.startWidth ||
           static_cast<UINT>(size.Height) != impl.startHeight;
}

ID3D11Texture2D* BackdropCapture::LatestTexture() const { return impl_->stable.Get(); }

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
    if (impl.context) {
        D3D11_TEXTURE2D_DESC desc = {};
        latest->GetDesc(&desc);
        if (!impl.stable || impl.stableWidth != desc.Width || impl.stableHeight != desc.Height) {
            D3D11_TEXTURE2D_DESC copyDesc = desc;
            copyDesc.Usage = D3D11_USAGE_DEFAULT;
            copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            copyDesc.CPUAccessFlags = 0;
            copyDesc.MiscFlags = 0;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> stable;
            ID3D11Device* device = nullptr;
            latest->GetDevice(&device);
            const HRESULT created =
                device ? device->CreateTexture2D(&copyDesc, nullptr, &stable) : E_FAIL;
            if (device) device->Release();
            if (SUCCEEDED(created)) {
                impl.stable = stable;
                impl.stableWidth = desc.Width;
                impl.stableHeight = desc.Height;
            }
        }
        if (impl.stable) {
            impl.context->CopyResource(impl.stable.Get(), latest.Get());
            impl.stable.CopyTo(texture);
        }
    }
    if (revision) *revision = impl.revision;
    impl.acquired = impl.revision;
    return true;
}

}  // namespace island
