#include "stdafx.h"
#include "FontPreview.h"

namespace {

// The preview shows the face name in the dialog's own font followed by these digits in the
// selected face: the name stays readable at a fixed size while the digits show the actual
// glyphs the overlay will draw.
constexpr LPCTSTR kDigits = TEXT("1234567890");

// Minimum separation between the name and the right-aligned digits, in average character
// widths of the run it follows.
constexpr int kGapChars = 1;

// Share of the control's height the digits may occupy, leaving a little room inside the
// border rather than butting up against it.
constexpr int kFillPercent = 90;
constexpr int kPercentScale = 100;

constexpr UINT_PTR kSubclassId = 1;

// The font that renders the digits run in the chosen face, sized to fit whatever width the
// face name leaves free; owned here, rebuilt by every Update, freed by Cleanup. Null when the
// name alone fills the control, in which case the preview shows the name on its own.
HFONT g_digitsFont;

// The box the two runs lay out in: the control's client area less an inset on each side, so
// the text keeps clear of the border instead of butting up against it. The inset is the fixed
// window-frame metric taken at the control's DPI -- the same measure the overlay insets its
// separators by -- so the spacing scales with the display like the rest of the UI.
[[nodiscard]] bool ContentRect(HWND preview, RECT& content) {
    if (!GetClientRect(preview, &content)) return false;

    const int inset =
        GetSystemMetricsForDpi(SM_CXFIXEDFRAME, WinAPI::Window::GetDpi(preview));
    content.left  += inset;
    content.right -= inset;
    return true;
}

// One measured text run: the advance box GDI lays it out in, the ink that leans outside that
// box, and the separation to keep after it. The distinction matters because an italic or
// script face paints past its advance width -- the leading glyph can reach left of the origin
// and the trailing one past the run's end -- so reserving only the advance clips those glyphs
// against the control's border.
struct TextRun {
    SIZE extent;
    int leftBearing;
    int rightBearing;
    int gap;

