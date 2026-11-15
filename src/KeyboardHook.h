#pragma once

// Global low-level Windows-key hook that tracks whether the Win-tip overlay should be
// visible. The hook runs on a dedicated thread (so a busy main thread never delays
// keyboard input) and does no work beyond storing a word-sized press counter on each Win
// key-down/up, so it always returns well within the LowLevelHooksTimeout and
// never depends on the main message queue being pumped (no PostMessage).
namespace KeyboardHook {

// Start the hook thread and install the WH_KEYBOARD_LL hook on it. Returns true on
// success.
[[nodiscard]] bool Install(HINSTANCE inst);

// Remove the hook and end its thread.
void Uninstall();

// Whether the overlay should currently be shown. Combines the hook's edge-detected
// press state (set on Win down/up) with the live physical key state, which self-heals a
// missed key-up or key-down. Poll this from a timer to drive Overlay::Show/Hide.
[[nodiscard]] bool ShouldShow();

} // namespace KeyboardHook
