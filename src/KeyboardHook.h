#pragma once

// Global low-level Windows-key hook that tracks whether the Win-tip overlay should be
// visible. The hook does no work beyond flipping a single 32-bit atomic flag on each
// Win key-down/up, so it always returns well within the LowLevelHooksTimeout and never
// depends on the message queue being pumped (no PostMessage).
namespace KeyboardHook {

// Install the WH_KEYBOARD_LL hook. Returns true on success.
[[nodiscard]] bool Install(HINSTANCE inst);

// Remove the hook.
void Uninstall();

// Whether the overlay should currently be shown. Combines the hook's edge-detected
// flag (set on Win down/up) with the live physical key state, which self-heals a
// missed key-up or key-down. Poll this from a timer to drive Overlay::Show/Hide.
[[nodiscard]] bool ShouldShow();

} // namespace KeyboardHook
