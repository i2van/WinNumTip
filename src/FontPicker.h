#pragma once

// Font-selection helper for the Preferences dialog. It owns the "working" font -- the face
// and style chosen in the dialog but not yet persisted -- and the preview shown in the
// IDC_FONT_NAME control, and drives the ChooseFont common dialog (with the custom
// FONTSELECTORDLG template). State is a single instance, matching the single Preferences
// dialog; the parent forwards the relevant dialog messages to the calls below.
namespace FontPicker {

// Seed the working font from the saved preference (or, when none is set, the dialog's own
// font as a fallback) and show it in the preview. Call from WM_INITDIALOG; 'inst' owns the
// FONTSELECTORDLG template resource passed to ChooseFont.
void Init(HWND dlg, HINSTANCE inst);

// Open the ChooseFont common dialog; on OK adopt the selection as the working font and
// refresh the preview.
void Open(HWND dlg);

// Adopt a selection applied by the still-open ChooseFont dialog, refresh the preview, and
// persist just the font preference. A null selection means the taskbar-font fallback.
void ApplySelection(HWND dlg, const LOGFONT* selected);

// Reset the working font to the taskbar-font fallback and refresh the preview. The change
// is not persisted until Save (matching the dialog's OK/Apply/Cancel semantics).
void Reset(HWND dlg);

// Persist the working font to Preferences (SetFont, or ClearFont when using the fallback).
// Call on OK or Apply.
void Save();

// Whether the working font differs from the currently persisted font preference.
[[nodiscard]] bool HasChanges();

// Release the preview font this module owns. Call from WM_DESTROY.
void Cleanup();

} // namespace FontPicker
