#include "stdafx.h"
#include "Overlay.h"
#include "Preferences.h"
#include "Taskbar.h"

namespace {

LPCTSTR const kOverlayClass = TEXT("WinNumTipOverlay-") APP_GUID;

// Win+0..9: 10 is the most taskbar buttons we show tips for.
constexpr int kMaxTips = 10;

// Control id used to tell a separator static apart from a number-tip static in
// WM_CTLCOLORSTATIC (tips are transparent; separators are a solid filled line).
constexpr int kSepId = 1;

// Module state. The overlay window lives for the whole "Win held" session so its
// refresh timer keeps polling even when the bar is momentarily empty; only the child
// controls and per-show theme/font/brush are swapped as the taskbar changes.
HWND     g_overlay;
HFONT    g_font;
bool     g_ownFont;    // true when g_font must be DeleteObject'd (not a stock font)
HTHEME   g_theme;
HBRUSH   g_lineBrush;  // solid brush that paints the separator lines
HBRUSH   g_bgBrush;    // solid strip-fill brush, only in invert-colors mode
bool     g_invert;     // true when the strip/number colors are swapped
int      g_bgPart = TBP_BACKGROUNDBOTTOM;  // taskbar background part for the docked edge
COLORREF g_textColor;  // resolved from the theme / system colors in Show
bool     g_active;     // true between Show (Win down) and Hide (Win up)

// Retained so the refresh timer can rebuild the bar in place when the taskbar's
// buttons change while it is shown (e.g. an app minimizes/closes to the notification
// area, which removes its button and would otherwise leave an orphaned number behind).
IUIAutomation* g_uia;
HINSTANCE      g_inst;
int            g_snapN;  // button count captured when the bar was built
RECT           g_snap[kMaxTips];              // button rects captured when the bar was built
// Timer id for the persistent refresh timer; its interval is the user's "refresh interval"
// preference (Preferences::RefreshIntervalMs), re-applied on each Show or Apply.
constexpr UINT_PTR kRefreshTimer = 1;

// Forward decls. Refresh() rebuilds the bar's contents in place for the current
// taskbar; while a session is active the overlay window and its timer persist, so
// updates never flicker and an empty period (0 buttons) can recover when a button
// reappears.
void Refresh();

// Count the current on-screen taskbar buttons (0 when the taskbar is missing or
// obscured), filling 'out'. Shared by the timer's change check and Refresh.
[[nodiscard]] int CollectCurrent(RECT* out, int max);

inline void ArmRefreshTimer() {
    VERIFY(SetTimer(g_overlay, kRefreshTimer, Preferences::RefreshIntervalMs(), nullptr));
}

// Destroy all child controls (number tips + separators) of the overlay window.
void DestroyChildren(HWND hwnd) {
    while (const HWND hwndChild = GetWindow(hwnd, GW_CHILD)) DestroyWindow(hwndChild);
}

// Release the per-show theme/font/brush. The window itself is left intact so it can
// be reused by a refresh; the caller decides whether to destroy the window.
void FreeResources() {
    if (g_theme) { CloseThemeData(g_theme); g_theme = nullptr; }
    if (g_ownFont) WinAPI::GdiObject::Delete(g_font);
    g_font = nullptr;
    g_ownFont = false;
    WinAPI::GdiObject::Delete(g_lineBrush);
    WinAPI::GdiObject::Delete(g_bgBrush);
}

// Compute the tip-size bounds for a taskbar at 'tr' docked on 'edge' at 'dpi': the slim
// default strip thickness and the full taskbar-button thickness, both along the axis
// perpendicular to the taskbar. The button thickness is taken from the taskbar's own
// cross-axis extent (side buttons span its full width; top/bottom buttons are about its
// height), so the renderer and the Preferences dialog agree on 0..100% without the dialog
// needing UI Automation. 'vertical' is set for a side-docked (left/right) taskbar.
void ComputeStripBounds(const RECT& tr, UINT edge, UINT dpi,
                        int& defThick, int& btnThick, bool& vertical) {
    vertical = edge == ABE_LEFT || edge == ABE_RIGHT;
    btnThick = vertical ? (tr.right - tr.left) : (tr.bottom - tr.top);
    const int menu = GetSystemMetricsForDpi(vertical ? SM_CXMENUSIZE : SM_CYMENUSIZE, dpi);
    defThick = (btnThick > 0 && menu > btnThick) ? btnThick : menu;
}


// Paint the bar background: in invert-colors mode a solid fill with the number color;
// otherwise the taskbar theme (falling back to the 3D face color when the theme is
// unavailable, e.g. classic mode). Used from both WM_ERASEBKGND and WM_PRINTCLIENT so
// DrawThemeParentBackground can show it behind the tips.
void PaintBackground(HWND hwnd, HDC hdc) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (g_invert && g_bgBrush) FillRect(hdc, &rc, g_bgBrush);
    else if (g_theme)          DrawThemeBackground(g_theme, hdc, g_bgPart, 0, &rc, nullptr);
    else                       FillRect(hdc, &rc, reinterpret_cast<HBRUSH>(COLOR_3DFACE + 1));
}

