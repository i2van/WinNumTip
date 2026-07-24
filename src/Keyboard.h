#pragma once

// Small shared keyboard-state helpers (header-only so they inline into every caller).
namespace Keyboard {

// True while either physical Windows key is currently held down. Used to gate the
// overlay: the hook trusts it to distinguish a genuine Win release from the synthetic
// injected up sent during Win+<key> chords, and the refresh timer polls it to self-heal
// when a Win key-up is never delivered.
[[nodiscard]] inline bool IsWinDown() {
    return GetAsyncKeyState(VK_LWIN) & 0x8000 || GetAsyncKeyState(VK_RWIN) & 0x8000;
}

} // namespace Keyboard
