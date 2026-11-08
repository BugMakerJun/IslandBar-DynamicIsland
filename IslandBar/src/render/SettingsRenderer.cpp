// =====================================================================
//  SettingsRenderer.cpp -- see the header.
// =====================================================================
#include "render/SettingsRenderer.h"

#include <algorithm>
#include <cmath>
#include <cwchar>

#include "render/Theme.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace island {
namespace {

constexpr D2D1_COLOR_F kWindow = {0.035f, 0.043f, 0.050f, 1.0f};
constexpr D2D1_COLOR_F kSidebar = {0.022f, 0.027f, 0.031f, 1.0f};
constexpr D2D1_COLOR_F kPanel = {0.055f, 0.066f, 0.076f, 1.0f};
constexpr D2D1_COLOR_F kPanelHover = {0.075f, 0.089f, 0.102f, 1.0f};
constexpr D2D1_COLOR_F kSelected = {0.070f, 0.180f, 0.300f, 1.0f};
constexpr D2D1_COLOR_F kEdge = {0.18f, 0.22f, 0.25f, 1.0f};
constexpr D2D1_COLOR_F kAccent = {0.33f, 0.69f, 1.0f, 1.0f};
constexpr D2D1_COLOR_F kDanger = {0.92f, 0.26f, 0.28f, 1.0f};
constexpr D2D1_COLOR_F kText = {0.94f, 0.96f, 0.98f, 1.0f};
constexpr D2D1_COLOR_F kMuted = {0.58f, 0.63f, 0.68f, 1.0f};

D2D1_COLOR_F Alpha(D2D1_COLOR_F colour, float alpha) {
    colour.a = alpha;
    return colour;
}

} // namespace

SettingsRenderer::~SettingsRenderer() {
    Shutdown();
}

bool SettingsRenderer::EnsureTextFormats() {
    if (!write_) return false;
    const auto make = [&](float size, DWRITE_FONT_WEIGHT weight,
                          Microsoft::WRL::ComPtr<IDWriteTextFormat>* out) {
        return SUCCEEDED(write_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, weight,
                                                  DWRITE_FONT_STYLE_NORMAL,
                                                  DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN",
                                                  out->ReleaseAndGetAddressOf()));
    };
    return make(19.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &brandFormat_) &&
           make(14.0f, DWRITE_FONT_WEIGHT_NORMAL, &navFormat_) &&
           make(26.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &pageTitleFormat_) &&
           make(13.0f, DWRITE_FONT_WEIGHT_NORMAL, &subtitleFormat_) &&
           make(13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &sectionFormat_) &&
           make(15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &rowFormat_) &&
           make(13.0f, DWRITE_FONT_WEIGHT_NORMAL, &valueFormat_) &&
           make(12.0f, DWRITE_FONT_WEIGHT_NORMAL, &smallFormat_);
}

bool SettingsRenderer::Initialize(HWND hwnd) {
    Shutdown();
    hwnd_ = hwnd;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.ReleaseAndGetAddressOf()))) {
        hwnd_ = nullptr;
        return false;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(write_.ReleaseAndGetAddressOf()))) ||
        !EnsureTextFormats()) {
        Shutdown();
        return false;
    }

    RECT client = {};
    GetClientRect(hwnd_, &client);
    const D2D1_SIZE_U pixels = {
        static_cast<UINT32>(std::max<LONG>(1, client.right - client.left)),
        static_cast<UINT32>(std::max<LONG>(1, client.bottom - client.top))};
    if (FAILED(factory_->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(hwnd_, pixels, D2D1_PRESENT_OPTIONS_NONE),
            target_.ReleaseAndGetAddressOf()))) {
        Shutdown();
        return false;
    }
    const UINT dpi = GetDpiForWindow(hwnd_);
    target_->SetDpi(static_cast<float>(dpi ? dpi : 96), static_cast<float>(dpi ? dpi : 96));
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    target_->CreateSolidColorBrush(kText, brush_.ReleaseAndGetAddressOf());
    return target_ && brush_;
}