// WM_ERASEBKGND / WM_PRINTCLIENT: paint the themed bar background.
BOOL OnEraseBkgnd(HWND hwnd, HDC hdc) {
    PaintBackground(hwnd, hdc);

    return TRUE;
}

void OnPrintClient(HWND hwnd, HDC hdc) {
    PaintBackground(hwnd, hdc);
}

// WM_CTLCOLORSTATIC. A separator static is a thin control filled with the solid line
// brush; the system fills its whole client rect with the returned brush, giving a
// crisp line that contrasts with the bar (no custom drawing needed). A number tip
// instead gets the parent's themed background painted into its DC (via
// DrawThemeParentBackground) and a hollow brush, so its text draws transparently.
HBRUSH OnCtlColorStatic(HWND /*hwnd*/, HDC hdc, HWND child, int /*type*/) {
    if (GetDlgCtrlID(child) == kSepId && g_lineBrush)
        return g_lineBrush;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, g_textColor);
    DrawThemeParentBackground(child, hdc, nullptr);

    return static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
}

// windowsx-style cracker for WM_PRINTCLIENT (not provided by windowsx.h), so it can
// be dispatched with HANDLE_MSG alongside the standard messages.
//   handler signature: void fn(HWND hwnd, HDC hdc)
#define HANDLE_WM_PRINTCLIENT(hwnd, wParam, lParam, fn) ((fn)((hwnd), (HDC)(wParam)), 0L)

// WM_TIMER: re-collect the taskbar buttons and rebuild the bar in place if they
// changed, so numbers stay aligned when a button appears/disappears (or the bar
// recovers from a momentarily empty taskbar) without waiting for the Win key to be
// released. Overlay visibility itself is owned by the message window's poll
// (KeyboardHook::ShouldShow), so this timer only refreshes contents, never hides.
void OnTimer(HWND /*hwnd*/, UINT id) {
    if (id != kRefreshTimer) return;

    RECT cur[kMaxTips];
    const int m = CollectCurrent(cur, kMaxTips);
    bool changed = m != g_snapN;
    for (int i = 0; !changed && i < m; ++i)
        if (!EqualRect(&cur[i], &g_snap[i])) changed = true;
    if (changed) Refresh();   // rebuilds the bar in place (no flicker); recovers from empty
}

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        HANDLE_MSG(hwnd, WM_ERASEBKGND,      OnEraseBkgnd);
        HANDLE_MSG(hwnd, WM_PRINTCLIENT,     OnPrintClient);
        HANDLE_MSG(hwnd, WM_CTLCOLORSTATIC,  OnCtlColorStatic);
        HANDLE_MSG(hwnd, WM_TIMER,           OnTimer);
        default: return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

[[nodiscard]] HWND CreateOverlayWindow(HINSTANCE inst) {
    return CreateWindowEx(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        kOverlayClass, TEXT(""), WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 10, 10, nullptr, nullptr, inst, nullptr);
}

// Apply the "opacity" preference to the overlay window as a whole-window alpha blend
// (WS_EX_LAYERED + LWA_ALPHA covers the window and its child tip/separator statics
// together, so no per-child changes are needed). No-op while the window doesn't exist.
void ApplyOpacityPreference() {
    if (!g_overlay) return;
    const BYTE alpha = static_cast<BYTE>(MulDiv(255, Preferences::OpacityPercent(), 100));
    VERIFY(SetLayeredWindowAttributes(g_overlay, 0, alpha, LWA_ALPHA));
}

