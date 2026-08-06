#pragma once

// Small shared helper for giving a window the application icon.
namespace DialogIcon {

// Set 'wnd's large and small window icons to the application icon (IDI_APPICON), so
// Alt+Tab and the taskbar show it instead of the generic default. Used by the dialogs
// (About, Preferences, the font picker) whose owner -- the hidden message window, or a
// hidden per-dialog helper window -- has no class icon.
void Set(HWND wnd, HINSTANCE inst);

} // namespace DialogIcon
