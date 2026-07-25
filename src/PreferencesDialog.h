#pragma once

// The modal Preferences dialog (IDD_PREFERENCES): edits the persisted user preferences
// (today, the overlay "label size"). The values themselves live in the Preferences model.
namespace PreferencesDialog {

// Show the modal Preferences dialog, owned by 'owner'. Only one instance is shown at a
// time; a repeat request re-focuses the existing dialog.
void Show(HINSTANCE inst, HWND owner);

} // namespace PreferencesDialog
