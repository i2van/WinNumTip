#include "stdafx.h"
#include "FontPicker.h"
#include "Preferences.h"
#include "resource.h"

namespace {

// The application instance that owns the FONTSELECTORDLG template resource (captured in
// Init); passed to ChooseFont so it can load the custom template.
HINSTANCE g_inst = nullptr;

// The font currently chosen in the dialog and whether the user has picked one at all.
// Only face + style fields matter downstream (the overlay drives glyph height); the height
// carried here is just the dialog font's, used to render the preview at a legible size.
// When g_workingFontSet is false the overlay would fall back to the taskbar font, and the
// preview shows "Default" in the dialog's own font.
LOGFONT g_workingFont = { 0 };
bool g_workingFontSet = false;

// The default (fallback) font captured at Init -- the taskbar/dialog font shown as "Default".
// The in-dialog "Reset to default font" link reverts the ChooseFont selection to it, and
// g_resetToFallback records that reset so OK stores "Default" (cleared) rather than the
// fallback face; any explicit font/style/effect change in the dialog cancels it.
LOGFONT g_fallbackFont = { 0 };
bool g_resetToFallback = false;

// The font that renders the IDC_FONT_NAME preview in the chosen face; owned here and
// rebuilt on every change, freed by Cleanup.
HFONT g_previewFont = nullptr;

// The ChooseFont common dialog while it is open (captured in the hook), or null otherwise;
// lets the Preferences dialog bring it to the foreground on a repeat request. The font that
// renders its Sample control (stc5) in the current selection is owned here and rebuilt on
// every change (with the custom template comdlg32 does not repaint the sample itself).
HWND  g_chooseFontDlg = nullptr;
HFONT g_sampleFont    = nullptr;

// Posted to the ChooseFont dialog from the hook so the Sample is refreshed once the initial
// selection has settled (WM_APP is safe here: WM_TRAYICON uses WM_APP+1 on another window).
constexpr UINT kRefreshSampleMsg = WM_APP + 0x100;

// The custom ChooseFont dialog template (WinNumTip.rc) and the sample string it shows --
// the badge digits, so the preview reflects what the overlay actually draws.
constexpr LPCTSTR kFontTemplate = TEXT("FONTSELECTORDLG");
constexpr LPCTSTR kFontSample   = TEXT("0123456789");

// comdlg32 rewrites the ChooseFont Sample control (stc5) with its own preview string
// ("AaBbYyZz") on every selection change. Subclass it and intercept WM_SETTEXT so the
// sample always shows our badge digits instead.
LRESULT CALLBACK FontSampleSubclassProc(HWND w, UINT m, WPARAM wp, LPARAM lp,
                                        UINT_PTR id, DWORD_PTR /*ref*/) {
    switch (m) {
        case WM_SETTEXT:   return DefSubclassProc(w, WM_SETTEXT, wp, reinterpret_cast<LPARAM>(kFontSample));
        case WM_NCDESTROY: VERIFY(RemoveWindowSubclass(w, FontSampleSubclassProc, id)); break;
    }

    return DefSubclassProc(w, m, wp, lp);
}

// Repaint the ChooseFont Sample control (stc5) in the currently selected font. With a custom
// template comdlg32 does not refresh the sample itself, so read the live selection with
// WM_CHOOSEFONT_GETLOGFONT, build a matching font sized to fill the Sample box, and apply it
// (keeping the badge digits).
void RefreshChooseFontSample(HWND hDlg) {
    const HWND sample = GetDlgItem(hDlg, stc5);
    if (!sample) return;

    LOGFONT lf;
    ZeroMemory(&lf, sizeof(lf));
    SendMessage(hDlg, WM_CHOOSEFONT_GETLOGFONT, 0, reinterpret_cast<LPARAM>(&lf));

    // The Size control is hidden, so the selection carries the small dialog-font height and the
    // digits would render tiny. Size the sample to fill the Sample box instead -- its face, style
    // and effects still track the selection -- so it shows large, like the reference app: measure
    // the digits at a reference height, then take the largest height that fits the box in both
    // width and height (with a small margin).
    RECT rc;
    GetClientRect(sample, &rc);
    const int boxW = rc.right - rc.left;
    const int boxH = rc.bottom - rc.top;
    constexpr int kRefHeight = 100;
    lf.lfWidth  = 0;
    lf.lfHeight = -kRefHeight;
    if (const HFONT measure = CreateFontIndirect(&lf)) {
        const HDC dc = GetDC(sample);
        const HFONT prev = static_cast<HFONT>(SelectObject(dc, measure));
        SIZE ext = { 0, 0 };
        GetTextExtentPoint32(dc, kFontSample, lstrlen(kFontSample), &ext);
        SelectObject(dc, prev);
        ReleaseDC(sample, dc);
        DeleteObject(measure);

        int h = kRefHeight;
        if (ext.cx > 0 && boxW > 0) { const int hw = MulDiv(kRefHeight, boxW * 9 / 10, ext.cx); if (hw < h) h = hw; }
        if (ext.cy > 0 && boxH > 0) { const int hh = MulDiv(kRefHeight, boxH * 9 / 10, ext.cy); if (hh < h) h = hh; }
        lf.lfHeight = -(h > 0 ? h : 1);
    }

    const HFONT nf = CreateFontIndirect(&lf);
    if (!nf) return;
    if (g_sampleFont) DeleteObject(g_sampleFont);
    g_sampleFont = nf;
    SendMessage(sample, WM_SETFONT, reinterpret_cast<WPARAM>(nf), TRUE);
    VERIFY(SetWindowText(sample, kFontSample));
}

// Revert the ChooseFont selection to the default (fallback) font. WM_CHOOSEFONT_SETLOGFONT
// updates comdlg32's internal LOGFONT but not the visible Font/Style/Effects controls (so the
// font-name combo would keep the previous face), so instead drive the controls the way a user
// would: clear the effects, then select the default face in the name list (cmb1) and notify
// comdlg32 as if the user picked it -- CB_SETCURSEL alone sends no CBN_SELCHANGE -- which makes
// it repopulate the style list (cmb2) for the new face and rebuild its LOGFONT; then force the
// Regular style. The WM_COMMAND handler above refreshes the sample after each notification.
// Returns false when the fallback face is not in the list (nothing changed).
[[nodiscard]] bool ResetChooseFontToFallback(HWND hDlg) {
    const HWND nameCombo  = GetDlgItem(hDlg, cmb1);
    const HWND styleCombo = GetDlgItem(hDlg, cmb2);
    if (!nameCombo) return false;

    // Clear the effects first so the LOGFONT comdlg32 rebuilds below reads the default (none).
    VERIFY(CheckDlgButton(hDlg, chx1, BST_UNCHECKED));   // Strikeout off
    VERIFY(CheckDlgButton(hDlg, chx2, BST_UNCHECKED));   // Underline off

    const int face = static_cast<int>(SendMessage(nameCombo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                                                   reinterpret_cast<LPARAM>(g_fallbackFont.lfFaceName)));
    if (face < 0) return false;
    SendMessage(nameCombo, CB_SETCURSEL, face, 0);
    SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(cmb1, CBN_SELCHANGE), reinterpret_cast<LPARAM>(nameCombo));

