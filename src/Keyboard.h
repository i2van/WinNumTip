#pragma once

// Small shared keyboard-state helpers (header-only so they inline into every caller).
namespace Keyboard {

// True while either physical Windows key is currently held down. Used to gate the
// overlay: the hook trusts it to distinguish a genuine Win release from the synthetic
// injected up sent during Win+<key> chords, and the refresh timer polls it to self-heal
// when a Win key-up is never delivered.
[[nodiscard]] inline bool IsWinDown() {
    // GetAsyncKeyState sets this high-order bit in its result while the key is physically down.
    constexpr int kKeyDownBit = 0x8000;

    return GetAsyncKeyState(VK_LWIN) & kKeyDownBit || GetAsyncKeyState(VK_RWIN) & kKeyDownBit;
}

} // namespace Keyboard
