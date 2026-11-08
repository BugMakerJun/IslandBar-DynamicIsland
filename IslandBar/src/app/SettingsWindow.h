// =====================================================================
//  SettingsWindow.h -- IslandBar's separate, activating settings surface.
//
//  The window is created on demand and destroyed on close. It owns no worker and no bar state;
//  the frame loop applies shape/theme commands and feeds read-only runtime status back in.
// =====================================================================
#pragma once

#include <windows.h>

#include <string>

#include "domain/BarShape.h"
#include "domain/SettingsLayout.h"
#include "render/SettingsRenderer.h"

namespace island {

struct SettingsRuntimeStatus {
    bool hidesNativeTaskbar = true;
    bool keepsNativeTray = true;
    bool fullscreenSuppressed = false;
    bool glassCaptureEnabled = true;
    bool peakMeterOnDemand = true;
    bool autoStartEnabled = false;
    BarShape shape = BarShape::Capsule;
    int themeIndex = 0;
    int themeCount = 0;
};

class SettingsWindow {
public:
    SettingsWindow() = default;
    ~SettingsWindow();
    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    bool Open(HWND owner, SettingsPage page, const SettingsRuntimeStatus& status);
    void SetStatus(const SettingsRuntimeStatus& status);
    void Destroy();
    bool IsOpen() const { return hwnd_ != nullptr; }
    bool AutoStartEnabled() const { return status_.autoStartEnabled; }

    bool TakeShapeCommand(BarShape* shape);
    bool TakeThemeCommand(int* themeIndex);

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    void Activate(const SettingsHit& hit);
    SettingsHit HitAtClientPoint(POINT pixels) const;
    SettingsHit HitAtScreenPoint(POINT screen) const;
    SettingsHit NextFocusTarget(bool backwards) const;
    float DpiScale() const;
    void Invalidate();
    void Draw();
    void OpenFolder(const wchar_t* path) const;

    HWND owner_ = nullptr;
    HWND hwnd_ = nullptr;
    SettingsPage page_ = SettingsPage::General;
    SettingsRuntimeStatus status_;
    SettingsRenderer renderer_;
    SettingsHit hovered_;
    SettingsHit focused_;
    bool trackingMouse_ = false;
    bool pendingShape_ = false;
    bool pendingTheme_ = false;
    BarShape requestedShape_ = BarShape::Capsule;
    int requestedTheme_ = -1;
    unsigned long long privateBytesBeforeOpen_ = 0;
    unsigned long long openedAtMs_ = 0;
    bool firstPaintLogged_ = false;
    std::wstring configPath_;
    std::wstring logPath_;
};

} // namespace island
