#include "stdafx.h"
#include "FontPicker.h"
#include "FontPickerHelper.h"
#include "Preferences.h"
#include "resource.h"

namespace {

// The application instance that owns the FONTSELECTORDLG template resource (captured in
// Init); passed to the helper process so it can load the custom template.
HINSTANCE g_inst = nullptr;

// The font currently chosen in the dialog and whether the user has picked one at all.
// Only face + style fields matter downstream (the overlay drives glyph height); the height
// carried here is just the dialog font's, used to render the preview at a legible size.
// When g_workingFontSet is false the overlay falls back to the taskbar font, and the
// preview shows "Default" in the dialog's own font.
LOGFONT g_workingFont = { 0 };
bool g_workingFontSet = false;

// The default (fallback) font captured at Init -- the taskbar/dialog font shown as "Default".
LOGFONT g_fallbackFont = { 0 };

// The font that renders the IDC_FONT_NAME preview in the chosen face; owned here and
// rebuilt on every change, freed by Cleanup.
HFONT g_previewFont = nullptr;

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
    TCHAR def[16];
    VERIFY(SetWindowText(name, g_workingFontSet ? g_workingFont.lfFaceName
                                                : LoadStr(g_inst, IDS_DEFAULT, def)));

    LOGFONT lf;
    ZeroMemory(&lf, sizeof(lf));
    MoveMemory(&lf, &g_workingFont, sizeof(lf));

    // Render at the label's own line height rather than the chosen size.
    if (const HFONT dlgFont = reinterpret_cast<HFONT>(SendMessage(dlg, WM_GETFONT, 0, 0))) {
        LOGFONT base;
        ZeroMemory(&base, sizeof(base));
        if (GetObject(dlgFont, sizeof(base), &base)) {
            lf.lfHeight = base.lfHeight;
            lf.lfWidth  = 0;
        }
    }

    if (const HFONT nf = CreateFontIndirect(&lf)) {
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

void Open(HWND dlg) {
    LOGFONT selected;
    ZeroMemory(&selected, sizeof(selected));
    const FontPickerHelper::Result result = FontPickerHelper::Open(g_inst, dlg, g_workingFont, g_fallbackFont, selected);

    if (result == FontPickerHelper::Result::Chosen) {
        MoveMemory(&g_workingFont, &selected, sizeof(g_workingFont));
        g_workingFontSet = true;
        UpdateFontPreview(dlg);
    } else if (result == FontPickerHelper::Result::Default) {
        SeedFallbackFont(dlg);
        UpdateFontPreview(dlg);
    }
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
}

} // namespace FontPicker
