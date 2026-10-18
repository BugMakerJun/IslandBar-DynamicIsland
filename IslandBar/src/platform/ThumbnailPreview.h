// =====================================================================
//  ThumbnailPreview.h
//  A live thumbnail of a window, shown under the bar while the pointer rests
//  on that window's icon.
//
//  WHY A SECOND WINDOW. A DWM thumbnail is not a bitmap this program can
//  fetch: the compositor draws the source window INTO an HWND of ours, on its
//  own schedule. So the preview is a window of its own -- a topmost, never
//  activated, click-through popup placed under the bar -- and the bar's own
//  surface is not involved at all.
//
//  Click-through is deliberate. The preview is a reminder, not a control: the
//  icon above it is what opens the window, and the preview must not be able to
//  swallow a click meant for the desktop underneath or for the bar above.
//
//  Minimised windows have nothing to show (DWM renders them empty), which is
//  also what the shell's own taskbar does. Nothing here special-cases that.
// =====================================================================
#pragma once

#include <windows.h>

// HTHUMBNAIL, the handle a DWM thumbnail registration answers with. It is a member of this class, and
// the header needs the declaration for it -- leaving it out makes the compiler read the member's NAME as
// a specifier and complain about that instead ("C3646: 'thumbnail_': unknown override specifier"), which
// points nowhere near the missing include. The same lesson AppBar.h records for APPBARDATA.
#include <dwmapi.h>

namespace island {

// The largest a preview may be, in DIPs.
//
// A preview says WHICH window this is; it is not a second desktop. Past this size it covers the
// application the user is working in, which is the opposite of the point.
//
// THE CARD IS THIS SIZE FOR EVERY WINDOW, and the picture is fitted inside it. A card that took its
// shape from the window made every preview a different size, which is what "现在预览框 大小也不一样"
// reported: a row of previews should be one shape, the way the shell's own are, and the window's
// proportions belong inside the picture rather than in the frame around it.
inline constexpr float kThumbnailMaxWidthDip  = 260.0f;
inline constexpr float kThumbnailMaxHeightDip = 160.0f;

// How far below the bar's own underside the preview hangs.
inline constexpr float kThumbnailGapDip = 10.0f;

// The card the picture stands in: a title row, a margin, and a rounded corner.
//
// The preview is a CARD rather than a bare rectangle of someone else's window, because a raw bitmap
// floating under the bar reads as a rendering fault (reported as such: "从第七个预览框就变成这样了"
// was a window squeezed into a strip, with nothing around it to say what it was). The card says what
// it is: the application's icon, the window's title, and the picture inside a frame.
inline constexpr float kThumbnailPadDip   = 8.0f;
inline constexpr float kThumbnailTitleDip = 22.0f;
inline constexpr float kThumbnailRadiusDip = 12.0f;

// Where a preview goes, in SCREEN PIXELS: the CARD (this window's own rectangle) and the PICTURE
// inside it (the rectangle the compositor draws the thumbnail into).
//
// Pure, so its placement can be asserted without a compositor, a window list or a screenshot. The
// properties that matter are all statements about arithmetic: the picture keeps the source's aspect
// ratio, the card wraps it with its title row and margin, the card is centred under the icon it
// belongs to, it hangs below the bar, it never leaves the monitor, and it is never degenerate.
//
// `sourceWidthPx`/`sourceHeightPx` are what the COMPOSITOR reports for the source
// (DwmQueryThumbnailSourceSize), NOT the window's rectangle, and the difference is a defect this
// function used to have: it took the aspect from GetWindowRect, and the compositor stretches the
// thumbnail to fill the rectangle it is given -- so a window whose reported rectangle and whose
// thumbnail differed came out SQUASHED into a strip (a 1226x701 window drawn into a 260x44 box,
// reported from the user's own screen). The compositor's own answer cannot disagree with the
// compositor's own drawing.
//
// The card is ONE SIZE whatever those numbers are; only the PICTURE inside it takes their shape, and
// the space left over is card (a letterbox). That is the difference between "the previews are all the
// same object with different contents" and "the previews are all different objects".
struct ThumbnailPlacement {
    RECT card = {};      // the rounded card: what this window is
    RECT picture = {};   // the thumbnail's destination, inside the card
    bool valid = false;
};

ThumbnailPlacement ThumbnailBoxFor(int sourceWidthPx, int sourceHeightPx, const RECT& iconBoxPx,
                                   const RECT& monitorPx, float scale);

// The card's own furniture: its body, its ink and its rim. Supplied by the app layer from the active
// theme, because platform/ must not know about render/Theme. `acrylic` asks for a real blurred backdrop
// behind the card's bands (the glass theme); if the compositor refuses it, the bands fall back to `fill`.
struct PreviewStyle {
    COLORREF fill = RGB(18, 18, 22);
    COLORREF ink = RGB(214, 219, 228);
    COLORREF rim = RGB(56, 56, 66);
    bool acrylic = false;
};

// The preview window itself.
class ThumbnailPreview {
public:
    ThumbnailPreview() = default;
    ~ThumbnailPreview();