    // Force the default (Regular) style now that the list matches the new face. The fallback is
    // the dialog font, which is always Regular, so its normal style is listed as "Regular".
    if (styleCombo) {
        const int style = static_cast<int>(SendMessage(styleCombo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                                                        reinterpret_cast<LPARAM>(TEXT("Regular"))));
        if (style >= 0) {
            SendMessage(styleCombo, CB_SETCURSEL, style, 0);
            SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(cmb2, CBN_SELCHANGE), reinterpret_cast<LPARAM>(styleCombo));
        }
    }

    return true;
}

// WM_COMMAND on the ChooseFont dialog subclass (below): comdlg32 has changed the font (cmb1),
// style (cmb2) or effects (chx1/chx2). Let it update its internal LOGFONT first (DefSubclassProc),
// then refresh the Sample and cancel any pending reset. The cracker returns the (ignored)
// WM_COMMAND result as 0; MAKEWPARAM/hwndCtl reconstruct the message passed on unchanged.
void OnChooseFontCommand(HWND hwnd, int id, HWND hwndCtl, UINT codeNotify) {
    DefSubclassProc(hwnd, WM_COMMAND, MAKEWPARAM(id, codeNotify), reinterpret_cast<LPARAM>(hwndCtl));
    if (id == cmb1 || id == cmb2 || id == chx1 || id == chx2) {
        RefreshChooseFontSample(hwnd);
        g_resetToFallback = false;   // an explicit font/style/effect change cancels a pending reset
    }
}

