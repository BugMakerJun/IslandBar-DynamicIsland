// =====================================================================
//  SettingsWindow.cpp -- see the header.
// =====================================================================
#include "app/SettingsWindow.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "core/Log.h"
#include "core/ProcessMetrics.h"
#include "platform/AutoStart.h"
#include "render/Theme.h"

#pragma comment(lib, "dwmapi.lib")

namespace island {
namespace {

constexpr const wchar_t* kClassName = L"IslandBar.SettingsWindow";
constexpr float kDefaultWidthDip = 760.0f;
constexpr float kDefaultHeightDip = 560.0f;
constexpr float kMinWidthDip = 680.0f;
constexpr float kMinHeightDip = 500.0f;
constexpr int kTitleBarDip = 44;

bool IsResizeHit(LRESULT hit) {
    return hit == HTLEFT || hit == HTRIGHT || hit == HTTOP || hit == HTBOTTOM ||
           hit == HTTOPLEFT || hit == HTTOPRIGHT || hit == HTBOTTOMLEFT ||
           hit == HTBOTTOMRIGHT;
}

const char* PageLogName(SettingsPage page) {
    switch (page) {
        case SettingsPage::General: return "general";
        case SettingsPage::Appearance: return "appearance";
        case SettingsPage::Performance: return "performance";
        case SettingsPage::Experimental: return "experimental";
        case SettingsPage::About: return "about";
        case SettingsPage::Count: break;
    }
    return "general";
}

} // namespace

SettingsWindow::~SettingsWindow() {
    Destroy();
}

bool SettingsWindow::Open(HWND owner, SettingsPage page, const SettingsRuntimeStatus& status) {
    owner_ = owner;
    page_ = page;
    status_ = status;

    if (hwnd_) {
        openedAtMs_ = GetTickCount64();
        firstPaintLogged_ = false;
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd_);
        SetFocus(hwnd_);
        Invalidate();
        LogAndPrint("  settings: activated existing window source=panel-appearance page=%s",
                    PageLogName(page_));
        return true;
    }

    ProcessMemorySample before = {};
    SampleProcessMemory(&before);
    privateBytesBeforeOpen_ = before.privateBytes;
    openedAtMs_ = GetTickCount64();
    firstPaintLogged_ = false;

    wchar_t home[MAX_PATH] = {};
    const DWORD homeLength = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    if (homeLength > 0 && homeLength < MAX_PATH) configPath_ = std::wstring(home) + L"\\IslandBar";
    const wchar_t* log = LogPath();
    if (log) logPath_ = log;

    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW cls = {};
    cls.cbSize = sizeof(cls);
    cls.style = CS_HREDRAW | CS_VREDRAW;
    cls.lpfnWndProc = &SettingsWindow::WndProcThunk;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = nullptr;
    cls.lpszClassName = kClassName;
    if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    const UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
    const DWORD style = WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT bounds = {0, 0, DipToPixels(kDefaultWidthDip, dpi), DipToPixels(kDefaultHeightDip, dpi)};
    AdjustWindowRectExForDpi(&bounds, style, FALSE, WS_EX_APPWINDOW, dpi ? dpi : 96);
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;

    RECT work = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (owner) {
        MONITORINFO info = {};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &info)) {
            work = info.rcWork;
        }
    }
    const int x = static_cast<int>(work.left) +
                  std::max(0, static_cast<int>(work.right - work.left) - width) / 2;
    const int y = static_cast<int>(work.top) +
                  std::max(0, static_cast<int>(work.bottom - work.top) - height) / 2;

    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, kClassName, L"IslandBar Settings",
                            style, x, y, width, height, nullptr, nullptr, instance, this);
    if (!hwnd_) {
        LogAndPrint("  settings: create FAILED (error %lu)", GetLastError());
        return false;
    }

    const int dark = TRUE;
    DwmSetWindowAttribute(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    const int round = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd_, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));

    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
    Invalidate();
    LogAndPrint("  settings: opened source=panel-appearance page=%s hwnd=%p",
                PageLogName(page_), reinterpret_cast<void*>(hwnd_));
    return true;
}

