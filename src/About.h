#pragma once

// "About" dialog: application name/version, copyright, and a link to the README.
namespace About {

// Show the modal About dialog (from the IDD_ABOUT resource), owned by 'owner'.
void Show(HINSTANCE inst, HWND owner);

} // namespace About
