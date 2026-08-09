#pragma once

// The modal Preferences dialog (IDD_PREFERENCES): edits the persisted user preferences
// (overlay tip size, opacity, invert colors, font, and the refresh/poll timer intervals).
// The values themselves live in the Preferences model.
namespace PreferencesDialog {

// Show the modal Preferences dialog, owned by 'owner'. Only one instance is shown at a
// time; a repeat request re-focuses the existing dialog. Apply saves without closing and
// forwards its IDC_APPLY WM_COMMAND to the owner. Returns IDOK when the dialog closes
// through OK, otherwise IDCANCEL, so the caller can also re-apply settings saved by OK.
[[nodiscard]] INT_PTR Show(HINSTANCE inst, HWND owner);

// Bring the already-open Preferences dialog to the foreground, along with its ChooseFont
// helper dialog (see FontPickerHelper::ActivateDialog) when one is open over it, so a
// tray-icon click while switched away to another app surfaces both windows together rather
// than just whichever one is topmost. Returns false when Preferences is not currently open.
[[nodiscard]] bool ActivateDialog();

} // namespace PreferencesDialog