void SettingsWindow::SetStatus(const SettingsRuntimeStatus& status) {
    const bool changed = status.shape != status_.shape || status.themeIndex != status_.themeIndex ||
                         status.themeCount != status_.themeCount ||
                         status.hidesNativeTaskbar != status_.hidesNativeTaskbar ||
                         status.keepsNativeTray != status_.keepsNativeTray ||
                         status.fullscreenSuppressed != status_.fullscreenSuppressed ||
                         status.glassCaptureEnabled != status_.glassCaptureEnabled ||
                         status.peakMeterOnDemand != status_.peakMeterOnDemand ||
                         status.autoStartEnabled != status_.autoStartEnabled ||
                         status.scrollTilingEnabled != status_.scrollTilingEnabled ||
                         status.scrollTilingActive != status_.scrollTilingActive ||
                         status.scrollTilingColumns != status_.scrollTilingColumns ||
                         status.scrollTilingActiveColumn != status_.scrollTilingActiveColumn;
    status_ = status;
    if (changed) Invalidate();
}

void SettingsWindow::Destroy() {
    if (hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

bool SettingsWindow::TakeShapeCommand(BarShape* shape) {
    if (!pendingShape_ || !shape) return false;
    pendingShape_ = false;
    *shape = requestedShape_;
    return true;
}

bool SettingsWindow::TakeThemeCommand(int* themeIndex) {
    if (!pendingTheme_ || !themeIndex) return false;
    pendingTheme_ = false;
    *themeIndex = requestedTheme_;
    return true;
}

bool SettingsWindow::TakeScrollTilingPreference(bool* enabled) {
    if (!pendingScrollTilingPreference_ || !enabled) return false;
    pendingScrollTilingPreference_ = false;
    *enabled = requestedScrollTilingEnabled_;
    return true;
}

bool SettingsWindow::TakeScrollTilingCommand(ScrollTilingCommand* command) {
    if (pendingScrollTilingCommand_ == ScrollTilingCommand::None || !command) return false;
    *command = pendingScrollTilingCommand_;
    pendingScrollTilingCommand_ = ScrollTilingCommand::None;
    return true;
}

float SettingsWindow::DpiScale() const {
    const UINT dpi = hwnd_ ? GetDpiForWindow(hwnd_) : 96;
    return static_cast<float>(dpi ? dpi : 96) / 96.0f;
}

void SettingsWindow::Invalidate() {
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

SettingsHit SettingsWindow::HitAtClientPoint(POINT pixels) const {
    if (!hwnd_) return {};
    RECT client = {};
    GetClientRect(hwnd_, &client);
    const UINT dpi = GetDpiForWindow(hwnd_);
    const float width = PixelsToDip(client.right - client.left, dpi);
    const float height = PixelsToDip(client.bottom - client.top, dpi);
    const SettingsLayout layout = SettingsLayoutFor(width, height, status_.themeCount);
    return SettingsHitAt(layout, page_, PixelsToDip(pixels.x, dpi), PixelsToDip(pixels.y, dpi));
}

SettingsHit SettingsWindow::HitAtScreenPoint(POINT screen) const {
    POINT client = screen;
    ScreenToClient(hwnd_, &client);
    return HitAtClientPoint(client);
}

SettingsHit SettingsWindow::NextFocusTarget(bool backwards) const {
    if (!hwnd_) return {};
    RECT client = {};
    GetClientRect(hwnd_, &client);
    const UINT dpi = GetDpiForWindow(hwnd_);
    const SettingsLayout layout = SettingsLayoutFor(PixelsToDip(client.right - client.left, dpi),
                                                    PixelsToDip(client.bottom - client.top, dpi),
                                                    status_.themeCount);
    SettingsHit targets[24] = {};
    const int count = SettingsFocusTargets(layout, page_, targets, 24);
    if (count <= 0) return {};
    int current = -1;
    for (int i = 0; i < count; ++i) {
        if (targets[i] == focused_) {
            current = i;
            break;
        }
    }
    if (backwards) current = current <= 0 ? count - 1 : current - 1;
    else current = (current + 1) % count;
    return targets[current];
}

void SettingsWindow::OpenFolder(const wchar_t* path) const {
    if (!path || !path[0]) return;
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", path, nullptr, nullptr, SW_SHOWNORMAL);
    LogAndPrint("  settings: open path '%ls' result=%lld", path,
                static_cast<long long>(reinterpret_cast<INT_PTR>(result)));
}

void SettingsWindow::Activate(const SettingsHit& hit) {
    switch (hit.kind) {
        case SettingsHitKind::Close:
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            break;
        case SettingsHitKind::Navigation:
            page_ = SettingsPageFromIndex(hit.index);
            focused_ = hit;
            Invalidate();
            LogAndPrint("  settings: page=%s", PageLogName(page_));
            break;
        case SettingsHitKind::ShapeCapsule:
        case SettingsHitKind::ShapeNotch:
            requestedShape_ = hit.kind == SettingsHitKind::ShapeNotch ? BarShape::Notch
                                                                      : BarShape::Capsule;
            pendingShape_ = true;
            status_.shape = requestedShape_;
            focused_ = hit;
            Invalidate();
            break;
        case SettingsHitKind::Theme:
            if (hit.index >= 0 && hit.index < status_.themeCount) {
                requestedTheme_ = hit.index;
                pendingTheme_ = true;
                status_.themeIndex = hit.index;
                focused_ = hit;
                Invalidate();
            }
            break;
        case SettingsHitKind::ToggleAutoStart: {
            const bool wanted = !status_.autoStartEnabled;
            const bool saved = SetAutoStartEnabled(wanted);
            if (saved) status_.autoStartEnabled = wanted;
            focused_ = hit;
            Invalidate();
            LogAndPrint("  settings: auto-start=%ls persist=%ls",
                        wanted ? L"on" : L"off", saved ? L"success" : L"FAILED");
            break;
        }
        case SettingsHitKind::ToggleScrollTiling:
            requestedScrollTilingEnabled_ = !status_.scrollTilingEnabled;
            pendingScrollTilingPreference_ = true;
            status_.scrollTilingEnabled = requestedScrollTilingEnabled_;
            if (!requestedScrollTilingEnabled_) {
                pendingScrollTilingCommand_ = ScrollTilingCommand::Exit;
            }
            focused_ = hit;
            Invalidate();
            LogAndPrint("  settings: scroll-tiling-enabled=%ls",
                        requestedScrollTilingEnabled_ ? L"on" : L"off");
            break;
        case SettingsHitKind::ScrollTilingAction:
            if (status_.scrollTilingEnabled) {
                pendingScrollTilingCommand_ =
                    status_.scrollTilingActive ? ScrollTilingCommand::Exit
                                               : ScrollTilingCommand::Enter;
                focused_ = hit;
                Invalidate();
                LogAndPrint("  settings: scroll-tiling-action=%ls",
                            status_.scrollTilingActive ? L"exit" : L"enter");
            }
            break;
        case SettingsHitKind::Quit:
            LogAndPrint("  settings: full exit requested");
            PostQuitMessage(0);
            DestroyWindow(hwnd_);
            break;
        case SettingsHitKind::OpenConfigFolder:
            OpenFolder(configPath_.c_str());
            break;
        case SettingsHitKind::OpenLogFolder:
            OpenFolder(logPath_.c_str());
            break;
        case SettingsHitKind::None:
            break;
    }
}

void SettingsWindow::Draw() {
    if (!renderer_.Ready() && !renderer_.Initialize(hwnd_)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            LogAndPrint("  settings: Direct2D initialization FAILED");
        }
        return;
    }

    SettingsViewModel model;
    model.page = page_;
    model.shape = status_.shape;
    model.themeIndex = status_.themeIndex;
    model.themeCount = status_.themeCount;
    model.hidesNativeTaskbar = status_.hidesNativeTaskbar;
    model.keepsNativeTray = status_.keepsNativeTray;
    model.fullscreenSuppressed = status_.fullscreenSuppressed;
    model.glassCaptureEnabled = status_.glassCaptureEnabled;
    model.peakMeterOnDemand = status_.peakMeterOnDemand;
    model.autoStartEnabled = status_.autoStartEnabled;
    model.scrollTilingEnabled = status_.scrollTilingEnabled;
    model.scrollTilingActive = status_.scrollTilingActive;
    model.scrollTilingColumns = status_.scrollTilingColumns;
    model.scrollTilingActiveColumn = status_.scrollTilingActiveColumn;
    model.hovered = hovered_;
    model.focused = focused_;
    model.configPath = configPath_.c_str();
    model.logPath = logPath_.c_str();

    const HRESULT hr = renderer_.Render(model);
    if (hr == D2DERR_RECREATE_TARGET) {
        renderer_.Shutdown();
        Invalidate();
    } else if (SUCCEEDED(hr) && !firstPaintLogged_) {
        firstPaintLogged_ = true;
        LogAndPrint("  settings: first-paint_ms=%llu", GetTickCount64() - openedAtMs_);
    }
}

LRESULT CALLBACK SettingsWindow::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    SettingsWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<SettingsWindow*>(create->lpCreateParams);
        if (self) self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);
    return self->WndProc(hwnd, message, wParam, lParam);
}

