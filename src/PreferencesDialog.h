#pragma once

// The modal Preferences dialog (IDD_PREFERENCES): edits the persisted user preferences
// (overlay label size, invert colors, bold font, and the refresh/poll timer intervals).
// The values themselves live in the Preferences model.
namespace PreferencesDialog {

// Show the modal Preferences dialog, owned by 'owner'. Only one instance is shown at a
// time; a repeat request re-focuses the existing dialog. Returns the dialog result
// (IDOK when the user confirmed changes, otherwise IDCANCEL) so the caller can re-apply
// settings that need it (e.g. re-arm the poll timer).
INT_PTR Show(HINSTANCE inst, HWND owner);

} // namespace PreferencesDialog
