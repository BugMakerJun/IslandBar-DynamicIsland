#include "platform/ScrollTilingGrabLayer.h"

#include <algorithm>

namespace island {
namespace {

constexpr wchar_t kGrabLayerClass[] = L"IslandBar.ScrollTilingGrabLayer";
constexpr BYTE kGrabLayerAlpha = 1;

} // namespace

ScrollTilingGrabLayer::~ScrollTilingGrabLayer() {
    Destroy();
}

bool ScrollTilingGrabLayer::Create(const RECT& monitorScreen) {
    if (hwnd_ && IsWindow(hwnd_)) {
        SetWindowPos(hwnd_, nullptr, monitorScreen.left, monitorScreen.top,
                     monitorScreen.right - monitorScreen.left,
                     monitorScreen.bottom - monitorScreen.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return true;
    }

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &ScrollTilingGrabLayer::WndProcThunk;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_HAND);
    windowClass.lpszClassName = kGrabLayerClass;
    if (!RegisterClassExW(&windowClass) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    const DWORD exStyle = WS_EX_TOPMOST | WS_EX_NOACTIVATE |
                          WS_EX_TOOLWINDOW | WS_EX_LAYERED;
    hwnd_ = CreateWindowExW(
        exStyle, kGrabLayerClass, L"IslandBar scroll tiling drag", WS_POPUP,
        monitorScreen.left, monitorScreen.top,
        std::max(1L, monitorScreen.right - monitorScreen.left),
        std::max(1L, monitorScreen.bottom - monitorScreen.top),
        nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;

    SetLayeredWindowAttributes(hwnd_, 0, kGrabLayerAlpha, LWA_ALPHA);
    return true;
}

void ScrollTilingGrabLayer::Destroy() {
    Hide();
    if (hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

bool ScrollTilingGrabLayer::Show(const RECT& monitorScreen) {
    if (!Create(monitorScreen)) return false;
    ClearInput();
    const BOOL shown = SetWindowPos(
        hwnd_, HWND_TOPMOST, monitorScreen.left, monitorScreen.top,
        std::max(1L, monitorScreen.right - monitorScreen.left),
        std::max(1L, monitorScreen.bottom - monitorScreen.top),
        SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    if (!shown) return false;
    visible_ = true;
    SetCursor(LoadCursorW(nullptr, IDC_HAND));
    return true;
}

void ScrollTilingGrabLayer::Hide() {
    if (hwnd_ && IsWindow(hwnd_)) ShowWindow(hwnd_, SW_HIDE);
    visible_ = false;
    leftDown_ = false;
    trackingLeave_ = false;
    ClearInput();
}

bool ScrollTilingGrabLayer::TakeInput(ScrollTilingGrabInput* input) {
    if (!input) return false;
    *input = {};
    input->started = pendingStarted_;
    input->moved = pendingMoved_;
    input->ended = pendingEnded_;
    input->cancelled = pendingCancelled_;
    input->position = pendingPoint_;
    const bool any = input->started || input->moved || input->ended ||
                     input->cancelled;
    ClearInput();
    return any;
}

bool ScrollTilingGrabLayer::CoversPointer(POINT screenPoint) const {
    if (!visible_ || !hwnd_ || !IsWindow(hwnd_)) return false;
    RECT rect = {};
    if (!GetWindowRect(hwnd_, &rect) ||
        screenPoint.x < rect.left || screenPoint.x >= rect.right ||
        screenPoint.y < rect.top || screenPoint.y >= rect.bottom) {
        return false;
    }
    return WindowFromPoint(screenPoint) == hwnd_;
}

LRESULT CALLBACK ScrollTilingGrabLayer::WndProcThunk(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    ScrollTilingGrabLayer* layer = reinterpret_cast<ScrollTilingGrabLayer*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        layer = static_cast<ScrollTilingGrabLayer*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(layer));
    }
    if (!layer) return DefWindowProcW(hwnd, message, wParam, lParam);
    return layer->WndProc(hwnd, message, wParam, lParam);
}

LRESULT ScrollTilingGrabLayer::WndProc(HWND hwnd, UINT message, WPARAM wParam,
                                       LPARAM lParam) {
    (void)wParam;
    switch (message) {
        case WM_NCHITTEST:
            return HTCLIENT;

        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;

        case WM_ERASEBKGND:
            return TRUE;

        case WM_PAINT: {
            PAINTSTRUCT paint = {};
            HDC dc = BeginPaint(hwnd, &paint);
            RECT client = {};
            GetClientRect(hwnd, &client);
            FillRect(dc, &client,
                     static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            EndPaint(hwnd, &paint);
            return 0;
        }

        case WM_MOUSEMOVE:
            if (!trackingLeave_) {
                TRACKMOUSEEVENT tracking = {};
                tracking.cbSize = sizeof(tracking);
                tracking.dwFlags = TME_LEAVE;
                tracking.hwndTrack = hwnd;
                trackingLeave_ = TrackMouseEvent(&tracking) != FALSE;
            }
            if (leftDown_) {
                RememberPoint();
                pendingMoved_ = true;
            }
            return 0;

        case WM_LBUTTONDOWN:
            leftDown_ = true;
            RememberPoint();
            pendingStarted_ = true;
            return 0;

        case WM_LBUTTONUP:
            if (leftDown_) {
                leftDown_ = false;
                RememberPoint();
                pendingEnded_ = true;
            }
            return 0;

        case WM_MOUSELEAVE:
            trackingLeave_ = false;
            if (leftDown_) pendingCancelled_ = true;
            return 0;

        case WM_CANCELMODE:
            if (leftDown_) pendingCancelled_ = true;
            leftDown_ = false;
            return 0;

        case WM_DESTROY:
            hwnd_ = nullptr;
            visible_ = false;
            leftDown_ = false;
            trackingLeave_ = false;
            ClearInput();
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void ScrollTilingGrabLayer::RememberPoint() {
    if (!GetCursorPos(&pendingPoint_)) return;
    havePendingPoint_ = true;
}

void ScrollTilingGrabLayer::ClearInput() {
    pendingStarted_ = false;
    pendingMoved_ = false;
    pendingEnded_ = false;
    pendingCancelled_ = false;
    havePendingPoint_ = false;
    pendingPoint_ = {};
}

} // namespace island