LRESULT SettingsWindow::WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            return 0;

        case WM_SIZE:
            renderer_.Resize();
            Invalidate();
            return 0;

        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            renderer_.Resize();
            Invalidate();
            return 0;
        }

        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            const UINT dpi = GetDpiForWindow(hwnd);
            info->ptMinTrackSize.x = DipToPixels(kMinWidthDip, dpi);
            info->ptMinTrackSize.y = DipToPixels(kMinHeightDip, dpi);
            return 0;
        }

        case WM_NCHITTEST: {
            const LRESULT base = DefWindowProcW(hwnd, message, wParam, lParam);
            if (IsResizeHit(base)) return base;
            const POINT screen = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const SettingsHit hit = HitAtScreenPoint(screen);
            if (hit.kind != SettingsHitKind::None) return HTCLIENT;
            POINT client = screen;
            ScreenToClient(hwnd, &client);
            const UINT dpi = GetDpiForWindow(hwnd);
            if (PixelsToDip(client.y, dpi) < static_cast<float>(kTitleBarDip)) return HTCAPTION;
            return HTCLIENT;
        }

        case WM_SETCURSOR: {
            if (LOWORD(lParam) != HTCLIENT) break;
            POINT screen = {};
            const SettingsHit hit = GetCursorPos(&screen) ? HitAtScreenPoint(screen) : SettingsHit{};
            SetCursor(LoadCursorW(nullptr, hit.kind == SettingsHitKind::None ? IDC_ARROW : IDC_HAND));
            return TRUE;
        }

        case WM_MOUSEMOVE: {
            POINT pixels = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const SettingsHit next = HitAtClientPoint(pixels);
            if (next != hovered_) {
                hovered_ = next;
                Invalidate();
            }
            if (!trackingMouse_) {
                TRACKMOUSEEVENT track = {};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = hwnd;
                TrackMouseEvent(&track);
                trackingMouse_ = true;
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            trackingMouse_ = false;
            hovered_ = {};
            Invalidate();
            return 0;

        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            POINT pixels = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const SettingsHit hit = HitAtClientPoint(pixels);
            if (hit.kind != SettingsHitKind::None) Activate(hit);
            return 0;
        }

        case WM_KEYDOWN:
            if (wParam == VK_TAB) {
                focused_ = NextFocusTarget((GetKeyState(VK_SHIFT) & 0x8000) != 0);
                Invalidate();
                return 0;
            }
            if (wParam == VK_RETURN || wParam == VK_SPACE) {
                if (focused_.kind != SettingsHitKind::None) Activate(focused_);
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
            break;

        case WM_PAINT: {
            PAINTSTRUCT paint = {};
            BeginPaint(hwnd, &paint);
            Draw();
            EndPaint(hwnd, &paint);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY: {
            renderer_.Shutdown();
            ProcessMemorySample after = {};
            SampleProcessMemory(&after);
            const long long deltaKb =
                (static_cast<long long>(after.privateBytes) -
                 static_cast<long long>(privateBytesBeforeOpen_)) / 1024;
            LogAndPrint("  settings: closed reason=user memory_delta_kb=%lld hwnd_destroyed=1 "
                        "resources_released=1", deltaKb);
            hwnd_ = nullptr;
            return 0;
        }

        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace island
