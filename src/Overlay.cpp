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
HBRUSH   g_borderBrush;  // solid brush, fixed to the system window-frame color, that paints the outer border
HBRUSH   g_bgBrush;    // solid strip-fill brush, only in invert-colors mode
Preferences::RenderFlags g_flags;  // cached; mirrors Preferences::Flags() for this show
int      g_bgPart = TBP_BACKGROUNDBOTTOM;  // taskbar background part for the docked edge
COLORREF g_textColor;  // resolved from the theme / system colors in Show
COLORREF g_barColor;   // sampled themed bar color; flat fallback fill for "hide border"
bool     g_active;     // true between Show (Win down) and Hide (Win up)

// Retained so the refresh timer can rebuild the bar in place when the taskbar's
// buttons change while it is shown (e.g. an app minimizes/closes to the notification
// area, which removes its button and would otherwise leave an orphaned number behind).
IUIAutomation* g_uia;
HINSTANCE      g_inst;
int            g_snapN;  // button count captured when the bar was built; also the number
                          // of valid entries in g_snap and g_tipRect below
RECT           g_snap[kMaxTips];              // button rects captured when the bar was built
RECT           g_tipRect[kMaxTips];  // each tip's own digit square (see ApplyStripRegion),
                                     // cached so PaintBorder can frame each one
                                     // individually in "compact" mode
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
    WinAPI::GdiObject::Delete(g_borderBrush);
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


// Fill one rect's four edges, each 'w' thick, with the border brush -- the shared
// drawing step behind both the whole-strip border and, in "compact" mode, each tip's
// own individual one below.
void PaintFrame(HDC hdc, const RECT& r, int w) {
    RECT edge;
    edge = {.left = r.left,      .top = r.top,        .right = r.right,     .bottom = r.top + w   }; FillRect(hdc, &edge, g_borderBrush);
    edge = {.left = r.left,      .top = r.bottom - w,  .right = r.right,     .bottom = r.bottom    }; FillRect(hdc, &edge, g_borderBrush);
    edge = {.left = r.left,      .top = r.top,         .right = r.left + w,  .bottom = r.bottom    }; FillRect(hdc, &edge, g_borderBrush);
    edge = {.left = r.right - w, .top = r.top,         .right = r.right,     .bottom = r.bottom    }; FillRect(hdc, &edge, g_borderBrush);
}

// Paint a border, fixed to the system window-frame color (see g_borderBrush) rather than
// the separator's theme-derived color, so it reads as a normal window border regardless
// of the sampled taskbar theme, as four filled edge strips (rather than FrameRect, which
// cannot be widened). Its thickness comes from the system border metric, same as a
// separator's, so it scales with DPI. In "compact" mode this frames each tip's own digit
// square (g_tipRect, cached by ApplyStripRegion) individually rather than the whole strip
// as one shared frame -- the square is already the entirety of what the window's own
// region shows for that tip (see ApplyStripRegion), so the frame's outer edge lines up
// exactly with the region's own edge there and nothing further out is needed; every tip
// reads as its own small bordered box, with the gap between the window region's tips
// (formerly a separator's job) now serving as the visual divider, which is why "compact"
// forces "hide separator" on instead (see ReadFlags): a dedicated divider would be
// redundant. Otherwise (not "compact") frames 'rc', the whole strip, as a single rect,
// drawn over whatever PaintBackground just filled.
void PaintBorder(HWND hwnd, HDC hdc, const RECT& rc) {
    if (!g_borderBrush) return;

    const int w = max(1, GetSystemMetricsForDpi(SM_CXBORDER, WinAPI::Window::GetDpi(hwnd)));
    if (g_flags.compact) {
        for (int i = 0; i < g_snapN; ++i) PaintFrame(hdc, g_tipRect[i], w);
    } else {
        PaintFrame(hdc, rc, w);
    }
}