// Create a themed STATIC child (a number tip or a separator line) relative to the
// overlay origin, wired up to the shared taskbar font. 'id' distinguishes separators
// (kSepId) from tips (0) for WM_CTLCOLORSTATIC.
void AddStatic(HWND parent, HINSTANCE inst, DWORD style, LPCTSTR text,
               int x, int y, int w, int h, int id = 0) {
    const HWND s = CreateWindowEx(0, TEXT("STATIC"), text,
                            WS_CHILD | WS_VISIBLE | style,
                            x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
    if (s && g_font) SetWindowFont(s, g_font, FALSE);
}

// Choose the number text color. Prefer the taskbar theme's own text color; when the
// theme has none (or is unavailable), fall back to whichever system color contrasts
// with the sampled bar background (COLOR_WINDOWTEXT vs COLOR_HIGHLIGHTTEXT).

// Sample the center pixel of the themed bar background, or CLR_INVALID when the bar
// is not themed. Used to pick both the contrasting text color and the (blended)
// separator color.
[[nodiscard]] COLORREF SampleBarColor(HTHEME theme, int part, int w, int h) {
    if (!theme || w <= 0 || h <= 0) return CLR_INVALID;
    const HDC screen = GetDC(nullptr);
    const HDC mem = CreateCompatibleDC(screen);
    const HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    const HGDIOBJ old = SelectObject(mem, bmp);
    const RECT rc = { 0, 0, w, h };
    DrawThemeBackground(theme, mem, part, 0, &rc, nullptr);
    const COLORREF c = GetPixel(mem, w / 2, h / 2);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    return c;
}

// ITU-R BT.601 luma weights (summing to kLumaScale) that reduce a color to its perceived
// brightness, and the midpoint of the 0..255 luminance range at which the contrasting
// text color flips between the light and dark system colors.
constexpr int kLumaWeightR  = 299;
constexpr int kLumaWeightG  = 587;
constexpr int kLumaWeightB  = 114;
constexpr int kLumaScale    = 1000;
constexpr int kLumaMidpoint = 128;

// Contrasting number-text color for the given bar background: prefer the theme's own
// text color, else pick the light/dark system text color by background luminance.
[[nodiscard]] COLORREF PickTextColor(HTHEME theme, int part, COLORREF bar) {
    COLORREF themed = 0;
    if (theme && SUCCEEDED(GetThemeColor(theme, part, 0, TMT_TEXTCOLOR, &themed)))
        return themed;
    if (bar == CLR_INVALID) return GetSysColor(COLOR_WINDOWTEXT);
    const int lum = (GetRValue(bar) * kLumaWeightR + GetGValue(bar) * kLumaWeightG +
                     GetBValue(bar) * kLumaWeightB) / kLumaScale;

    return lum < kLumaMidpoint ? GetSysColor(COLOR_HIGHLIGHTTEXT) : GetSysColor(COLOR_WINDOWTEXT);
}

// Separator color: the midpoint between the text color and the bar background, so the
// divider is always clearly visible (half the text's contrast) yet softer than the
// numbers. Falls back to the grayed-text system color when the bar isn't themed.
[[nodiscard]] COLORREF PickSeparatorColor(COLORREF text, COLORREF bar) {
    if (bar == CLR_INVALID) return GetSysColor(COLOR_GRAYTEXT);

    return RGB((GetRValue(text) + GetRValue(bar)) / 2,
               (GetGValue(text) + GetGValue(bar)) / 2,
               (GetBValue(text) + GetBValue(bar)) / 2);
}

} // namespace