void SettingsRenderer::Shutdown() {
    brush_.Reset();
    target_.Reset();
    brandFormat_.Reset();
    navFormat_.Reset();
    pageTitleFormat_.Reset();
    subtitleFormat_.Reset();
    sectionFormat_.Reset();
    rowFormat_.Reset();
    valueFormat_.Reset();
    smallFormat_.Reset();
    write_.Reset();
    factory_.Reset();
    hwnd_ = nullptr;
}

void SettingsRenderer::Resize() {
    if (!target_ || !hwnd_) return;
    RECT client = {};
    GetClientRect(hwnd_, &client);
    const D2D1_SIZE_U pixels = {
        static_cast<UINT32>(std::max<LONG>(1, client.right - client.left)),
        static_cast<UINT32>(std::max<LONG>(1, client.bottom - client.top))};
    target_->Resize(pixels);
    const UINT dpi = GetDpiForWindow(hwnd_);
    target_->SetDpi(static_cast<float>(dpi ? dpi : 96), static_cast<float>(dpi ? dpi : 96));
}

void SettingsRenderer::SetInk(const D2D1_COLOR_F& colour) {
    if (brush_) brush_->SetColor(colour);
}

void SettingsRenderer::Text(const wchar_t* text, const RectF& rect,
                            const Microsoft::WRL::ComPtr<IDWriteTextFormat>& format,
                            DWRITE_TEXT_ALIGNMENT alignment,
                            DWRITE_PARAGRAPH_ALIGNMENT paragraph) {
    if (!target_ || !brush_ || !text || !format) return;
    format->SetTextAlignment(alignment);
    format->SetParagraphAlignment(paragraph);
    target_->DrawTextW(text, static_cast<UINT32>(wcslen(text)), format.Get(),
                       D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom), brush_.Get());
}

void SettingsRenderer::FillRound(const RectF& rect, float radius, const D2D1_COLOR_F& colour) {
    if (!target_ || !brush_) return;
    SetInk(colour);
    const D2D1_ROUNDED_RECT rounded = {
        D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom), radius, radius};
    target_->FillRoundedRectangle(&rounded, brush_.Get());
}

void SettingsRenderer::StrokeRound(const RectF& rect, float radius, const D2D1_COLOR_F& colour,
                                   float width) {
    if (!target_ || !brush_) return;
    SetInk(colour);
    const D2D1_ROUNDED_RECT rounded = {
        D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom), radius, radius};
    target_->DrawRoundedRectangle(&rounded, brush_.Get(), width);
}