// WM_NOTIFY on the ChooseFont dialog subclass: the "Reset to default font" link reverts the
// Font/Style/Effects controls to the default (fallback) font. Flag the reset only if it took
// effect, and AFTER the call, since the synthetic combo changes it makes run through
// OnChooseFontCommand above and clear the flag; OK then stores "Default" (cleared) rather than
// the fallback face. Every other notification is passed through to comdlg32.
LRESULT OnChooseFontNotify(HWND hwnd, int /*idFrom*/, NMHDR* nm) {
    if (nm->idFrom == IDC_FONT_RESET_LINK && (nm->code == NM_CLICK || nm->code == NM_RETURN)) {
        if (ResetChooseFontToFallback(hwnd))
            g_resetToFallback = true;
        return 0;
    }

    return DefSubclassProc(hwnd, WM_NOTIFY, static_cast<WPARAM>(nm->idFrom), reinterpret_cast<LPARAM>(nm));
}

// Subclass on the ChooseFont dialog window itself: after comdlg32 has processed a change to the
// font/style/effects -- so its internal LOGFONT is current -- refresh the Sample (see the
// handlers above). Also services the initial-refresh message posted from the hook.
LRESULT CALLBACK ChooseFontDlgSubclass(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                       UINT_PTR id, DWORD_PTR /*ref*/) {
    switch (msg) {
        HANDLE_MSG(hwnd, WM_COMMAND, OnChooseFontCommand);
        HANDLE_MSG(hwnd, WM_NOTIFY,  OnChooseFontNotify);
        case kRefreshSampleMsg:
            RefreshChooseFontSample(hwnd);
            return 0;
        case WM_NCDESTROY:
            VERIFY(RemoveWindowSubclass(hwnd, ChooseFontDlgSubclass, id));
            break;
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// WM_INITDIALOG for the ChooseFont hook: track the dialog (for foregrounding), subclass the
// Sample control so it keeps the badge digits (see FontSampleSubclassProc), subclass the dialog
// itself for the live Sample refresh, post the initial refresh, and put the initial focus on the
// font-name combo. Returning FALSE keeps that focus (the dialog manager applies its own default
// focus only when WM_INITDIALOG returns TRUE) while comdlg32 still finishes its own init.
BOOL OnChooseFontInitDialog(HWND hwnd, HWND /*focus*/, LPARAM /*cf*/) {
    g_chooseFontDlg = hwnd;
    if (const HWND sample = GetDlgItem(hwnd, stc5)) {
        VERIFY(SetWindowSubclass(sample, FontSampleSubclassProc, 1, 0));
        VERIFY(SetWindowText(sample, kFontSample));
    }
    VERIFY(SetWindowSubclass(hwnd, ChooseFontDlgSubclass, 2, 0));
    VERIFY(PostMessage(hwnd, kRefreshSampleMsg, 0, 0));

    // Focus the font-name combo (cmb1) instead of the dialog's first tab stop -- which, because
    // the "Reset to default" SysLink precedes the combo in the template, would otherwise be that
    // link. Returning FALSE below keeps this focus rather than the dialog manager's default.
    if (const HWND nameCombo = GetDlgItem(hwnd, cmb1))
        SetFocus(nameCombo);

    return FALSE;
}

// ChooseFont hook: dispatch the dialog's messages via HANDLE_MSG crackers. Only WM_INITDIALOG is
// handled (see OnChooseFontInitDialog); returning 0 for everything else lets comdlg32 do its own
// default processing.
UINT_PTR CALLBACK ChooseFontHook(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        HANDLE_MSG(hwnd, WM_INITDIALOG, OnChooseFontInitDialog);
    }

    return 0;
}

// Seed g_workingFont from the dialog's own font (the fallback baseline: a valid LOGFONT
// with a sensible height/charset), marking no user selection.
void SeedFallbackFont(HWND dlg) {
    ZeroMemory(&g_workingFont, sizeof(g_workingFont));
    HFONT dlgFont = reinterpret_cast<HFONT>(SendMessage(dlg, WM_GETFONT, 0, 0));
    if (!dlgFont) dlgFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    VERIFY(GetObject(dlgFont, sizeof(g_workingFont), &g_workingFont));
    g_workingFontSet = false;
    MoveMemory(&g_fallbackFont, &g_workingFont, sizeof(g_fallbackFont));
}

// Refresh the IDC_FONT_NAME preview: show the chosen face name (or "Default" when unset),
// rendered in that face/style at the dialog font's height so any font stays legible.
void UpdateFontPreview(HWND dlg) {
    const HWND name = GetDlgItem(dlg, IDC_FONT_NAME);
    VERIFY(SetWindowText(name, g_workingFontSet ? g_workingFont.lfFaceName : TEXT("Default")));

    LOGFONT lf;
    ZeroMemory(&lf, sizeof(lf));
    MoveMemory(&lf, &g_workingFont, sizeof(lf));

    // Render at the label's own line height rather than the chosen size.
    HFONT dlgFont = reinterpret_cast<HFONT>(SendMessage(dlg, WM_GETFONT, 0, 0));
    if (dlgFont) {
        LOGFONT base;
        ZeroMemory(&base, sizeof(base));
        if (GetObject(dlgFont, sizeof(base), &base)) {
            lf.lfHeight = base.lfHeight;
            lf.lfWidth  = 0;
        }
    }

    const HFONT nf = CreateFontIndirect(&lf);
    if (nf) {
        SendMessage(name, WM_SETFONT, reinterpret_cast<WPARAM>(nf), TRUE);
        if (g_previewFont) DeleteObject(g_previewFont);
        g_previewFont = nf;
    }
}

} // namespace