namespace Overlay {

void Init(HINSTANCE inst) {
    WNDCLASSEX wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = inst;
    wc.lpszClassName = kOverlayClass;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;   // background comes from the taskbar theme
    VERIFY(RegisterClassEx(&wc));
}

void Shutdown() {
    Hide();
}

// Fully tear down the overlay session (called on Win release). Destroys the window
// (and its timer + child controls) and releases the per-show resources. Idempotent:
// a no-op when nothing is shown, so the persistent poll can call it every tick.
void Hide() {
    if (!g_active && !g_overlay) return;
    g_active = false;
    if (g_overlay) {
        DestroyWindow(g_overlay);   // also destroys the child STATIC controls + timer
        g_overlay = nullptr;
    }
    FreeResources();
    g_snapN = 0;
}

// Begin an overlay session (Win pressed). Creates the persistent window + refresh
// timer once, then builds the bar for the current taskbar. The window and timer stay
// alive for the whole session so the bar keeps tracking taskbar changes -- and can
// reappear after an empty period -- until Hide().
void Show(IUIAutomation* uia, HINSTANCE inst) {
    if (g_active) return;   // already in a session; refreshes go through the timer
    g_uia = uia;
    g_inst = inst;
    if (!g_overlay) {
        g_overlay = CreateOverlayWindow(inst);
        if (!g_overlay) return;
    }
    // Re-arm each session (the window and its timer persist across sessions) so a changed
    // refresh-interval preference takes effect on the next Win-press.
    ApplyOpacityPreference();
    ArmRefreshTimer();
    g_active = true;
    Refresh();
}

void ApplyPreferences() {
    if (!g_active || !g_overlay) return;
    ApplyOpacityPreference();
    ArmRefreshTimer();
    Refresh();
}

// Report the current taskbar's tip-size bounds (see Overlay.h). Shares
// ComputeStripBounds with the renderer so the dialog's percentages map to the same
// thicknesses the bar will draw. No UI Automation needed -- the bounds come from the
// taskbar's own geometry.
bool TipSizeBounds(int& defThick, int& btnThick, bool& vertical) {
    const HWND taskbar = Taskbar::Find();
    RECT tr;
    UINT edge;
    if (!taskbar || !Taskbar::GetPos(tr, edge)) return false;
    const UINT dpi = WinAPI::Window::GetDpi(taskbar);
    ComputeStripBounds(tr, edge, dpi, defThick, btnThick, vertical);
    return true;
}

} // namespace Overlay

