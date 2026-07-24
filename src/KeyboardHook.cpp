#include "stdafx.h"
#include "KeyboardHook.h"
#include "Keyboard.h"

namespace {

HHOOK g_hook   = nullptr;
HWND  g_target = nullptr;

// Low-level keyboard hook. It posts a single visibility request to the target window
// and does no heavy work, so it always returns within the LowLevelHooksTimeout.
//
// Windows key handling:
//   * Any Win key-DOWN (physical or injected) shows the bar. We do not filter
//     injected downs, because in some setups (remapping tools, laptop firmware, VMs,
//     remote sessions) the Win events are delivered injected -- filtering them made
//     the bar fail to appear.
//   * On Win key-UP we hide only when the key is really released: a physical
//     (non-injected) up is trusted directly, while an injected up is honored only if
//     the key is actually up right now. This is the crucial bit: Windows sends a
//     synthetic (injected) Win up during Win+<key> chords to suppress the Start menu,
//     and it can also deliver the real release as injected -- gating the injected up
//     on the live key state hides on a genuine release (no lingering bar) yet keeps
//     the bar while Win is still physically held during Win+<number> app switching.
//
// No shown/hidden flag is kept here: Overlay::Show/Hide are idempotent, so we post
// unconditionally and encode the desired state (down=show, up=hide) in wParam.
LRESULT CALLBACK KeyHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const KBDLLHOOKSTRUCT* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        if (k->vkCode == VK_LWIN || k->vkCode == VK_RWIN) {
            if (!(k->flags & LLKHF_UP)) {
                PostMessage(g_target, WM_SHOW_WINNUMTIP, TRUE, 0);
            } else {
                const bool injected  = (k->flags & LLKHF_INJECTED) != 0;
                if (!injected || !Keyboard::IsWinDown())
                    PostMessage(g_target, WM_SHOW_WINNUMTIP, FALSE, 0);
            }
        }
    }

    return CallNextHookEx(nullptr, code, wp, lp);
}

} // namespace

namespace KeyboardHook {

bool Install(HINSTANCE inst, HWND target) {
    g_target = target;
    g_hook = SetWindowsHookEx(WH_KEYBOARD_LL, KeyHook, inst, 0);

    return g_hook != nullptr;
}

void Uninstall() {
    if (g_hook) { VERIFY(UnhookWindowsHookEx(g_hook)); g_hook = nullptr; }
}

} // namespace KeyboardHook