namespace FontPicker {

void Init(HWND dlg, HINSTANCE inst) {
    g_inst = inst;

    // Seed the working font from the saved selection (over the dialog font as a baseline for
    // height/charset) or, when none is set, from the dialog font itself (the fallback), then
    // show it in the preview.
    SeedFallbackFont(dlg);
    if (Preferences::FontIsSet()) {
        const LOGFONT& f = Preferences::Font();
        lstrcpyn(g_workingFont.lfFaceName, f.lfFaceName, LF_FACESIZE);
        g_workingFont.lfWeight    = f.lfWeight;
        g_workingFont.lfItalic    = f.lfItalic;
        g_workingFont.lfUnderline = f.lfUnderline;
        g_workingFont.lfStrikeOut = f.lfStrikeOut;
        g_workingFont.lfCharSet   = DEFAULT_CHARSET;
        g_workingFontSet = true;
    }
    UpdateFontPreview(dlg);
}

void Choose(HWND dlg) {
    LOGFONT lf;
    ZeroMemory(&lf, sizeof(lf));
    MoveMemory(&lf, &g_workingFont, sizeof(lf));

    // No in-dialog reset yet this session; the "Reset to default font" link sets it.
    g_resetToFallback = false;

    CHOOSEFONT cf;
    ZeroMemory(&cf, sizeof(cf));
    cf.lStructSize    = sizeof(cf);
    cf.hwndOwner      = dlg;
    cf.hInstance      = g_inst;
    cf.lpTemplateName = kFontTemplate;
    cf.lpLogFont      = &lf;
    cf.lpfnHook       = ChooseFontHook;
    cf.Flags          = CF_INITTOLOGFONTSTRUCT | CF_EFFECTS | CF_SCREENFONTS |
                        CF_FORCEFONTEXIST | CF_ENABLEHOOK | CF_ENABLETEMPLATE;

    const BOOL chosen = ChooseFont(&cf);

    // The dialog has closed: stop tracking it and release the Sample font it used.
    g_chooseFontDlg = nullptr;
    if (g_sampleFont) { DeleteObject(g_sampleFont); g_sampleFont = nullptr; }

    if (chosen) {
        ZeroMemory(&g_workingFont, sizeof(g_workingFont));
        MoveMemory(&g_workingFont, &lf, sizeof(g_workingFont));
        // A pending in-dialog reset means the user chose "Default": clear rather than store.
        g_workingFontSet = !g_resetToFallback;
        UpdateFontPreview(dlg);
    }
}

HWND ActiveDialog() {
    return g_chooseFontDlg;
}

void Reset(HWND dlg) {
    SeedFallbackFont(dlg);
    UpdateFontPreview(dlg);
}

void Save() {
    if (g_workingFontSet) Preferences::SetFont(g_workingFont);
    else                  Preferences::ClearFont();
}

void Cleanup() {
    if (g_previewFont) { DeleteObject(g_previewFont); g_previewFont = nullptr; }
    if (g_sampleFont)  { DeleteObject(g_sampleFont);  g_sampleFont  = nullptr; }
}

} // namespace FontPicker