namespace {

int CollectCurrent(RECT* out, int max) {
    const HWND taskbar = Taskbar::Find();
    if (!taskbar || Taskbar::Obscured(taskbar)) return 0;

    return Taskbar::CollectButtonRects(g_uia, taskbar, out, max);
}

// Hide the bar's contents while keeping the window and its timer alive, so the session
// keeps polling and the bar can reappear when a button comes back (e.g. the only app's
// button briefly disappears during a Win+Arrow switch).
void ApplyEmpty() {
    if (g_overlay) ShowWindow(g_overlay, SW_HIDE);
    if (g_overlay) DestroyChildren(g_overlay);
    FreeResources();
    g_snapN = 0;
}

// Rebuild the bar's contents in place for the current taskbar state. The window
// already exists (created by Show); on every refresh its child controls and per-show
// resources are swapped with painting suspended, so numbers realign without flicker.
// When there are no buttons to annotate the window is hidden but the session continues.
void Refresh() {
    if (!g_overlay) return;

    RECT btn[kMaxTips];
    const int n = CollectCurrent(btn, kMaxTips);
    if (n == 0) { ApplyEmpty(); return; }

    const HWND taskbar = Taskbar::Find();
    RECT tr;
    UINT edge;
    if (!taskbar || !Taskbar::GetPos(tr, edge)) { ApplyEmpty(); return; }

    // Strip thickness (perpendicular to the taskbar) from the shared bounds: a slim
    // default up to a full taskbar-button-sized cell. The "Tip size" preference is the
    // percentage of that full button thickness (Preferences::TipSizePercent), clamped so
    // the strip is never thinner than the default -- the dialog only offers percentages at
    // or above the default's share, so every selectable value visibly changes the strip.
    const UINT dpi = WinAPI::Window::GetDpi(taskbar);
    int defThick, btnThick;
    bool vertical;
    ComputeStripBounds(tr, edge, dpi, defThick, btnThick, vertical);

    int p = Preferences::TipSizePercent();
    if (p < Preferences::kMinPercent) p = Preferences::kMinPercent;
    else if (p > Preferences::kMaxPercent) p = Preferences::kMaxPercent;
    int thick = MulDiv(btnThick, p, Preferences::kMaxPercent);
    if (thick < defThick) thick = defThick;
    else if (thick > btnThick) thick = btnThick;

    const int BH = vertical ? 0 : thick;
    const int BW = vertical ? thick : 0;

    // One continuous strip spanning from the first to the last taskbar button along
    // the taskbar's long axis, laid just OUTSIDE the taskbar's inner edge so it sits
    // above (or beside) the buttons without overlapping the taskbar itself.
    int lo = vertical ? btn[0].top   : btn[0].left;
    int hi = vertical ? btn[0].bottom: btn[0].right;
    for (int i = 1; i < n; ++i) {
        const int a = vertical ? btn[i].top    : btn[i].left;
        const int b = vertical ? btn[i].bottom : btn[i].right;
        if (a < lo) lo = a;
        if (b > hi) hi = b;
    }

    RECT ov;
    switch (edge) {
        case ABE_TOP:    ov = {.left = lo, .top = tr.bottom, .right = hi, .bottom = tr.bottom + BH }; g_bgPart = TBP_BACKGROUNDTOP;    break;
        case ABE_LEFT:   ov = {.left = tr.right, .top = lo, .right = tr.right + BW, .bottom = hi };   g_bgPart = TBP_BACKGROUNDLEFT;   break;
        case ABE_RIGHT:  ov = {.left = tr.left - BW, .top = lo, .right = tr.left, .bottom = hi };     g_bgPart = TBP_BACKGROUNDRIGHT;  break;
        default:         ov = {.left = lo, .top = tr.top - BH, .right = hi, .bottom = tr.top };       g_bgPart = TBP_BACKGROUNDBOTTOM; break; // BOTTOM
    }
    const int OW = ov.right - ov.left;
    const int OH = ov.bottom - ov.top;
    if (OW <= 0 || OH <= 0) { ApplyEmpty(); return; }

    // Update in place: suspend the window's painting, drop the old child controls and
    // per-show resources, then rebuild everything and repaint once (double-buffered
    // via WS_EX_COMPOSITED) so the swap is flicker-free.
    SetWindowRedraw(g_overlay, FALSE);
    DestroyChildren(g_overlay);
    FreeResources();
    SetWindowPos(g_overlay, HWND_TOPMOST, ov.left, ov.top, OW, OH,
                 SWP_NOACTIVATE | SWP_NOREDRAW);

    g_theme = OpenThemeData(g_overlay, TEXT("TaskBar"));
    const COLORREF barColor  = SampleBarColor(g_theme, g_bgPart, OW, OH);
    const COLORREF textColor = PickTextColor(g_theme, g_bgPart, barColor);

    // Invert-colors mode swaps the roles of the bar and number colors: the strip is
    // filled solid with the number (text) color and the numbers are drawn in the bar
    // color, giving a highlighted look. The fill is cached in a brush for PaintBackground;
    // in normal mode the themed taskbar background is painted instead.
    g_invert = Preferences::InvertColors();
    if (g_invert) {
        g_textColor = barColor != CLR_INVALID ? barColor : GetSysColor(COLOR_3DFACE);
        g_bgBrush   = CreateSolidBrush(textColor);
    } else {
        g_textColor = textColor;
    }

    // Shell/taskbar UI font (SPI_GETNONCLIENTMETRICS, DPI-aware) so the numbers match the
    // taskbar's own text; fall back to the system default GUI font. When the "Tip size"
    // preference has grown the strip past the default, the font is scaled up by the same
    // ratio so the numbers grow to fill the larger cell -- capped to the cell's fixed
    // cross extent (button width for a horizontal bar, button height for a vertical one)
    // so a digit never overflows the dimension that the strip does not grow. When a font
    // has been chosen in Preferences, its face and style (weight/italic/underline/
    // strikeout) replace the taskbar font's while keeping this computed height; otherwise
    // the taskbar font is used unchanged (the fallback).
    NONCLIENTMETRICS ncm;
    ZeroMemory(&ncm, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi)) {
        if (defThick > 0 && thick > defThick) {
            LOGFONT* const lf = &ncm.lfMessageFont;
            const int base = lf->lfHeight < 0 ? -lf->lfHeight : lf->lfHeight;
            int scaled = MulDiv(base, thick, defThick);
            int cross = vertical ? (btn[0].bottom - btn[0].top) : (btn[0].right - btn[0].left);
            for (int i = 1; i < n; ++i) {
                const int c = vertical ? (btn[i].bottom - btn[i].top) : (btn[i].right - btn[i].left);
                if (c > 0 && c < cross) cross = c;
            }
            if (cross > 0 && scaled > cross) scaled = cross;
            if (scaled < 1) scaled = 1;
            lf->lfHeight = lf->lfHeight < 0 ? -scaled : scaled;
        }
        if (Preferences::FontIsSet()) {
            const LOGFONT& pf = Preferences::Font();
            LOGFONT* const lf = &ncm.lfMessageFont;
            lstrcpyn(lf->lfFaceName, pf.lfFaceName, LF_FACESIZE);
            lf->lfWeight         = pf.lfWeight;
            lf->lfItalic         = pf.lfItalic;
            lf->lfUnderline      = pf.lfUnderline;
            lf->lfStrikeOut      = pf.lfStrikeOut;
            lf->lfCharSet        = DEFAULT_CHARSET;
            lf->lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE; // let the face name decide
        }
        g_font = CreateFontIndirect(&ncm.lfMessageFont);
        g_ownFont = g_font != nullptr;
    }
    if (!g_font) {
        g_font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        g_ownFont = false;
    }

