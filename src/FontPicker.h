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
// refresh the preview. Call when the "Choose..." button is clicked.
void Choose(HWND dlg);

// The ChooseFont common dialog window while it is open (it runs modally over the Preferences
// dialog), or null when it is not open. Lets the parent bring the font dialog itself to the
// foreground on a repeat Preferences request, rather than its disabled owner.
[[nodiscard]] HWND ActiveDialog();

// Reset the working font to the taskbar-font fallback and refresh the preview. The change
// is not persisted until Save (matching the dialog's OK/Cancel semantics).
void Reset(HWND dlg);

// Persist the working font to Preferences (SetFont, or ClearFont when using the fallback).
// Call on OK.
void Save();

// Release the preview font this module owns. Call from WM_DESTROY.
void Cleanup();

} // namespace FontPicker
