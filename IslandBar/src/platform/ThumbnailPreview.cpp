// =====================================================================
//  ThumbnailPreview.cpp
// =====================================================================
#include "platform/ThumbnailPreview.h"

#include "domain/Geometry.h"
#include "core/Log.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>

#pragma comment(lib, "dwmapi.lib")

namespace island {
namespace {

constexpr wchar_t kThumbnailClassName[] = L"IslandBarThumbnail";

} // namespace

ThumbnailPlacement ThumbnailBoxFor(int sourceWidthPx, int sourceHeightPx, const RECT& iconBoxPx,
                                   const RECT& monitorPx, float scale) {
    ThumbnailPlacement placement;
    if (scale <= 0.0f) scale = 1.0f;

    const float slotWidth = kThumbnailMaxWidthDip * scale;
    const float slotHeight = kThumbnailMaxHeightDip * scale;
    const float pad = kThumbnailPadDip * scale;
    const float title = kThumbnailTitleDip * scale;

    // THE CARD IS ONE SIZE, whatever window it is showing: one margin, one title row, one slot for the
    // picture. Every preview in a row is then the same object, which is what the shell's own previews are
    // and what "现在预览框 大小也不一样" asked for.
    const float cardWidth = slotWidth + pad * 2.0f;
    const float cardHeight = slotHeight + pad * 2.0f + title;

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

    // The picture is CENTRED in the slot, so the letterbox is even on both sides -- and ROUNDED rather
    // than truncated, because truncating a half-pixel moved the letterbox a pixel to one side and the
    // self-test's own "even top to bottom" caught it (6 above against 8 below).
    const float slotLeft = static_cast<float>(placement.card.left) + pad;
    const float slotTop = static_cast<float>(placement.card.top) + pad + title;
    placement.picture.left = std::lround(slotLeft + (slotWidth - width) * 0.5f);
    placement.picture.top = std::lround(slotTop + (slotHeight - height) * 0.5f);
    placement.picture.right = placement.picture.left + std::lround(width);
    placement.picture.bottom = placement.picture.top + std::lround(height);

    placement.valid = (placement.card.right > placement.card.left) &&
                      (placement.card.bottom > placement.card.top) &&
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
        if (!SendMessageTimeoutW(source, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG, 100, &result)) {
            result = 0;
        }
        icon_ = reinterpret_cast<HICON>(result);
        if (!icon_ &&
            SendMessageTimeoutW(source, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 100, &result)) {
            icon_ = reinterpret_cast<HICON>(result);
        }
        if (!icon_) icon_ = reinterpret_cast<HICON>(GetClassLongPtrW(source, GCLP_HICONSM));
    }

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
    picture_ = placement.picture;

    const int cardWidth = static_cast<int>(card_.right - card_.left);
    const int cardHeight = static_cast<int>(card_.bottom - card_.top);

    // ROUNDED, by a window region rather than by a layered window: DWM refuses to register a thumbnail
    // whose destination is layered, so the corners are cut out of the window's own shape instead. The
    // region clips the thumbnail exactly as it clips everything else this window draws.
    const int diameter = static_cast<int>(kThumbnailRadiusDip * 2.0f);
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

    // EVERYTHING BUT THE PICTURE. Filling the whole client would paint OVER the thumbnail, which the
    // compositor draws into this same window: the four bands around the picture are what this card owns,
    // and the picture belongs to the compositor.
    const int pictureTop = static_cast<int>(picture_.top - card_.top);
    const int pictureBottom = static_cast<int>(picture_.bottom - card_.top);
    const int pictureLeft = static_cast<int>(picture_.left - card_.left);
    const int pictureRight = static_cast<int>(picture_.right - card_.left);

    // The card's body, painted from the style the app layer supplied. It used to be a hard-coded
    // RGB(18,18,22) for every theme; the glass theme now hands it the island's own refracted average, so
    // the card is the island's glass colour rather than a black or white plate.
    {
        HBRUSH card = CreateSolidBrush(style_.fill);
        const RECT bands[4] = {
            {client.left, client.top, client.right, pictureTop},                  // the header, as wide as the card
            {client.left, pictureBottom, client.right, client.bottom},            // below the picture
            {client.left, pictureTop, pictureLeft, pictureBottom},                // left margin
            {pictureRight, pictureTop, client.right, pictureBottom},              // right margin
        };
        for (const RECT& band : bands) {
            if (band.right > band.left && band.bottom > band.top) FillRect(dc, &band, card);
        }
        DeleteObject(card);
    }

    // The hairline under the header: without it the title floats over the picture when the window's own
    // content starts right at its top edge.
    HBRUSH line = CreateSolidBrush(style_.rim);
    const RECT divider = {client.left, pictureTop - 1, client.right, pictureTop};
    if (divider.bottom > divider.top) FillRect(dc, &divider, line);
    DeleteObject(line);

    // The card's own edge: the same idea as the island's rim band, in the one stroke GDI gives. It is
    // drawn INSIDE the region the window already has, so the rim cannot stick out of the rounded corner.
    {
        const int diameter = static_cast<int>(kThumbnailRadiusDip * 2.0f);
        HPEN pen = CreatePen(PS_SOLID, 2, style_.rim);
        if (pen) {
            HGDIOBJ previousPen = SelectObject(dc, pen);
            HGDIOBJ previousBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            RoundRect(dc, 1, 1, client.right - 1, client.bottom - 1, diameter, diameter);
            SelectObject(dc, previousBrush);
            SelectObject(dc, previousPen);
            DeleteObject(pen);
        }
    }

    // The icon and the title, so the card says which window this is.
    const int iconSize = 16;
    int textLeft = 10;
    if (icon_) {
        DrawIconEx(dc, 10, (pictureTop - iconSize) / 2, icon_, iconSize, iconSize, 0, nullptr,
                   DI_NORMAL);
        textLeft = 10 + iconSize + 8;
    }

    if (title_[0] != L'\0' && textLeft < static_cast<int>(client.right) - 16) {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, style_.ink);
        HFONT font = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH, L"Segoe UI Variable Text");
        HGDIOBJ previous = font ? SelectObject(dc, font) : nullptr;
        RECT text = {textLeft, 0, static_cast<LONG>(client.right) - 12, pictureTop};
        // DT_END_ELLIPSIS rather than a hard cut: a title that does not fit ends in "...", which reads as a
        // long title, where a cut reads as a broken card.
        DrawTextW(dc, title_, -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (font) {
            SelectObject(dc, previous);
            DeleteObject(font);
        }
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