    // Pass 1: a centered number tip per taskbar button (tips span the full button
    // width and meet at the boundaries). Pass 2: a separator line on each boundary,
    // created last so it sits on top of the tips' Z-order and is not occluded. Each
    // separator is a plain STATIC filled with the solid line brush (via
    // WM_CTLCOLORSTATIC): a neutral divider midway between the text and bar colors so
    // it is clearly visible yet softer than the numbers. Its thickness comes from the
    // system border metric and it is inset from the bar ends by the fixed window-frame
    // metric, so both scale with DPI. The midpoint uses the true text/bar colors, so it
    // stays balanced whether or not the colors are inverted.
    g_lineBrush = CreateSolidBrush(PickSeparatorColor(textColor, barColor));
    const int lineW  = max(1, GetSystemMetricsForDpi(SM_CXBORDER, dpi));
    const int insetX = GetSystemMetricsForDpi(SM_CXFIXEDFRAME, dpi);
    const int insetY = GetSystemMetricsForDpi(SM_CYFIXEDFRAME, dpi);

    for (int i = 0; i < n; ++i) {
        TCHAR s[2];
        s[0] = i < kMaxTips - 1 ? static_cast<TCHAR>(TEXT('1') + i) : TEXT('0');
        s[1] = 0;
        if (vertical) {
            const int y = btn[i].top - ov.top;
            const int h = btn[i].bottom - btn[i].top;
            AddStatic(g_overlay, g_inst, SS_CENTER | SS_CENTERIMAGE, s, 0, y, OW, h);
        } else {
            const int x = btn[i].left - ov.left;
            const int w = btn[i].right - btn[i].left;
            AddStatic(g_overlay, g_inst, SS_CENTER | SS_CENTERIMAGE, s, x, 0, w, OH);
        }
    }

    for (int i = 0; i + 1 < n; ++i) {
        if (vertical) {
            const int boundary = (btn[i].bottom + btn[i + 1].top) / 2 - ov.top;
            AddStatic(g_overlay, g_inst, 0, TEXT(""),
                      insetX, boundary - lineW / 2, OW - 2 * insetX, lineW, kSepId);
        } else {
            const int boundary = (btn[i].right + btn[i + 1].left) / 2 - ov.left;
            AddStatic(g_overlay, g_inst, 0, TEXT(""),
                      boundary - lineW / 2, insetY, lineW, OH - 2 * insetY, kSepId);
        }
    }

    // Re-enable painting, make sure the window is visible (it may have been hidden
    // during an empty period) and topmost, then repaint the whole bar once. Batching
    // the child swap between WM_SETREDRAW FALSE/TRUE keeps the in-place refresh from
    // flickering.
    SetWindowRedraw(g_overlay, TRUE);
    SetWindowPos(g_overlay, HWND_TOPMOST, ov.left, ov.top, OW, OH,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER | SWP_NOREDRAW);
    RedrawWindow(g_overlay, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN);

    // Update the button snapshot the timer compares against on the next tick.
    g_snapN = n;
    MoveMemory(g_snap, btn, sizeof(RECT) * n);
}

} // namespace
