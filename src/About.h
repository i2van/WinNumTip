#pragma once

// "About" dialog: application name/version, copyright, links to the README/project site,
// and a "Preferences" link that closes About and opens the Preferences dialog.
namespace About {

// Show the modal About dialog (from the IDD_ABOUT resource), owned by 'owner'.
void Show(HINSTANCE inst, HWND owner);

} // namespace About
