#include "stdafx.h"
#include "FontPicker.h"
#include "FontPickerHelper.h"
#include "FontPreview.h"
#include "Preferences.h"
#include "resource.h"

namespace {

// The application instance that owns the FONTSELECTORDLG template resource (captured in
// Init); passed to the helper process so it can load the custom template.
HINSTANCE g_inst;

// The font currently chosen in the dialog and whether the user has picked one at all.
// Only face + style fields matter downstream (the overlay drives glyph height); the height
// carried here is just the dialog font's, used as a baseline when fitting the preview.
// When g_workingFontSet is false the overlay falls back to the taskbar font, and the
// preview names the face "Default".
LOGFONT g_workingFont;
bool g_workingFontSet;

// The default (fallback) font captured at Init -- the taskbar/dialog font shown as "Default".
LOGFONT g_fallbackFont;

// Seed g_workingFont from the dialog's own font (the fallback baseline: a valid LOGFONT
// with a sensible height/charset), marking no user selection.
void SeedFallbackFont(HWND dlg) {
    ZeroMemory(&g_workingFont, sizeof(g_workingFont));
    HFONT dlgFont = GetWindowFont(dlg);
    if (!dlgFont) dlgFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    VERIFY(WinAPI::Font::GetLogFont(dlgFont, g_workingFont));
    g_workingFontSet = false;
    MoveMemory(&g_fallbackFont, &g_workingFont, sizeof(g_fallbackFont));
}

// Refresh the IDC_FONT_NAME preview: it names the chosen face (or "Default" when unset) and
// shows the tip digits in that face.
void UpdateFontPreview(HWND dlg) {
    TCHAR def[16];
    FontPreview::Update(GetDlgItem(dlg, IDC_FONT_NAME),
                        g_workingFontSet ? g_workingFont.lfFaceName
                                         : WinAPI::String::Load(g_inst, IDS_DEFAULT, def),
                        g_workingFont);
}

void SetWorkingSelection(HWND dlg, const LOGFONT* selected) {
    if (selected) {
        MoveMemory(&g_workingFont, selected, sizeof(g_workingFont));
        g_workingFontSet = true;
    } else {
        SeedFallbackFont(dlg);
    }
    UpdateFontPreview(dlg);
}

// Build the saved font preference over the fallback baseline (which carries the sensible
// height/charset the preference itself does not store), reporting whether one is set at all.
// Requires g_fallbackFont, so call only after SeedFallbackFont.
bool SavedSelection(LOGFONT& saved) {
    MoveMemory(&saved, &g_fallbackFont, sizeof(saved));
    if (!Preferences::FontIsSet()) return false;

    const LOGFONT& f = Preferences::Font();
    WinAPI::String::Copy(saved.lfFaceName, f.lfFaceName);
    saved.lfWeight    = f.lfWeight;
    saved.lfItalic    = f.lfItalic;
    saved.lfUnderline = f.lfUnderline;
    saved.lfStrikeOut = f.lfStrikeOut;
    saved.lfCharSet   = DEFAULT_CHARSET;
    return true;
}

} // namespace

namespace FontPicker {

void Init(HWND dlg, HINSTANCE inst) {
    g_inst = inst;

    // The preview mixes two fonts on one line, which a stock static cannot do, so take over
    // its painting before the first refresh puts anything in it.
    FontPreview::Init(GetDlgItem(dlg, IDC_FONT_NAME));

    // Seed the working font from the saved selection (over the dialog font as a baseline for
    // height/charset) or, when none is set, from the dialog font itself (the fallback), then
    // show it in the preview.
    SeedFallbackFont(dlg);
    g_workingFontSet = SavedSelection(g_workingFont);
    UpdateFontPreview(dlg);
}

void Open(HWND dlg) {
    // The dialog opens on the working font, but its Apply button measures against the saved
    // one: the two differ whenever a previous OK adopted a selection that Preferences has not
    // persisted yet, and Apply must stay live for exactly that difference.
    LOGFONT applied;
    const bool appliedSet = SavedSelection(applied);

    LOGFONT selected;
    ZeroMemory(&selected, sizeof(selected));
    const FontPickerHelper::Result result = FontPickerHelper::Open(
        g_inst, dlg, g_workingFont, !g_workingFontSet, applied, !appliedSet,
        g_fallbackFont, selected);

    if (result == FontPickerHelper::Result::Chosen)
        SetWorkingSelection(dlg, &selected);
    else if (result == FontPickerHelper::Result::Default)
        SetWorkingSelection(dlg, nullptr);
}

void ApplySelection(HWND dlg, const LOGFONT* selected) {
    SetWorkingSelection(dlg, selected);
    Save();
}

void Reset(HWND dlg) {
    SetWorkingSelection(dlg, nullptr);
}

void Save() {
    if (g_workingFontSet) Preferences::SetFont(g_workingFont);
    else                  Preferences::ClearFont();
}

bool HasChanges() {
    if (g_workingFontSet != Preferences::FontIsSet()) return true;
    if (!g_workingFontSet) return false;

    const LOGFONT& saved = Preferences::Font();
    return lstrcmp(g_workingFont.lfFaceName, saved.lfFaceName) != 0 ||
           g_workingFont.lfWeight    != saved.lfWeight ||
           g_workingFont.lfItalic    != saved.lfItalic ||
           g_workingFont.lfUnderline != saved.lfUnderline ||
           g_workingFont.lfStrikeOut != saved.lfStrikeOut;
}

void Cleanup() {
    FontPreview::Cleanup();
}

} // namespace FontPicker