// Paint the bar background: in invert-colors mode a solid fill with the number color;
// otherwise the taskbar theme -- as a flat fill of the sampled bar color instead of the
// theme drawing itself when "hide border" is set (see the flat-fill branch below), or
// falling back to the 3D face color when the theme is unavailable, e.g. classic mode.
// Then the border on top, unless "hide border" is set. Painted the same way regardless of
// "compact": that flag shrinks the window's own region (see ApplyStripRegion) down
// to the border, the separators and a small square around each digit instead of changing
// what is drawn here, so only those pieces of this same fill end up on screen and the
// rest of the strip is simply not part of the window. Used from both WM_ERASEBKGND and
// WM_PRINTCLIENT so DrawThemeParentBackground can show it behind the tips.
void PaintBackground(HWND hwnd, HDC hdc) {
    RECT rc;
    VERIFY(GetClientRect(hwnd, &rc));
    if (g_flags.invertColors && g_bgBrush) {
        FillRect(hdc, &rc, g_bgBrush);
    } else if (g_theme && g_flags.hideBorder) {
        // DrawThemeBackground stretches the visual style's own edge/highlight to fit our
        // slim strip, which leaves a residual line right where the border would be even
        // with it suppressed; a flat fill with the already-sampled bar color avoids that
        // baked-in artifact instead of trying to paint over it.
        const HBRUSH bar = CreateSolidBrush(g_barColor);
        ASSERT(bar);
        if (bar) {
            VERIFY(FillRect(hdc, &rc, bar));
            VERIFY(DeleteObject(bar));
        }
    } else if (g_theme) {
        DrawThemeBackground(g_theme, hdc, g_bgPart, 0, &rc, nullptr);
    } else {
        FillRect(hdc, &rc, reinterpret_cast<HBRUSH>(COLOR_3DFACE + 1));
    }

    if (!g_flags.hideBorder) PaintBorder(hwnd, hdc, rc);
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

// Apply the "opacity" preference to the overlay window as a constant alpha blend
// (WS_EX_LAYERED + LWA_ALPHA covers the window and its child tip/separator statics
// together, so no per-child changes are needed). "Compact view" is handled separately,
// by shrinking the window's own region (see ApplyStripRegion in Refresh) rather than a
// layered-window color key. Reads Preferences directly rather than the Refresh-cached
// flags, since both call sites (Show, ApplyPreferences) run this before Refresh
// repopulates them. No-op while the window doesn't exist.
void ApplyLayeredAttributes() {
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
    ASSERT(screen);
    if (!screen) return CLR_INVALID;

    const HDC mem = CreateCompatibleDC(screen);
    ASSERT(mem);
    if (!mem) { VERIFY(ReleaseDC(nullptr, screen) == 1); return CLR_INVALID; }

    const HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    ASSERT(bmp);
    if (!bmp) {
        VERIFY(DeleteDC(mem));
        VERIFY(ReleaseDC(nullptr, screen) == 1);
        return CLR_INVALID;
    }

    const HGDIOBJ old = SelectObject(mem, bmp);
    const bool selected = old && old != HGDI_ERROR;
    ASSERT(selected);

    COLORREF c = CLR_INVALID;
    if (selected) {
        const RECT rc = { 0, 0, w, h };
        DrawThemeBackground(theme, mem, part, 0, &rc, nullptr);
        c = GetPixel(mem, w / 2, h / 2);
        VERIFY(SelectObject(mem, old));
    }

    VERIFY(DeleteObject(bmp));
    VERIFY(DeleteDC(mem));
    VERIFY(ReleaseDC(nullptr, screen) == 1);

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
    ApplyLayeredAttributes();
    ArmRefreshTimer();
    g_active = true;
    Refresh();
}

void ApplyPreferences() {
    if (!g_active || !g_overlay) return;
    ApplyLayeredAttributes();
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

// Measure tip 'i's actual digit glyph (the same character the tip loop below draws) at
// whatever font is currently selected into 'hdc' -- the shared tip font (g_font), which
// already carries any custom weight/italic/underline/strikeout from Preferences (see
// Refresh), and 'tm' is that same font's metrics (GetTextMetrics), fetched once by the
// caller since they are identical for every digit.
//
// Horizontally: GetTextExtentPoint32 reports the advance box at the font's real size and
// weight. An italic or otherwise slanted face paints past that box's left/right edges,
// so GetCharABCWidths' negative A/C bearings report how far the ink reaches beyond it on
// each side -- the same technique FontPreview.cpp uses for its own digits run. 'shiftX'
// is half that left/right imbalance: SS_CENTER centers the advance box, not the ink, so
// a slanted glyph's true ink center sits off that box's midpoint by this much. Bearings
// stay zero for a bitmap face -- it has no such overhang and folds its own slant into
// extent.cx already -- since GetCharABCWidths only serves TrueType.
//
// Vertically, there is no advance-box equivalent to lean on: tmAscent/tmDescent are the
// font's whole cell, sized to also fit accents above capitals and descenders below the
// baseline that digits never use, so even an upright digit can sit well short of the
// cell's top while touching its bottom (true for Segoe UI, for one) -- SS_CENTERIMAGE
// centers that lopsided cell, not the ink within it, leaving a bigger gap above the
// glyph than below. GetGlyphOutline's GLYPHMETRICS gives the glyph's true black-box
// height (gmBlackBoxY) and its top-left corner relative to the baseline
// (gmptGlyphOrigin, positive upward), which is what lets that lopsidedness be measured,
// via the same gap-imbalance idea as 'shiftX', as 'shiftY'.
void MeasureDigitInk(HDC hdc, const TEXTMETRIC& tm, int i, int& inkW, int& inkH, int& shiftX, int& shiftY) {
    inkW = inkH = shiftX = shiftY = 0;
    const TCHAR c = i < kMaxTips - 1 ? static_cast<TCHAR>(TEXT('1') + i) : TEXT('0');
    SIZE extent = { 0, 0 };
    const bool measured = GetTextExtentPoint32(hdc, &c, 1, &extent);
    ASSERT(measured);
    if (!measured) return;

    int leftBearing = 0, rightBearing = 0;
    ABC abc;
    if (GetCharABCWidths(hdc, c, c, &abc)) {
        if (abc.abcA < 0) leftBearing  = -abc.abcA;
        if (abc.abcC < 0) rightBearing = -abc.abcC;
    }
    inkW   = leftBearing + extent.cx + rightBearing;
    shiftX = (rightBearing - leftBearing) / 2;

    GLYPHMETRICS gm;
    MAT2 identity = {};
    identity.eM11.value = 1;
    identity.eM22.value = 1;
    if (GetGlyphOutline(hdc, c, GGO_METRICS, &gm, 0, nullptr, &identity) != GDI_ERROR) {
        inkH = gm.gmBlackBoxY;
        const int topGap    = tm.tmAscent  - gm.gmptGlyphOrigin.y;
        const int bottomGap = tm.tmDescent - (static_cast<int>(gm.gmBlackBoxY) - gm.gmptGlyphOrigin.y);
        shiftY = (topGap - bottomGap) / 2;
    } else {
        inkH = extent.cy;   // fall back to the padded cell height; still caps side below
    }
}

// Add one rectangle to the current path as an explicit 4-point polygon rather than via
// Rectangle(hdc, ...): GDI's Rectangle collapses to an empty path element whenever the
// rect is exactly 1 pixel thin along either axis -- confirmed with an offscreen
// BeginPath/Rectangle/PathToRegion/PtInRegion repro, where a lone 1-pixel-tall
// Rectangle() produced a NULLREGION regardless of pen (even NULL_PEN). 1 device pixel
// is exactly the border/separator thickness (SM_CXBORDER) at 100% DPI, which is why the
// per-tip border below vanished from the window's region entirely -- clipped away, not
// just painted with the wrong color -- instead of merely looking thin. Polygon has no
// such collapse, so it is used for every rect added to the path below, not just the
// thin ones, for consistency.
void PathRect(HDC hdc, int left, int top, int right, int bottom) {
    const POINT pts[4] = {
        { left,  top    },
        { right, top    },
        { right, bottom },
        { left,  bottom },
    };
    Polygon(hdc, pts, 4);
}

// Shape the overlay window itself for the "compact" flag, instead of painting a
// transparent fill: restrict the window's region to just the separators (the same rects
// the separator AddStatic calls use) and one square centered on each digit (also cached
// per tip into g_tipRect so PaintBorder can frame it individually -- the square is the
// entire per-tip region here, so a frame around it needs no rect of its own). A square's
// side is its actual measured ink extent (see MeasureDigitInk)
// padded on all sides by the same system margin used below to inset from the strip's own
// edges, so the glyph gets some breathing room rather than the square hugging its exact
// ink bounds -- capped by the tip cell's along-strip extent and by its cross extent
// shrunk by that same margin (insetX/insetY, the same the separators are already inset
// by) on both ends, so the padded square still never reaches past the cell or the
// border/edge either. Its center is nudged by the glyph's own bearing imbalance (shiftX
// horizontally, shiftY vertically) so it follows the ink rather than the box
// SS_CENTER/SS_CENTERIMAGE actually center, staying accurate for a bold, italic or
// otherwise lopsided custom font alike -- this keeps the ink's own top/bottom (and
// left/right) margins equal within the square, since shiftY/shiftX are derived directly
// from the glyph's measured gaps rather than any fixed or font-family-specific guess.
// Everything outside these shapes is simply not
// part of the window, letting whatever is behind show through with no color key and no
// antialiasing halo to work around. Restores the default whole-rectangle region when the
// flag is off. 'ov' and 'btn' are the same screen-coordinate rects Refresh just used to
// place the tips and separators; hwnd is always g_overlay.
void ApplyStripRegion(HWND hwnd, const RECT& ov, const RECT* btn, int n, bool vertical) {
    if (!g_flags.compact) {
        SetWindowRgn(hwnd, nullptr, FALSE);
        return;
    }

    RECT rc;
    VERIFY(GetClientRect(hwnd, &rc));
    const int OW = rc.right;
    const int OH = rc.bottom;
    const UINT dpi   = WinAPI::Window::GetDpi(hwnd);
    const int lineW  = max(1, GetSystemMetricsForDpi(SM_CXBORDER, dpi));
    const int insetX = GetSystemMetricsForDpi(SM_CXFIXEDFRAME, dpi);
    const int insetY = GetSystemMetricsForDpi(SM_CYFIXEDFRAME, dpi);

    const HDC hdc = GetDC(hwnd);
    ASSERT(hdc);
    const HGDIOBJ prevFont = SelectObject(hdc, g_font);
    const bool fontSelected = prevFont && prevFont != HGDI_ERROR;
    ASSERT(fontSelected);

    // tm is shared by every digit (see MeasureDigitInk), so it is fetched once here
    // rather than once per digit.
    TEXTMETRIC tm;
    const bool gotMetrics = fontSelected && GetTextMetrics(hdc, &tm);
    ASSERT(!fontSelected || gotMetrics);

    // Measured up front, before the path bracket opens below, so BeginPath only ever
    // captures the actual outline-producing calls (PathRect), not these text-metric
    // queries.
    int inkW[kMaxTips], inkH[kMaxTips], shiftX[kMaxTips], shiftY[kMaxTips];
    for (int i = 0; i < n; ++i) {
        if (gotMetrics) MeasureDigitInk(hdc, tm, i, inkW[i], inkH[i], shiftX[i], shiftY[i]);
        else inkW[i] = inkH[i] = shiftX[i] = shiftY[i] = 0;
    }

    VERIFY(BeginPath(hdc));
    // WINDING (rather than the default ALTERNATE) so touching or overlapping rectangles
    // union together instead of XOR-cancelling where they meet.
    SetPolyFillMode(hdc, WINDING);

    if (!g_flags.hideSeparator) {
        for (int i = 0; i + 1 < n; ++i) {
            if (vertical) {
                const int boundary = (btn[i].bottom + btn[i + 1].top) / 2 - ov.top;
                PathRect(hdc, insetX, boundary - lineW / 2, OW - insetX, boundary - lineW / 2 + lineW);
            } else {
                const int boundary = (btn[i].right + btn[i + 1].left) / 2 - ov.left;
                PathRect(hdc, boundary - lineW / 2, insetY, boundary - lineW / 2 + lineW, OH - insetY);
            }
        }
    }

    for (int i = 0; i < n; ++i) {
        // Each tip's own cell, full-width (vertical) or full-height (horizontal) -- the
        // same bounds Refresh gave its AddStatic call, used below only to center the
        // digit square within it.
        const int x = vertical ? 0 : btn[i].left - ov.left;
        const int y = vertical ? btn[i].top - ov.top : 0;
        const int w = vertical ? OW : btn[i].right - btn[i].left;
        const int h = vertical ? btn[i].bottom - btn[i].top : OH;

        // Padded so the square gives the glyph some breathing room instead of hugging
        // its exact ink bounds -- the same system margin used to inset from the strip's
        // own edges, applied here on all four sides of the ink. Padding is only added
        // once measurement has confirmed there is real ink to pad; a failed measurement
        // (rawInk 0) still falls through to the plain geometric cap below.
        const int rawInk = max(inkW[i], inkH[i]);
        const int pad = vertical ? insetX : insetY;
        const int ink = rawInk > 0 ? rawInk + 2 * pad : 0;
        const int cap  = vertical ? min(OW - 2 * insetX, h) : min(w, OH - 2 * insetY);
        const int side = ink > 0 ? min(cap, ink) : cap;
        const int left = vertical ? (OW - side) / 2 + shiftX[i] : x + (w - side) / 2 + shiftX[i];
        const int top  = vertical ? y + (h - side) / 2 + shiftY[i] : (OH - side) / 2 + shiftY[i];
        // Cached so PaintBorder can frame exactly this square in "compact" mode -- the
        // square is this tip's whole region here, so its own outer edge doubles as the
        // border frame's outer edge; no separate, larger border rect is added to the
        // path below.
        g_tipRect[i] = {.left = left, .top = top, .right = left + side, .bottom = top + side};
        PathRect(hdc, left, top, left + side, top + side);
    }

    VERIFY(EndPath(hdc));
    const HRGN rgn = PathToRegion(hdc);
    if (fontSelected) VERIFY(SelectObject(hdc, prevFont));
    VERIFY(ReleaseDC(hwnd, hdc) == 1);
    ASSERT(rgn);

    const BOOL applied = SetWindowRgn(hwnd, rgn, FALSE);
    ASSERT(applied);
    if (!applied) VERIFY(DeleteObject(rgn));   // ownership only transfers to the OS on success
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

    const int tipPercent = Preferences::TipSizePercent();
    const int p = max(Preferences::kMinPercent, min(tipPercent, Preferences::kMaxPercent));
    int thick = MulDiv(btnThick, p, Preferences::kMaxPercent);
    thick = max(defThick, min(thick, btnThick));

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
        lo = min(lo, a);
        hi = max(hi, b);
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
    g_barColor = barColor;   // cached: PaintBackground's flat-fill fallback for "hide border"

    // Cache the current rendering toggles (see RenderFlags) for this build of the bar --
    // used throughout the rest of Refresh and by PaintBackground/PaintBorder.
    g_flags = Preferences::Flags();

    // Invert-colors mode swaps the roles of the bar and number colors: the strip is
    // filled solid with the number (text) color and the numbers are drawn in the bar
    // color, giving a highlighted look. The fill is cached in a brush for PaintBackground;
    // in normal mode the themed taskbar background is painted instead.
    if (g_flags.invertColors) {
        g_textColor = barColor != CLR_INVALID ? barColor : GetSysColor(COLOR_3DFACE);
        g_bgBrush   = CreateSolidBrush(textColor);
        ASSERT(g_bgBrush);
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
                if (c > 0) cross = min(cross, c);
            }
            if (cross > 0) scaled = min(scaled, cross);
            scaled = max(scaled, 1);
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
    // width and meet at the boundaries). Pass 2: unless the "hide separator" flag is set,
    // a separator line on each boundary, created last so it sits on top of the tips'
    // Z-order and is not occluded. Each separator is a plain STATIC filled with the solid
    // line brush (via WM_CTLCOLORSTATIC): a neutral divider midway between the text and
    // bar colors so it is clearly visible yet softer than the numbers. Its thickness
    // comes from the system border metric and it is inset from the bar ends by the fixed
    // window-frame metric, so both scale with DPI. The midpoint uses the true text/bar
    // colors, so it stays balanced whether or not the colors are inverted. The outer
    // border uses its own brush (g_borderBrush), fixed to the system window-frame color
    // regardless of the theme, so it stays a normal-looking border unlike the separators;
    // in "compact" mode PaintBorder frames each tip's own digit square with it instead of
    // the whole strip, using the bounds ApplyStripRegion caches into g_tipRect.
    // Tips keep this same full-button geometry in "compact" mode too -- only a
    // small square of each ends up visible, via the window region ApplyStripRegion sets
    // up below, not by resizing the controls themselves.
    g_lineBrush   = CreateSolidBrush(PickSeparatorColor(textColor, barColor));
    g_borderBrush = CreateSolidBrush(GetSysColor(COLOR_WINDOWFRAME));
    ASSERT(g_lineBrush);
    ASSERT(g_borderBrush);
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

    if (!g_flags.hideSeparator) {
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
    }

    // Shrink the window's own region down to the border, the separators and a square
    // around each digit when "compact" is set, or restore the default whole-
    // rectangle region otherwise. Placed here, alongside the tips/separators whose
    // geometry it mirrors, rather than because it depends on them being created first.
    ApplyStripRegion(g_overlay, ov, btn, n, vertical);

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
