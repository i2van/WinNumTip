#include "stdafx.h"
#include "KeyboardHook.h"
#include "Keyboard.h"

namespace {

HHOOK g_hook;

// Platform-width visibility flag shared between the hook callback and the poll.
// LONG_PTR is the architecture's natural register width (32-bit on Win32, 64-bit on
// x64/ARM64); a naturally-aligned word-sized load/store is a single atomic instruction
// (no torn values) on every supported architecture, so no Interlocked/PostMessage is
// needed: the hook just publishes the desired state and the poll reads the latest one.
// (The WH_KEYBOARD_LL callback also runs on the installing thread, i.e. the same thread
// as the poll, so there is no cross-thread race here.)
//
// g_desired is edge-driven by the hook (kShow on Win down, kHide on a trusted physical
// Win up) and is the SOLE authority for show/hide. It is deliberately NOT gated on
// GetAsyncKeyState in the show path: Windows modifier keys do not auto-repeat, so while
// the Win key is held there are no further key events and the live async state can
// briefly (or, on some keyboards/remappers/VMs, persistently) read "up" even though the
// key is physically down -- using it to gate showing made the bar fail to appear while
// Win was held.
constexpr LONG_PTR kHide = 0;   // overlay should be hidden
constexpr LONG_PTR kShow = 1;   // overlay should be visible
volatile  LONG_PTR g_desired = kHide;

// Debounced backstop counter (poll ticks the Win key has read released while the bar is
// still desired). Only used to clear a stuck bar when a physical Win up was never seen
// by the hook -- either it was never delivered (e.g. Win+L switches to the secure
// desktop) or the release arrived injected (remapper/VM) and so was ignored by the
// hook. See ShouldShow.
int            g_upTicks;
constexpr int  kUpTicksToHide  = 10;   // ~0.75s at the message window's 75ms poll

// Low-level keyboard hook. It only flips g_desired for Win key-down/up and returns,
// doing no heavy work and touching no message queue, so it always returns within the
// LowLevelHooksTimeout.
//
// Windows key handling:
//   * Any Win key-DOWN (physical or injected) requests show. We do not filter injected
//     downs, because in some setups (remapping tools, laptop firmware, VMs, remote
//     sessions) the Win events are delivered injected -- filtering them made the bar
//     fail to appear.
//   * A trusted physical (non-injected) Win key-UP requests hide immediately.
//   * An injected Win key-UP is IGNORED here. Windows injects a synthetic Win up during
//     Win+<key> chords to suppress the Start menu while the key is still physically
//     held, and gating that injected up on the live async key state proved unreliable
//     (the async state can lag or, on some keyboards/remappers/VMs, misreport), which
//     made the bar hide mid-hold or only show while the Start menu was open. Genuine
//     releases that arrive injected are instead caught by the debounced async backstop
//     in ShouldShow, so we never depend on a single async read to hide.
LRESULT CALLBACK KeyHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const KBDLLHOOKSTRUCT* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        if (k->vkCode == VK_LWIN || k->vkCode == VK_RWIN) {
            if (!(k->flags & LLKHF_UP))
                g_desired = kShow;                      // Win down -> show
            else if (!(k->flags & LLKHF_INJECTED))
                g_desired = kHide;                      // trusted physical up -> hide
        }
    }

    return CallNextHookEx(nullptr, code, wp, lp);
}

} // namespace

namespace KeyboardHook {

bool Install(HINSTANCE inst) {
    g_hook = SetWindowsHookEx(WH_KEYBOARD_LL, KeyHook, inst, 0);

    return g_hook != nullptr;
}

void Uninstall() {
    if (g_hook) { VERIFY(UnhookWindowsHookEx(g_hook)); g_hook = nullptr; }
}

bool ShouldShow() {
    // g_desired (edge-set by the hook) is authoritative: show whenever it is set. This
    // makes the bar appear reliably on Win-down and stay up for the whole hold, even
    // where the live async key state is unreliable while the key is held.
    //
    // GetAsyncKeyState is used ONLY as a debounced hide backstop for the case where a
    // physical Win up is never seen by the hook -- never delivered (e.g. Win+L switches
    // to the secure desktop) or the release arrived injected and was ignored -- which
    // would otherwise leave the bar stuck on. We clear the flag only after the key has
    // read released for kUpTicksToHide consecutive polls (~0.75s); the debounce keeps a
    // transient async "up" blip from hiding the bar mid-hold, and the normal hide path
    // is the hook's physical Win-up edge, not this backstop.
    // Tick the backstop only while the bar is shown AND the key reads physically up;
    // any other state (bar hidden, or key still held) resets the debounce. Checking
    // g_desired first also skips the GetAsyncKeyState call entirely while hidden -- the
    // common idle state. The !IsWinDown() guard is essential: it must gate the counter
    // so a physically-held key never ticks toward a hide (no mid-hold disappearance).
    if (g_desired == kShow && !Keyboard::IsWinDown()) {
        if (++g_upTicks >= kUpTicksToHide) { g_desired = kHide; g_upTicks = 0; }
    } else {
        g_upTicks = 0;
    }

    return g_desired == kShow;
}

} // namespace KeyboardHook