void SettingsRenderer::DrawNavigation(const SettingsLayout& layout, const SettingsViewModel& model) {
    SetInk(kText);
    Text(L"IslandBar", {22.0f, 10.0f, 158.0f, 34.0f}, brandFormat_);
    SetInk(kMuted);
    Text(L"SETTINGS", {22.0f, 47.0f, 158.0f, 68.0f}, smallFormat_);

    static const wchar_t* labels[] = {
        L"\u5E38\u89C4", L"\u5916\u89C2", L"\u6027\u80FD", L"\u5173\u4E8E"};
    for (int i = 0; i < static_cast<int>(SettingsPage::Count); ++i) {
        const RectF& row = layout.navigation[i];
        const SettingsHit target{SettingsHitKind::Navigation, i};
        const bool active = SettingsPageFromIndex(i) == model.page;
        const bool hovered = model.hovered == target;
        const bool focused = model.focused == target;
        FillRound(row, 9.0f, active ? kSelected : (hovered ? kPanelHover : Alpha(kPanel, 0.0f)));
        if (focused) StrokeRound(row, 9.0f, kAccent, 1.5f);
        SetInk(active ? kText : kMuted);
        Text(labels[i], {row.left + 14.0f, row.top, row.right - 8.0f, row.bottom}, navFormat_);
    }

    const SettingsHit close{SettingsHitKind::Close, -1};
    FillRound(layout.closeButton, 7.0f,
              model.hovered == close ? D2D1::ColorF(0.32f, 0.10f, 0.12f, 1.0f) : kPanel);
    StrokeRound(layout.closeButton, 7.0f, model.focused == close ? kAccent : kEdge, 1.0f);
    SetInk(kText);
    Text(L"\u00D7", layout.closeButton, navFormat_, DWRITE_TEXT_ALIGNMENT_CENTER,
         DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

void SettingsRenderer::DrawHeader(const SettingsLayout& layout, const SettingsViewModel& model) {
    static const wchar_t* titles[] = {
        L"\u5E38\u89C4", L"\u5916\u89C2", L"\u6027\u80FD", L"\u5173\u4E8E"};
    static const wchar_t* subtitles[] = {
        L"\u542F\u52A8\u4E0E\u8FD0\u884C\u72B6\u6001",
        L"\u8C03\u6574\u52A8\u6001\u5C9B\u7684\u5F62\u6001\u4E0E\u89C6\u89C9\u6548\u679C",
        L"\u5F53\u524D\u6293\u53D6\u4E0E\u91C7\u6837\u72B6\u6001",
        L"\u7248\u672C\u3001\u914D\u7F6E\u76EE\u5F55\u4E0E\u65E5\u5FD7"};
    const int page = SettingsPageIndex(model.page);
    SetInk(kText);
    Text(titles[page], layout.title, pageTitleFormat_);
    SetInk(kMuted);
    Text(subtitles[page], layout.subtitle, subtitleFormat_);
}

void SettingsRenderer::DrawStatusRow(const RectF& row, const wchar_t* label, const wchar_t* value) {
    FillRound(row, 10.0f, kPanel);
    StrokeRound(row, 10.0f, kEdge, 1.0f);
    SetInk(kText);
    Text(label, {row.left + 16.0f, row.top, row.left + 180.0f, row.bottom}, rowFormat_);
    SetInk(kMuted);
    Text(value, {row.left + 190.0f, row.top, row.right - 16.0f, row.bottom}, valueFormat_,
         DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

void SettingsRenderer::DrawGeneral(const SettingsLayout& layout, const SettingsViewModel& model) {
    const wchar_t* taskbar = !model.hidesNativeTaskbar
                                ? L"\u672A\u63A5\u7BA1"
                                : (model.keepsNativeTray ? L"\u5DF2\u88C1\u526A\uFF0C\u4EC5\u4FDD\u7559\u7CFB\u7EDF\u6258\u76D8"
                                                         : L"\u5DF2\u9690\u85CF");
    const wchar_t* fullscreen = model.fullscreenSuppressed
                                    ? L"\u5DF2\u6682\u505C\uFF08\u5168\u5C4F\u5E94\u7528\u5728\u524D\uFF09"
                                    : L"\u6B63\u5E38\u8FD0\u884C";
    const wchar_t* glass = model.glassCaptureEnabled
                               ? (model.fullscreenSuppressed ? L"\u5168\u5C4F\u65F6\u5DF2\u6682\u505C" : L"\u5DF2\u5F00\u542F")
                               : L"\u5DF2\u5173\u95ED";
    DrawStatusRow(layout.statusRows[0], L"\u539F\u751F\u4EFB\u52A1\u680F", taskbar);
    DrawStatusRow(layout.statusRows[1], L"\u5168\u5C4F\u6291\u5236", fullscreen);
    DrawStatusRow(layout.statusRows[2], L"\u5B9E\u65F6\u73BB\u7483\u6355\u83B7", glass);

    SetInk(kMuted);
    Text(L"\u542F\u52A8\u4E0E\u9000\u51FA", layout.startupLabel, smallFormat_);

    const SettingsHit autoStart{SettingsHitKind::ToggleAutoStart, -1};
    FillRound(layout.autoStartRow, 10.0f,
              model.hovered == autoStart ? kPanelHover : kPanel);
    StrokeRound(layout.autoStartRow, 10.0f,
                model.focused == autoStart ? kAccent : kEdge, 1.0f);
    SetInk(kText);
    Text(L"\u5F00\u673A\u81EA\u52A8\u542F\u52A8",
         {layout.autoStartRow.left + 16.0f, layout.autoStartRow.top,
          layout.autoStartValue.left - 12.0f, layout.autoStartRow.bottom},
         rowFormat_);
    SetInk(kMuted);
    Text(model.autoStartEnabled ? L"\u5DF2\u5F00\u542F" : L"\u5DF2\u5173\u95ED",
         layout.autoStartValue, valueFormat_, DWRITE_TEXT_ALIGNMENT_TRAILING,
         DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    const float switchRadius =
        (layout.autoStartSwitch.bottom - layout.autoStartSwitch.top) * 0.5f;
    FillRound(layout.autoStartSwitch, switchRadius,
              model.autoStartEnabled ? Alpha(kAccent, 0.78f) : D2D1::ColorF(0.16f, 0.19f, 0.22f, 1.0f));
    StrokeRound(layout.autoStartSwitch, switchRadius,
                model.autoStartEnabled ? Alpha(kAccent, 0.95f) : kEdge, 1.0f);
    const float knobSide = switchRadius * 1.55f;
    const float knobLeft = model.autoStartEnabled
                               ? layout.autoStartSwitch.right - knobSide - 3.0f
                               : layout.autoStartSwitch.left + 3.0f;
    if (target_) {
        SetInk(kText);
        target_->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(knobLeft + knobSide * 0.5f,
                                        (layout.autoStartSwitch.top +
                                         layout.autoStartSwitch.bottom) * 0.5f),
                          knobSide * 0.5f, knobSide * 0.5f),
            brush_.Get());
    }

    const SettingsHit quit{SettingsHitKind::Quit, -1};
    const bool quitHot = model.hovered == quit;
    FillRound(layout.quitRow, 10.0f,
              Alpha(kDanger, (quitHot ? 0.14f : 0.075f)));
    StrokeRound(layout.quitRow, 10.0f,
                model.focused == quit ? kDanger : Alpha(kDanger, 0.34f), 1.0f);
    SetInk(kText);
    Text(L"\u5B8C\u5168\u9000\u51FA IslandBar",
         {layout.quitRow.left + 16.0f, layout.quitRow.top + 5.0f,
          layout.quitRow.right - 16.0f, layout.quitRow.top + 31.0f},
         rowFormat_);
    SetInk(Alpha(kDanger, 0.92f));
    Text(L"\u6062\u590D\u539F\u751F\u4EFB\u52A1\u680F\u5E76\u7ED3\u675F\u8FDB\u7A0B",
         {layout.quitRow.left + 16.0f, layout.quitRow.top + 27.0f,
          layout.quitRow.right - 16.0f, layout.quitRow.bottom - 4.0f},
         smallFormat_);
}

void SettingsRenderer::DrawAppearance(const SettingsLayout& layout, const SettingsViewModel& model) {
    SetInk(kText);
    Text(L"\u52A8\u6001\u5C9B\u5F62\u6001", layout.shapeLabel, sectionFormat_);
    SetInk(kMuted);
    Text(L"\u9009\u62E9\u6536\u8D77\u65F6\u7684\u5916\u5F62", layout.shapeSubtitle, smallFormat_);

    const auto shapeChip = [&](const RectF& rect, const wchar_t* label, bool selected,
                               SettingsHitKind kind) {
        const SettingsHit target{kind, -1};
        FillRound(rect, 10.0f, selected ? kSelected : (model.hovered == target ? kPanelHover : kPanel));
        StrokeRound(rect, 10.0f, selected || model.focused == target ? kAccent : kEdge,
                    selected ? 2.0f : 1.0f);
        SetInk(selected ? kText : kMuted);
        Text(label, rect, rowFormat_, DWRITE_TEXT_ALIGNMENT_CENTER,
             DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    };
    shapeChip(layout.shapeCapsule, L"\u80F6\u56CA", model.shape == BarShape::Capsule,
              SettingsHitKind::ShapeCapsule);
    shapeChip(layout.shapeNotch, L"\u5218\u6D77", model.shape == BarShape::Notch,
              SettingsHitKind::ShapeNotch);

    SetInk(kText);
    Text(L"\u4E3B\u9898", layout.themeLabel, sectionFormat_);
    SetInk(kMuted);
    Text(L"\u70B9\u51FB\u540E\u7ACB\u5373\u8BA9\u52A8\u6001\u5C9B\u5E94\u7528\u65B0\u6750\u8D28", layout.themeSubtitle,
         smallFormat_);

    for (int i = 0; i < layout.themeCount; ++i) {
        const RectF& rect = layout.themeButtons[i];
        const SettingsHit target{SettingsHitKind::Theme, i};
        const bool selected = i == model.themeIndex;
        FillRound(rect, 9.0f, selected ? kSelected : (model.hovered == target ? kPanelHover : kPanel));
        StrokeRound(rect, 9.0f, selected || model.focused == target ? kAccent : kEdge,
                    selected ? 2.0f : 1.0f);
        SetInk(selected ? kText : kMuted);
        Text(ThemeAt(i).name, {rect.left + 14.0f, rect.top, rect.right - 14.0f, rect.bottom},
             rowFormat_);
    }
}

void SettingsRenderer::DrawPerformance(const SettingsLayout& layout, const SettingsViewModel& model) {
    DrawStatusRow(layout.statusRows[0], L"\u73BB\u7483\u6355\u83B7",
                  !model.glassCaptureEnabled ? L"\u5DF2\u5173\u95ED"
                  : model.fullscreenSuppressed ? L"\u5168\u5C4F\u65F6\u5DF2\u6682\u505C"
                                                : L"30 Hz \u6355\u83B7\uFF0C12.5 Hz \u5237\u65B0");
    DrawStatusRow(layout.statusRows[1], L"\u97F3\u9891\u5CF0\u503C\u91C7\u6837",
                  model.peakMeterOnDemand ? L"\u6309\u9700\u91C7\u6837\uFF0833 ms / 500 ms\uFF09"
                                          : L"\u5E38\u9A7B 30 Hz");
    DrawStatusRow(layout.statusRows[2], L"\u5168\u5C4F\u9000\u573A",
                  model.fullscreenSuppressed ? L"\u6B63\u5728\u6682\u505C\u540E\u53F0\u5DE5\u4F5C"
                                             : L"\u7B49\u5F85\u5168\u5C4F\u5E94\u7528");
}

void SettingsRenderer::DrawAbout(const SettingsLayout& layout, const SettingsViewModel& model) {
    const auto pathRow = [&](const RectF& row, const wchar_t* label, const wchar_t* path,
                             SettingsHitKind kind) {
        const SettingsHit target{kind, -1};
        FillRound(row, 10.0f, model.hovered == target ? kPanelHover : kPanel);
        StrokeRound(row, 10.0f, model.focused == target ? kAccent : kEdge, 1.0f);
        SetInk(kText);
        Text(label, {row.left + 16.0f, row.top + 4.0f, row.right - 16.0f, row.top + 30.0f},
             rowFormat_);
        SetInk(kMuted);
        Text(path ? path : L"—", {row.left + 16.0f, row.top + 28.0f, row.right - 16.0f,
                                  row.bottom - 4.0f},
             smallFormat_);
    };
    pathRow(layout.pathRows[0], L"\u914D\u7F6E\u76EE\u5F55", model.configPath,
            SettingsHitKind::OpenConfigFolder);
    pathRow(layout.pathRows[1], L"\u65E5\u5FD7\u6587\u4EF6", model.logPath,
            SettingsHitKind::OpenLogFolder);

}

HRESULT SettingsRenderer::Render(const SettingsViewModel& model) {
    if (!target_ || !brush_) return E_UNEXPECTED;
    target_->BeginDraw();
    target_->SetTransform(D2D1::Matrix3x2F::Identity());
    target_->Clear(kWindow);

    const D2D1_SIZE_F size = target_->GetSize();
    const SettingsLayout layout = SettingsLayoutFor(size.width, size.height, model.themeCount);
    SetInk(kSidebar);
    target_->FillRectangle(D2D1::RectF(0.0f, 0.0f, 180.0f, size.height), brush_.Get());
    SetInk(kEdge);
    target_->DrawLine(D2D1::Point2F(180.0f, 0.0f), D2D1::Point2F(180.0f, size.height),
                      brush_.Get(), 1.0f);
    target_->DrawLine(D2D1::Point2F(0.0f, 44.0f), D2D1::Point2F(size.width, 44.0f),
                      brush_.Get(), 1.0f);

    DrawNavigation(layout, model);
    DrawHeader(layout, model);
    switch (model.page) {
        case SettingsPage::General:
            DrawGeneral(layout, model);
            break;
        case SettingsPage::Appearance:
            DrawAppearance(layout, model);
            break;
        case SettingsPage::Performance:
            DrawPerformance(layout, model);
            break;
        case SettingsPage::About:
            DrawAbout(layout, model);
            break;
        case SettingsPage::Count:
            break;
    }

    return target_->EndDraw();
}

} // namespace island
