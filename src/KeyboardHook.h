#pragma once

// Global low-level Windows-key hook that drives the overlay.
namespace KeyboardHook {

// Install the WH_KEYBOARD_LL hook; physical Win down/up posts WM_SHOW_WINNUMTIP
// (wParam 1=show / 0=hide) to 'target'. Returns true on success.
[[nodiscard]] bool Install(HINSTANCE inst, HWND target);

// Remove the hook.
void Uninstall();

} // namespace KeyboardHook