    ThumbnailPreview(const ThumbnailPreview&) = delete;
    ThumbnailPreview& operator=(const ThumbnailPreview&) = delete;

    // Registers the window class and creates the hidden popup, OWNED by `owner` (the bar's window).
    //
    // Ownership rather than a plain topmost popup, and that is the whole reason this takes a handle:
    // an owned window is always kept ABOVE its owner, so the bar can never end up covering its own
    // preview. Both being WS_EX_TOPMOST is not enough -- measured on this machine, a topmost popup that
    // was raised before the bar's topmost window stayed under it, and the preview was invisible while
    // every other answer (window exists, right rectangle, visible, thumbnail registered S_OK) said
    // otherwise (HANDOFF 8.51).
    bool Create(HWND owner);
    // The card's colours, from the active theme. Safe to call every frame: it only re-applies the
    // acrylic when its on/off state changes, and repaints.
    void SetStyle(const PreviewStyle& style);
    void Destroy();
    bool Ready() const { return hwnd_ != nullptr; }

    // Points the preview at `source`: registers the thumbnail if this is a new window, and asks the
    // compositor what the source measures.
    //
    // Returns that size, or {0,0} when the compositor will not preview this window (an elevated window, a
    // window on another desktop, a window that has gone). The size is what ThumbnailBoxFor needs, which is
    // why it is a separate call from Place: the card's shape cannot be decided before the compositor has
    // said what it is going to draw.
    SIZE Begin(HWND source);

    // Puts the card where `placement` says and shows the thumbnail in its picture rectangle. This is the
    // only call that moves the window, so the geometry and the window cannot drift apart.
    bool Place(const ThumbnailPlacement& placement);

    // Puts it away. Safe to call when it is already hidden, and called from the frame loop every frame
    // the pointer is not resting on an icon.
    void Hide();

    bool Visible() const { return visible_; }
    HWND Source() const { return source_; }
    HWND Handle() const { return hwnd_; }

    // The last thing the compositor said, for the log: the HRESULT of the update that put the thumbnail
    // on screen (or of the registration that was refused).
    long long LastResult() const { return lastResult_; }

    // What the compositor believes the source window measures, filled in by Begin.
    SIZE SourceSize() const { return sourceSize_; }

    // How many times a registration has been refused since the program started, so a window that
    // cannot be previewed is reported once rather than every frame.
    int Refusals() const { return refusals_; }

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    // Paints the card: everything EXCEPT the picture, which belongs to the compositor.
    void PaintCard(HDC dc);
    // Turns the window's background into the acrylic backdrop, tinted by style_.fill. False when the
    // compositor refuses; the caller then paints the bands with style_.fill instead.
    bool ApplyAcrylic();

    HWND hwnd_ = nullptr;
    HWND source_ = nullptr;
    HTHUMBNAIL thumbnail_ = nullptr;
    SIZE sourceSize_ = {0, 0};
    RECT card_ = {};
    RECT picture_ = {};
    PreviewStyle style_ = {};
    bool acrylicOn_ = false;
    // The window's title and icon for the card's header. The icon is the window's own (borrowed, never
    // destroyed): the same one the icon row shows, so the card and the row agree about what the window is.
    wchar_t title_[256] = {};
    HICON icon_ = nullptr;
    bool visible_ = false;
    long long lastResult_ = 0;
    int refusals_ = 0;
};

} // namespace island
