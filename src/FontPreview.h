#pragma once

// The Preferences dialog's font preview (the IDC_FONT_NAME control). A stock static cannot
// mix two fonts on one line, so this module subclasses the control and takes over its
// painting: the chosen face name at the left in the dialog's own font -- so it stays legible
// whatever was picked, including symbol and display faces -- and "0123456789" at the right in
// the chosen face itself, shrunk to fit whatever width the name leaves free. Both runs are
// laid out by the ink they actually cover rather than by their advance width, so an italic or
// script face is neither clipped against the border nor cramped away from it. State is a
// single instance, matching the single Preferences dialog.
namespace FontPreview {

// Take over the control's painting. Call once from WM_INITDIALOG, before the first Update;
// the subclass removes itself when the control is destroyed.
void Init(HWND preview);

// Show 'name' -- also set as the window text, so GetWindowText and accessibility keep
// yielding the face name -- with the digits beside it in 'face', refitted to the width the
// name leaves free, then repaint.
void Update(HWND preview, LPCTSTR name, const LOGFONT& face);

// Release the digits font this module owns. Call from WM_DESTROY.
void Cleanup();

} // namespace FontPreview