    // The width the glyphs actually cover, overhangs included.
    [[nodiscard]] int InkWidth() const { return leftBearing + extent.cx + rightBearing; }
};

// The font the preview control itself carries -- the dialog font, which renders the name.
[[nodiscard]] HFONT NameFont(HWND preview) {
    HFONT font = GetWindowFont(preview);
    if (!font) font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    return font;
}

// Ink overhangs of 'text' at the DC's current font, as positive pixel counts. Only the outer
// glyphs can push ink outside the run: an interior overhang is absorbed by the neighbouring
// character's advance. GetCharABCWidths reports the bearings as a negative A (ink left of the
// origin) or C (ink past the advance), and serves TrueType faces only -- a bitmap face
// synthesises its slant and already carries it in the advance GetTextExtentPoint32 returns,
// so leaving both bearings at zero is the right answer there.
void MeasureBearings(HDC dc, LPCTSTR text, int length, TextRun& run) {
    run.leftBearing = run.rightBearing = 0;
    if (length <= 0) return;

    ABC abc;
    if (GetCharABCWidths(dc, text[0], text[0], &abc) && abc.abcA < 0)
        run.leftBearing = -abc.abcA;

    const TCHAR last = text[length - 1];
    if (GetCharABCWidths(dc, last, last, &abc) && abc.abcC < 0)
        run.rightBearing = -abc.abcC;
}

// Measure 'text' with 'font' selected into 'dc'. Extent, bearings and gap all come from the
// same selection, so each run carries the layout figures appropriate to its own size.
[[nodiscard]] bool MeasureRun(HDC dc, HFONT font, LPCTSTR text, TextRun& run) {
    const HGDIOBJ previous = SelectObject(dc, font);
    const bool selected = previous && previous != HGDI_ERROR;
    ASSERT(selected);
    if (!selected) return false;

    const int length = lstrlen(text);
    TEXTMETRIC tm;
    const bool measured =
        GetTextMetrics(dc, &tm) && GetTextExtentPoint32(dc, text, length, &run.extent);
    ASSERT(measured);
    if (measured) MeasureBearings(dc, text, length, run);
    VERIFY(SelectObject(dc, previous));
    if (!measured) return false;

    run.gap = tm.tmAveCharWidth * kGapChars;
    return true;
}

// The 'face' at the largest height whose digits still fit 'maxWidth' by 'maxHeight'. Returns
// null when even a single pixel of height overflows -- i.e. the name leaves no room.
[[nodiscard]] HFONT CreateFittedDigitsFont(HDC dc, const LOGFONT& face, int maxWidth,
                                           int maxHeight) {
    if (maxWidth <= 0 || maxHeight <= 0) return nullptr;

    LOGFONT lf;
    MoveMemory(&lf, &face, sizeof(lf));
    lf.lfWidth = 0;

    int bestHeight = 0;
    int low = 1;
    int high = maxHeight;
    while (low <= high) {
        const int height = low + (high - low) / 2;
        lf.lfHeight = -height;
        const HFONT candidate = CreateFontIndirect(&lf);
        ASSERT(candidate);
        if (!candidate) break;

        TextRun run = { { 0, 0 }, 0, 0, 0 };
        const bool fits = MeasureRun(dc, candidate, kDigits, run) &&
                          run.InkWidth() <= maxWidth && run.extent.cy <= maxHeight;
        VERIFY(DeleteObject(candidate));

        if (fits) {
            bestHeight = height;
            low = height + 1;
        } else {
            high = height - 1;
        }
    }

    if (!bestHeight) return nullptr;
    lf.lfHeight = -bestHeight;
    return CreateFontIndirect(&lf);
}

// Draw one run inside 'box' with the given DrawText alignment flags. Each run is centred on
// its own metrics, so a small name and a large digits run each sit optically centred in the
// control rather than being tied to a shared baseline. DT_NOCLIP is essential: DrawText
// otherwise clips to the very rect it lays the text out in, so the inset the caller applies
// to make room for an italic overhang would clip that overhang away instead of showing it.
// The paint DC is already clipped to the client area, so nothing can spill onto the border.
void DrawRun(HDC dc, HFONT font, LPCTSTR text, const RECT& box, UINT align) {
    const HGDIOBJ previous = SelectObject(dc, font);
    const bool selected = previous && previous != HGDI_ERROR;
    ASSERT(selected);
    if (!selected) return;

    RECT rc = box;
    VERIFY(DrawText(dc, text, -1, &rc,
                    align | DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER | DT_NOCLIP |
                        DT_END_ELLIPSIS));
    VERIFY(SelectObject(dc, previous));
}

// Paint the preview: the face name in the control's own (dialog) font at the left, and the
// tip digits in the fitted selected face at the right, each vertically centred. The
// background brush comes from the parent's WM_CTLCOLORSTATIC response, exactly as the stock
// static control obtains it -- which also sets the DC's text and background colors.
void OnPaint(HWND preview) {
    PAINTSTRUCT ps;
    const HDC dc = BeginPaint(preview, &ps);
    ASSERT(dc);
    if (!dc) return;

    RECT rc;
    VERIFY(GetClientRect(preview, &rc));

    const HBRUSH brush = reinterpret_cast<HBRUSH>(
        SendMessage(GetParent(preview), WM_CTLCOLORSTATIC,
                    reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(preview)));
    if (brush) VERIFY(FillRect(dc, &rc, brush));
    SetBkMode(dc, TRANSPARENT);

    // Not brace-initialized: a zeroed buffer this large makes the compiler emit a memset
    // call, which this CRT-less build does not provide. GetWindowText terminates it.
    TCHAR name[LF_FACESIZE];
    name[0] = TEXT('\0');
    GetWindowText(preview, name, LF_FACESIZE);

    const HFONT nameFont = NameFont(preview);
    TextRun nameRun = { { 0, 0 }, 0, 0, 0 };
    RECT content;
    if (ContentRect(preview, content) && MeasureRun(dc, nameFont, name, nameRun)) {
        TextRun digitsRun = { { 0, 0 }, 0, 0, 0 };
        const bool showDigits =
            g_digitsFont && MeasureRun(dc, g_digitsFont, kDigits, digitsRun);

        // Position each run by its ink rather than by its advance box: nudge the name right
        // by whatever its first glyph leans left of the origin, and pull the digits left by
        // whatever their last glyph leans past the advance, so both sit inside the content
        // box. Without digits the name is free to use, and ellipsize into, the whole box.
        RECT nameBox = content;
        nameBox.left += nameRun.leftBearing;

        RECT digitsBox = content;
        if (showDigits) {
            digitsBox.right -= digitsRun.rightBearing;
            nameBox.right = digitsBox.right - digitsRun.extent.cx - nameRun.gap;
        } else {
            nameBox.right -= nameRun.rightBearing;
        }

        DrawRun(dc, nameFont, name, nameBox, DT_LEFT);
        if (showDigits) DrawRun(dc, g_digitsFont, kDigits, digitsBox, DT_RIGHT);
    }

    VERIFY(EndPaint(preview, &ps));
}

LRESULT CALLBACK SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                              DWORD_PTR ref);

void OnNcDestroy(HWND preview) {
    VERIFY(RemoveWindowSubclass(preview, SubclassProc, kSubclassId));
    FORWARD_WM_NCDESTROY(preview, DefSubclassProc);
}

// Take over the static's rendering: OnPaint fills the whole client area, so the default erase
// is suppressed to keep the two-font line from flickering on every repaint.
LRESULT CALLBACK SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                              UINT_PTR /*id*/, DWORD_PTR /*ref*/) {
    switch (msg) {
        HANDLE_MSG(hwnd, WM_PAINT, OnPaint);
        HANDLE_MSG(hwnd, WM_NCDESTROY, OnNcDestroy);
        case WM_ERASEBKGND: return TRUE;
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

} // namespace

namespace FontPreview {

void Init(HWND preview) {
    VERIFY(SetWindowSubclass(preview, SubclassProc, kSubclassId, 0));
}

void Update(HWND preview, LPCTSTR name, const LOGFONT& face) {
    VERIFY(SetWindowText(preview, name));

    WinAPI::GdiObject::Delete(g_digitsFont);

    RECT rc;
    const HDC dc = GetDC(preview);
    ASSERT(dc);
    if (dc && ContentRect(preview, rc)) {
        TextRun nameRun = { { 0, 0 }, 0, 0, 0 };
        if (MeasureRun(dc, NameFont(preview), name, nameRun))
            g_digitsFont = CreateFittedDigitsFont(
                dc, face, (rc.right - rc.left) - nameRun.InkWidth() - nameRun.gap,
                MulDiv(rc.bottom - rc.top, kFillPercent, kPercentScale));
    }
    if (dc) VERIFY(ReleaseDC(preview, dc) == 1);

    VERIFY(RedrawWindow(preview, nullptr, nullptr, RDW_INVALIDATE));
}

void Cleanup() {
    WinAPI::GdiObject::Delete(g_digitsFont);
}

} // namespace FontPreview
