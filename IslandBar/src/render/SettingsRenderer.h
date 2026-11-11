// =====================================================================
//  SettingsRenderer.h -- Direct2D painting for the separate settings window.
// =====================================================================
#pragma once

#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include "domain/BarShape.h"
#include "domain/SettingsLayout.h"

namespace island {

struct SettingsViewModel {
    SettingsPage page = SettingsPage::General;
    BarShape shape = BarShape::Capsule;
    int themeIndex = 0;
    int themeCount = 0;
    bool hidesNativeTaskbar = true;
    bool keepsNativeTray = true;
    bool fullscreenSuppressed = false;
    bool glassCaptureEnabled = true;
    bool peakMeterOnDemand = true;
    bool autoStartEnabled = false;
    bool scrollTilingEnabled = false;
    bool scrollTilingActive = false;
    int scrollTilingColumns = 0;
    int scrollTilingActiveColumn = -1;
    SettingsHit hovered;
    SettingsHit focused;
    const wchar_t* configPath = nullptr;
    const wchar_t* logPath = nullptr;
};

class SettingsRenderer {
public:
    SettingsRenderer() = default;
    ~SettingsRenderer();
    SettingsRenderer(const SettingsRenderer&) = delete;
    SettingsRenderer& operator=(const SettingsRenderer&) = delete;

    bool Initialize(HWND hwnd);
    void Shutdown();
    void Resize();
    HRESULT Render(const SettingsViewModel& model);
    bool Ready() const { return target_ != nullptr; }

private:
    bool EnsureTextFormats();
    void SetInk(const D2D1_COLOR_F& colour);
    void Text(const wchar_t* text, const RectF& rect,
              const Microsoft::WRL::ComPtr<IDWriteTextFormat>& format,
              DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING,
              DWRITE_PARAGRAPH_ALIGNMENT paragraph = DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    void FillRound(const RectF& rect, float radius, const D2D1_COLOR_F& colour);
    void StrokeRound(const RectF& rect, float radius, const D2D1_COLOR_F& colour, float width);
    void DrawNavigation(const SettingsLayout& layout, const SettingsViewModel& model);
    void DrawHeader(const SettingsLayout& layout, const SettingsViewModel& model);
    void DrawGeneral(const SettingsLayout& layout, const SettingsViewModel& model);
    void DrawAppearance(const SettingsLayout& layout, const SettingsViewModel& model);
    void DrawPerformance(const SettingsLayout& layout, const SettingsViewModel& model);
    void DrawExperimental(const SettingsLayout& layout, const SettingsViewModel& model);
    void DrawAbout(const SettingsLayout& layout, const SettingsViewModel& model);
    void DrawStatusRow(const RectF& row, const wchar_t* label, const wchar_t* value);
    void DrawSwitch(const RectF& track, bool enabled);

    HWND hwnd_ = nullptr;
    Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> write_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> brandFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> navFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> pageTitleFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> subtitleFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> sectionFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> rowFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> valueFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> smallFormat_;
};

} // namespace island
