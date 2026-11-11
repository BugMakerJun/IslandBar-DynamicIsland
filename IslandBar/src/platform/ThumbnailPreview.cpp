// =====================================================================
//  ThumbnailPreview.cpp
// =====================================================================
#include "platform/ThumbnailPreview.h"

#include "domain/Geometry.h"
#include "core/Log.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "msimg32.lib")

namespace island {
namespace {

constexpr wchar_t kThumbnailClassName[] = L"IslandBarThumbnail";

int DipToPx(float dip, float scale) {
    return static_cast<int>(std::lround(dip * scale));
}

COLORREF MixColour(COLORREF from, COLORREF to, float amount) {
    const float t = amount < 0.0f ? 0.0f : (amount > 1.0f ? 1.0f : amount);
    const auto channel = [t](BYTE a, BYTE b) {
        return static_cast<BYTE>(static_cast<float>(a) +
                                 (static_cast<float>(b) - static_cast<float>(a)) * t + 0.5f);
    };
    return RGB(channel(GetRValue(from), GetRValue(to)),
               channel(GetGValue(from), GetGValue(to)),
               channel(GetBValue(from), GetBValue(to)));
}

void FillRectColour(HDC dc, const RECT& rect, COLORREF colour) {
    if (!dc || rect.right <= rect.left || rect.bottom <= rect.top) return;
    HBRUSH brush = CreateSolidBrush(colour);
    if (!brush) return;
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void FillVerticalGradient(HDC dc, const RECT& rect, COLORREF top, COLORREF bottom) {
    if (!dc || rect.right <= rect.left || rect.bottom <= rect.top) return;

    TRIVERTEX vertices[2] = {};
    vertices[0].x = rect.left;
    vertices[0].y = rect.top;
    vertices[0].Red = static_cast<COLOR16>(GetRValue(top) << 8);
    vertices[0].Green = static_cast<COLOR16>(GetGValue(top) << 8);
    vertices[0].Blue = static_cast<COLOR16>(GetBValue(top) << 8);
    vertices[0].Alpha = 0xFF00;
    vertices[1].x = rect.right;
    vertices[1].y = rect.bottom;
    vertices[1].Red = static_cast<COLOR16>(GetRValue(bottom) << 8);
    vertices[1].Green = static_cast<COLOR16>(GetGValue(bottom) << 8);
    vertices[1].Blue = static_cast<COLOR16>(GetBValue(bottom) << 8);
    vertices[1].Alpha = 0xFF00;

    GRADIENT_RECT gradient = {0, 1};
    GradientFill(dc, vertices, 2, &gradient, 1, GRADIENT_FILL_RECT_V);
}

} // namespace

ThumbnailPlacement ThumbnailBoxFor(int sourceWidthPx, int sourceHeightPx, const RECT& iconBoxPx,
                                   const RECT& monitorPx, float scale) {
    ThumbnailPlacement placement;
    if (scale <= 0.0f) scale = 1.0f;

    const float slotWidth = kThumbnailMaxWidthDip * scale;
    const float slotHeight = kThumbnailMaxHeightDip * scale;
    const float pad = kThumbnailPadDip * scale;
    const float header = kThumbnailHeaderDip * scale;
    const float footer = kThumbnailFooterDip * scale;

    // THE CARD IS ONE SIZE, whatever window it is showing: one margin, one header, one slot for the
    // picture and one footer. Every preview in a row is then the same object, which is what the shell's
    // own previews are and what "现在预览框 大小也不一样" asked for.
    const float cardWidth = slotWidth + pad * 2.0f;
    const float cardHeight = slotHeight + pad * 2.0f + header + footer;

    // THE PICTURE takes the source's shape INSIDE that slot, and the leftover space becomes card: a
    // letterbox. The shape has to come from the compositor because it stretches what it draws to fill the
    // rectangle it is given, so any disagreement between the two is a squashed picture.
    //
    // No minimum side any more: it existed to stop an absurdly shaped source (a 1 x 9000 strip) from
    // giving a card that was itself a line, and the fixed slot removes that problem -- the card stays 276
    // DIP wide and the picture is a sliver in the middle of it, which is honest about a window that IS a
    // sliver. Clamping the side instead would stretch that sliver back into a lie.
    float width = slotWidth;
    float height = slotHeight;
    if (sourceWidthPx > 0 && sourceHeightPx > 0) {
        const float aspect = static_cast<float>(sourceWidthPx) / static_cast<float>(sourceHeightPx);
        width = slotWidth;
        height = width / aspect;
        if (height > slotHeight) {
            height = slotHeight;
            width = height * aspect;
        }
        // A floor of one pixel rather than a minimum SIDE: a source whose shape is absurd collapses to
        // nothing at all otherwise (a 1 x 9000 window comes out under a tenth of a pixel wide), and a
        // picture that is one pixel wide is still a picture of something, where an empty rectangle is a
        // card with an inexplicable hole in it.
        if (width < 1.0f) width = 1.0f;
        if (height < 1.0f) height = 1.0f;
    }

    // Centred under the icon, and hanging below it by the gap -- from the icon's BOTTOM edge, because
    // that is the edge the pointer came in over.
    const float centreX = (static_cast<float>(iconBoxPx.left) + static_cast<float>(iconBoxPx.right)) * 0.5f;
    float left = centreX - cardWidth * 0.5f;
    float top = static_cast<float>(iconBoxPx.bottom) + kThumbnailGapDip * scale;

    // Inside the monitor, on both axes -- the CARD is what has to fit, because the card is what is on
    // screen. A preview that hung off the edge would be clipped by the
    // display, which reads as a rendering fault rather than as a window that is too near the corner.
    const float monitorLeft = static_cast<float>(monitorPx.left);
    const float monitorRight = static_cast<float>(monitorPx.right);
    const float monitorTop = static_cast<float>(monitorPx.top);
    const float monitorBottom = static_cast<float>(monitorPx.bottom);

    if (left + cardWidth > monitorRight) left = monitorRight - cardWidth;
    if (left < monitorLeft) left = monitorLeft;
    if (top + cardHeight > monitorBottom) top = monitorBottom - cardHeight;
    if (top < monitorTop) top = monitorTop;

    placement.card.left = static_cast<LONG>(left);
    placement.card.top = static_cast<LONG>(top);
    placement.card.right = static_cast<LONG>(left + cardWidth);
    placement.card.bottom = static_cast<LONG>(top + cardHeight);
    placement.scale = scale;

    // The named bands are returned as well as being used here. Painting has to leave the picture rectangle
    // COMPLETELY untouched for DWM, and separate header/slot/footer rectangles make that an explicit part
    // of the pure geometry instead of four numbers reconstructed independently by the paint code.
    const int headerBottom = DipToPx(kThumbnailPadDip + kThumbnailHeaderDip, scale);
    const int slotHeightPx = DipToPx(kThumbnailMaxHeightDip, scale);
    const int footerHeightPx = DipToPx(kThumbnailFooterDip, scale);
    placement.header = {placement.card.left, placement.card.top, placement.card.right,
                        placement.card.top + headerBottom};
    placement.slot = {placement.card.left + DipToPx(kThumbnailPadDip, scale), placement.header.bottom,
                      placement.card.right - DipToPx(kThumbnailPadDip, scale),
                      placement.header.bottom + slotHeightPx};
    placement.footer = {placement.card.left, placement.card.bottom - footerHeightPx,
                        placement.card.right, placement.card.bottom};

    // The picture is CENTRED in the slot, so the letterbox is even on both sides -- and ROUNDED rather
    // than truncated, because truncating a half-pixel moved the letterbox a pixel to one side and the
    // self-test's own "even top to bottom" caught it (6 above against 8 below).
    placement.picture.left =
        std::lround(static_cast<float>(placement.slot.left) + (slotWidth - width) * 0.5f);
    placement.picture.top =
        std::lround(static_cast<float>(placement.slot.top) + (slotHeight - height) * 0.5f);
    placement.picture.right = placement.picture.left + std::lround(width);
    placement.picture.bottom = placement.picture.top + std::lround(height);

    placement.valid = (placement.card.right > placement.card.left) &&
                      (placement.card.bottom > placement.card.top) &&
                      (placement.header.right > placement.header.left) &&
                      (placement.header.bottom > placement.header.top) &&
                      (placement.slot.right > placement.slot.left) &&
                      (placement.slot.bottom > placement.slot.top) &&
                      (placement.slot.top >= placement.header.bottom) &&
                      (placement.footer.top >= placement.slot.bottom) &&
                      (placement.footer.right > placement.footer.left) &&
                      (placement.footer.bottom > placement.footer.top) &&
                      (placement.picture.right > placement.picture.left) &&
                      (placement.picture.bottom > placement.picture.top);
    return placement;
}

ThumbnailPreview::~ThumbnailPreview() {
    Destroy();
}

bool ThumbnailPreview::Create(HWND owner) {
    if (hwnd_) return true;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &ThumbnailPreview::WndProcThunk;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;   // the compositor paints the thumbnail
    wc.lpszClassName = kThumbnailClassName;

    // A second registration of the same class fails with ERROR_CLASS_ALREADY_EXISTS, which is not an
    // error: it is process-wide.
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    // NO WS_EX_LAYERED, and that is not a detail: DWM refuses to register a thumbnail whose destination
    // is a layered window. WS_EX_NOACTIVATE keeps it from taking focus -- the bar is not allowed to steal
    // the foreground, and neither is anything it owns.
    //
    // WS_EX_TRANSPARENT IS DELIBERATELY ABSENT, though it is the usual way to make a window
    // click-through: measured on this machine, a thumbnail is registered successfully into such a window
    // (DwmRegisterThumbnail returns S_OK, the window is visible at the right place and the source is an
    // ordinary visible window) and NOTHING IS DRAWN INTO IT. Click-through is done by answering
    // HTTRANSPARENT to WM_NCHITTEST instead, which is the same trick the bar itself uses and leaves the
    // window ordinary in every way the compositor cares about.
    const DWORD exStyle = WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;

    hwnd_ = CreateWindowExW(exStyle, kThumbnailClassName, L"IslandBar preview", WS_POPUP, 0, 0, 1, 1,
                            owner, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;
    return true;
}

void ThumbnailPreview::SetStyle(const PreviewStyle& style) {
    style_ = style;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void ThumbnailPreview::Destroy() {
    if (thumbnail_) {
        DwmUnregisterThumbnail(thumbnail_);
        thumbnail_ = nullptr;
    }
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    source_ = nullptr;
    sourceActive_ = false;
    sourceMinimized_ = false;
    visible_ = false;
}

SIZE ThumbnailPreview::Begin(HWND source) {
    if (!hwnd_ || !source || !IsWindow(source)) return SIZE{0, 0};

    if (source_ != source || !thumbnail_) {
        if (thumbnail_) {
            DwmUnregisterThumbnail(thumbnail_);
            thumbnail_ = nullptr;
        }

        const HRESULT registered = DwmRegisterThumbnail(hwnd_, source, &thumbnail_);
        lastResult_ = static_cast<long long>(registered);
        if (FAILED(registered) || !thumbnail_) {
            thumbnail_ = nullptr;
            source_ = nullptr;
            sourceSize_ = {0, 0};
            ++refusals_;
            return SIZE{0, 0};
        }
        source_ = source;

        // The card's header: the window's own title and icon, so the card says WHICH window this is and
        // not merely what it looks like. The icon is borrowed from the window and never destroyed here;
        // SendMessageTimeout rather than SendMessage because a hung window must not hang the bar.
        title_[0] = L'\0';
        GetWindowTextW(source, title_, 256);
        DWORD_PTR result = 0;
        if (!SendMessageTimeoutW(source, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 100, &result)) {
            result = 0;
        }
        icon_ = reinterpret_cast<HICON>(result);
        if (!icon_ &&
            SendMessageTimeoutW(source, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG, 100, &result)) {
            icon_ = reinterpret_cast<HICON>(result);
        }
        if (!icon_) icon_ = reinterpret_cast<HICON>(GetClassLongPtrW(source, GCLP_HICON));
        if (!icon_) icon_ = reinterpret_cast<HICON>(GetClassLongPtrW(source, GCLP_HICONSM));
    }

    sourceActive_ = GetForegroundWindow() == source;
    sourceMinimized_ = IsIconic(source) != FALSE;

    // What the COMPOSITOR believes the source is: this is the number it will draw from, and the shape the
    // card is built around. It is NOT the window's own rectangle, and taking the aspect from there is what
    // squashed one preview into a wide flat strip: for some windows GetWindowRect reports something much
    // shorter than the thumbnail, and the compositor stretches its picture to fill whatever rectangle it
    // is given (reported from the user's own screen: "从第七个预览框就变成这样了").
    sourceSize_ = {0, 0};
    DwmQueryThumbnailSourceSize(thumbnail_, &sourceSize_);
    return sourceSize_;
}

bool ThumbnailPreview::Place(const ThumbnailPlacement& placement) {
    if (!hwnd_ || !thumbnail_) return false;
    if (!placement.valid) return false;

    card_ = placement.card;
    header_ = placement.header;
    slot_ = placement.slot;
    picture_ = placement.picture;
    footer_ = placement.footer;
    scale_ = placement.scale > 0.0f ? placement.scale : 1.0f;

    const int cardWidth = static_cast<int>(card_.right - card_.left);
    const int cardHeight = static_cast<int>(card_.bottom - card_.top);

    // ROUNDED, by a window region rather than by a layered window: DWM refuses to register a thumbnail
    // whose destination is layered, so the corners are cut out of the window's own shape instead. The
    // region clips the thumbnail exactly as it clips everything else this window draws.
    const int diameter = DipToPx(kThumbnailRadiusDip * 2.0f, scale_);
    HRGN region = CreateRoundRectRgn(0, 0, cardWidth + 1, cardHeight + 1, diameter, diameter);
    if (region) SetWindowRgn(hwnd_, region, TRUE);   // the window owns the region from here

    SetWindowPos(hwnd_, HWND_TOPMOST, card_.left, card_.top, cardWidth, cardHeight,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    visible_ = true;

    // The card's own furniture is painted by us; the picture is painted by the compositor. Repainted on
    // every placement because the title and the icon are the window's and the window can change.
    InvalidateRect(hwnd_, nullptr, TRUE);

    // The whole source window, into the whole client area: the aspect ratio and the size were decided
    // by ThumbnailBoxFor, so the two must not disagree here as well.
    //
    // rcSource IS STATED EXPLICITLY, and that is the whole of a defect that cost several hours: an EMPTY
    // rcSource is documented as "the whole source window", and on this machine a thumbnail registered
    // with one draws NOTHING -- registration returns S_OK, the update returns S_OK,
    // DwmQueryThumbnailSourceSize answers 626x473, the destination window is visible at the right
    // rectangle, and not one pixel appears (tools/thumbprobe measured both ways: empty source -> the
    // window underneath shows through, explicit source -> the window itself). The rectangle the
    // compositor reports is what it will draw, so that is what is handed back to it.
    DWM_THUMBNAIL_PROPERTIES properties = {};
    properties.dwFlags =
        DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_OPACITY | DWM_TNP_VISIBLE;
    // In CLIENT coordinates, which is what DWM asks for: the picture's place inside the card.
    properties.rcDestination = {
        picture_.left - card_.left, picture_.top - card_.top,
        picture_.right - card_.left, picture_.bottom - card_.top};
    properties.rcSource = {0, 0, sourceSize_.cx, sourceSize_.cy};
    properties.opacity = 255;
    properties.fVisible = TRUE;
    // DWM_TNP_SOURCECLIENTAREAONLY is deliberately NOT set: the source rectangle above is the
    // compositor's own answer for the whole window, frame included, which is what a preview is meant to
    // look like.
    const HRESULT updated = DwmUpdateThumbnailProperties(thumbnail_, &properties);
    lastResult_ = static_cast<long long>(updated);
    if (FAILED(updated)) {
        // The registration was granted and the properties were refused, which is a different fault from
        // "the compositor will not preview this window" and would otherwise look identical: a window on
        // screen with nothing in it.
        ++refusals_;
        ShowWindow(hwnd_, SW_HIDE);
        visible_ = false;
        return false;
    }
    return true;
}

void ThumbnailPreview::PaintCard(HDC dc) {
    if (!dc) return;

    RECT client = {};
    GetClientRect(hwnd_, &client);

    const int pictureTop = static_cast<int>(picture_.top - card_.top);
    const int pictureBottom = static_cast<int>(picture_.bottom - card_.top);
    const int pictureLeft = static_cast<int>(picture_.left - card_.left);
    const int pictureRight = static_cast<int>(picture_.right - card_.left);

    // DWM owns the picture rectangle and paints it into this HWND. Every fill below stops at one of its
    // four edges; the header and footer and the letterbox around the picture are ours. Keeping those
    // regions separate is what lets a richer shell sit around a live thumbnail without covering it.
    const int slotTop = static_cast<int>(slot_.top - card_.top);
    const int slotBottom = static_cast<int>(slot_.bottom - card_.top);
    const int slotLeft = static_cast<int>(slot_.left - card_.left);
    const int slotRight = static_cast<int>(slot_.right - card_.left);
    const int footerTop = static_cast<int>(footer_.top - card_.top);

    const RECT topBand = {client.left, client.top, client.right, slotTop};
    const RECT bottomBand = {client.left, slotBottom, client.right, footerTop};
    const RECT leftBand = {client.left, slotTop, slotLeft, slotBottom};
    const RECT rightBand = {slotRight, slotTop, client.right, slotBottom};
    const RECT footerBand = {client.left, footerTop, client.right, client.bottom};

    FillVerticalGradient(dc, topBand, style_.fill, MixColour(style_.fill, style_.fillBottom, 0.55f));
    FillVerticalGradient(dc, bottomBand, MixColour(style_.fill, style_.fillBottom, 0.55f),
                         style_.fillBottom);
    FillVerticalGradient(dc, leftBand, MixColour(style_.fill, style_.fillBottom, 0.55f),
                         style_.fillBottom);
    FillVerticalGradient(dc, rightBand, MixColour(style_.fill, style_.fillBottom, 0.55f),
                         style_.fillBottom);
    FillVerticalGradient(dc, footerBand, style_.fillBottom, style_.fill);

    // The letterbox belongs to the screen plate, not the card. The actual image rectangle remains
    // untouched; these four pieces only cover the space around it.
    const RECT plateTop = {slotLeft, slotTop, slotRight, pictureTop};
    const RECT plateBottom = {slotLeft, pictureBottom, slotRight, slotBottom};
    const RECT plateLeft = {slotLeft, pictureTop, pictureLeft, pictureBottom};
    const RECT plateRight = {pictureRight, pictureTop, slotRight, pictureBottom};
    FillRectColour(dc, plateTop, style_.panel);
    FillRectColour(dc, plateBottom, style_.panel);
    FillRectColour(dc, plateLeft, style_.panel);
    FillRectColour(dc, plateRight, style_.panel);

    // The picture gets a recessed one-pixel surround, and each major band gets a restrained separator.
    const RECT screenRimTop = {slotLeft - 1, pictureTop - 1, slotRight + 1, pictureTop};
    const RECT screenRimBottom = {slotLeft - 1, pictureBottom, slotRight + 1, pictureBottom + 1};
    const RECT screenRimLeft = {slotLeft - 1, pictureTop, pictureLeft, pictureBottom};
    const RECT screenRimRight = {pictureRight, pictureTop, slotRight + 1, pictureBottom};
    FillRectColour(dc, screenRimTop, style_.rim);
    FillRectColour(dc, screenRimBottom, style_.rim);
    FillRectColour(dc, screenRimLeft, style_.rim);
    FillRectColour(dc, screenRimRight, style_.rim);

    const RECT headerDivider = {client.left + DipToPx(10.0f, scale_), slotTop - 1,
                                client.right - DipToPx(10.0f, scale_), slotTop};
    const RECT footerDivider = {client.left + DipToPx(10.0f, scale_), footerTop,
                                client.right - DipToPx(10.0f, scale_), footerTop + 1};
    FillRectColour(dc, headerDivider, MixColour(style_.rim, style_.fill, 0.35f));
    FillRectColour(dc, footerDivider, MixColour(style_.rim, style_.fill, 0.35f));

    // The inner light and the outer hairline are what stop the large dark surface from reading as a flat
    // rectangle. Both are drawn inside the window region, so the large rounded corners stay clean.
    const int diameter = DipToPx(kThumbnailRadiusDip * 2.0f, scale_);
    const RECT topHighlight = {DipToPx(22.0f, scale_), 1,
                               client.right - DipToPx(22.0f, scale_), 2};
    FillRectColour(dc, topHighlight, style_.highlight);
    HPEN pen = CreatePen(PS_SOLID, 1, style_.rim);
    if (pen) {
        HGDIOBJ previousPen = SelectObject(dc, pen);
        HGDIOBJ previousBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, 0, 0, client.right, client.bottom, diameter, diameter);
        SelectObject(dc, previousBrush);
        SelectObject(dc, previousPen);
        DeleteObject(pen);
    }

    // The top row says which window this is. The icon is materially larger than the old 16 px mark, and
    // the status chip is driven only by the real foreground/minimised state captured when Begin prepared
    // the thumbnail.
    const int iconSize = DipToPx(28.0f, scale_);
    const int iconLeft = DipToPx(12.0f, scale_);
    const int iconTop = DipToPx(kThumbnailPadDip, scale_) +
                        (DipToPx(kThumbnailHeaderDip, scale_) - iconSize) / 2;
    if (icon_) {
        DrawIconEx(dc, iconLeft, iconTop, icon_, iconSize, iconSize, 0, nullptr, DI_NORMAL);
    } else {
        const RECT fallback = {iconLeft, iconTop, iconLeft + iconSize, iconTop + iconSize};
        FillRectColour(dc, fallback, MixColour(style_.accent, style_.fill, 0.18f));
    }

    SetBkMode(dc, TRANSPARENT);
    const wchar_t* status = sourceActive_ ? L"当前" : (sourceMinimized_ ? L"已最小化" : nullptr);
    int chipLeft = client.right - DipToPx(12.0f, scale_);
    if (status) {
        HFONT chipFont = CreateFontW(-DipToPx(11.0f, scale_), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
                                     FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Text");
        if (chipFont) {
            HGDIOBJ previous = SelectObject(dc, chipFont);
            SIZE extent = {};
            GetTextExtentPoint32W(dc, status, static_cast<int>(wcslen(status)), &extent);
            const int chipWidth = extent.cx + DipToPx(18.0f, scale_);
            const int chipHeight = DipToPx(22.0f, scale_);
            chipLeft = client.right - DipToPx(12.0f, scale_) - chipWidth;
            const int chipTop = DipToPx(kThumbnailPadDip, scale_) +
                                (DipToPx(kThumbnailHeaderDip, scale_) - chipHeight) / 2;
            HBRUSH chipBrush = CreateSolidBrush(MixColour(style_.accent, RGB(255, 255, 255), 0.06f));
            HPEN chipPen = CreatePen(PS_SOLID, 1, MixColour(style_.accent, RGB(255, 255, 255), 0.24f));
            HGDIOBJ oldBrush = SelectObject(dc, chipBrush);
            HGDIOBJ oldPen = SelectObject(dc, chipPen);
            RoundRect(dc, chipLeft, chipTop, chipLeft + chipWidth, chipTop + chipHeight,
                      chipHeight, chipHeight);
            SelectObject(dc, oldPen);
            SelectObject(dc, oldBrush);
            DeleteObject(chipPen);
            DeleteObject(chipBrush);
            SetTextColor(dc, RGB(255, 255, 255));
            RECT chipText = {chipLeft, chipTop, chipLeft + chipWidth, chipTop + chipHeight};
            DrawTextW(dc, status, -1, &chipText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, previous);
            DeleteObject(chipFont);
        }
    }

    const wchar_t* shownTitle = title_[0] != L'\0' ? title_ : L"窗口预览";
    const int titleLeft = iconLeft + iconSize + DipToPx(10.0f, scale_);
    const int titleRight = chipLeft - DipToPx(10.0f, scale_);
    if (titleLeft < titleRight) {
        HFONT titleFont = CreateFontW(
            -DipToPx(15.0f, scale_), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
            L"Segoe UI Variable Display");
        HGDIOBJ previous = titleFont ? SelectObject(dc, titleFont) : nullptr;
        SetTextColor(dc, style_.ink);
        RECT titleRect = {titleLeft, DipToPx(kThumbnailPadDip, scale_), titleRight, slotTop};
        DrawTextW(dc, shownTitle, -1, &titleRect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (titleFont) {
            SelectObject(dc, previous);
            DeleteObject(titleFont);
        }
    }

    // The footer carries only information the card can stand behind: the DWM image is live, and the icon
    // above toggles the previewed window. It is a compact action row, not fabricated capture telemetry.
    const wchar_t* action = sourceActive_ ? L"单击收起窗口"
                            : sourceMinimized_ ? L"单击恢复窗口"
                                              : L"单击切换窗口";
    const int footerTextTop = footerTop + DipToPx(1.0f, scale_);
    const int footerTextBottom = client.bottom;
    HFONT footerFont = CreateFontW(-DipToPx(11.0f, scale_), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Text");
    if (footerFont) {
        HGDIOBJ previous = SelectObject(dc, footerFont);
        HBRUSH dot = CreateSolidBrush(style_.accent);
        const int dotSize = DipToPx(4.0f, scale_);
        const int dotLeft = DipToPx(12.0f, scale_);
        const int dotTop = footerTextTop + (footerTextBottom - footerTextTop - dotSize) / 2;
        const RECT dotRect = {dotLeft, dotTop, dotLeft + dotSize, dotTop + dotSize};
        FillRect(dc, &dotRect, dot);
        DeleteObject(dot);

        SetTextColor(dc, style_.mutedInk);
        RECT live = {dotLeft + dotSize + DipToPx(7.0f, scale_), footerTextTop,
                     client.right / 2, footerTextBottom};
        DrawTextW(dc, L"实时预览", -1, &live, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT hint = {client.right / 2, footerTextTop, client.right - DipToPx(12.0f, scale_),
                     footerTextBottom};
        DrawTextW(dc, action, -1, &hint, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, previous);
        DeleteObject(footerFont);
    }
}

void ThumbnailPreview::Hide() {
    if (!hwnd_ || !visible_) return;
    ShowWindow(hwnd_, SW_HIDE);
    visible_ = false;
}

LRESULT CALLBACK ThumbnailPreview::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam,
                                               LPARAM lParam) {
    switch (message) {
        // The object, kept for every later message: the paint handler is a member function and this
        // window has no other way to reach it. (lpCreateParams is the `this` CreateWindowEx was given.)
        case WM_NCCREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create ? create->lpCreateParams : nullptr));
            break;
        }
        // Nothing is erased here: the paint below draws the card's furniture, and the picture must NOT be
        // filled -- the compositor draws the thumbnail into those pixels and this window shares them.
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps = {};
            HDC dc = BeginPaint(hwnd, &ps);
            auto* self =
                reinterpret_cast<ThumbnailPreview*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (self) self->PaintCard(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        // Click-through by message as well as by style, so a click that lands on the preview during the
        // frame the style is being applied still reaches what is underneath.
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace island
