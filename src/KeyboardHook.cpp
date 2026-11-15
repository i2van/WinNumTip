#include "stdafx.h"
#include "KeyboardHook.h"

namespace {

// The dedicated thread hosting the hook (see Install) and its id, for Uninstall's WM_QUIT.
HANDLE g_thread;
DWORD  g_threadId;

// Visibility state shared between the hook thread and the poll on the main thread, kept as
// press generations rather than a flag. Each cross-thread variable is a naturally aligned
// word with a SINGLE writer (the hook thread), so no thread can overwrite another's update
// and no Interlocked read-modify-write is needed (the Interlocked intrinsics are
// out-of-line CRT calls on ARM64, which this /NODEFAULTLIB build cannot link). Plain
// volatile is not enough, though: ARM64 defaults to /volatile:iso, which gives no ordering.
// So the hook publishes with WriteULongRelease and the poll reads with ReadULongAcquire
// (winnt.h force-inlined intrinsics: ldar/stlr on ARM64, compiler barriers on x86/x64).
// The poll acquires g_releaseGen BEFORE g_pressGen, so the press it then reads is never
// older than the one that release recorded. The bar is desired while the latest press has
// been neither released (hook) nor cleared as stuck (backstop); because the backstop
// clears a specific generation, it can never cancel a newer press the hook registered
// meanwhile.
//
// Showing is edge-driven by the hook (a Win down starts a new generation, a trusted
// physical Win up releases it) and is the SOLE authority for show/hide. It is deliberately
// NOT gated on GetAsyncKeyState in the show path: Windows modifier keys do not
// auto-repeat, so while the Win key is held there are no further key events and the live
// async state can briefly (or, on some keyboards/remappers/VMs, persistently) read "up"
// even though the key is physically down -- using it to gate showing made the bar fail to
// appear while Win was held.
volatile DWORD g_pressGen;    // hook thread: count of Win key-downs (wraps harmlessly)
volatile DWORD g_releaseGen;  // hook thread: g_pressGen at the last trusted physical Win up
DWORD          g_clearGen;    // main thread only: g_pressGen the backstop cleared as stuck

// Read the hook's generations (see above for the order) and report whether the latest
// press is still pending -- neither released nor cleared -- returning that press in 'press'.
[[nodiscard]] bool PressPending(DWORD& press) {
    const DWORD released = ReadULongAcquire(&g_releaseGen);
    press = ReadULongAcquire(&g_pressGen);
    return press != released && press != g_clearGen;
}

// Debounced backstop counter (poll ticks the Win key has read released while the bar is
// still desired), and the press generation it has been counting against. Only used to
// clear a stuck bar when a physical Win up was never seen by the hook -- either it was
// never delivered (e.g. Win+L switches to the secure desktop) or the release arrived
// injected (remapper/VM) and so was ignored by the hook. Main thread only. See ShouldShow.
int            g_upTicks;
DWORD          g_upTicksGen;
constexpr int  kUpTicksToHide  = 10;   // ~0.75s at the message window's 75ms poll

// Low-level keyboard hook. It only advances the press generations on Win key-down/up and
// returns, doing no heavy work and touching no message queue, so it always returns within
// the LowLevelHooksTimeout.
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
                WriteULongRelease(&g_pressGen, ReadULongNoFence(&g_pressGen) + 1);   // Win down -> show (new press)
            else if (!(k->flags & LLKHF_INJECTED))
                WriteULongRelease(&g_releaseGen, ReadULongNoFence(&g_pressGen));   // trusted physical up -> hide
        }
    }

    return CallNextHookEx(nullptr, code, wp, lp);
}

// Handshake between Install and the hook thread: 'inst' (the module hosting the hook) goes
// in, 'installed' comes out, and 'ready' is set once that result is known. Lives on Install's stack,
// which is safe because the thread stops touching it once 'ready' is set.
struct HookStart {
    HINSTANCE inst;
    HANDLE    ready;
    bool      installed;
};

// The hook thread. A WH_KEYBOARD_LL callback runs on the thread that installed the hook,
// and only while that thread is waiting for messages; Windows holds up every keyboard
// event system-wide until it returns. The main thread can block for a while -- a
// slide/roll/expand show animation (AnimateWindow) or UI Automation calls -- so the hook
// gets a thread of its own that does nothing but wait, and keystrokes never stall.
DWORD WINAPI HookThread(LPVOID param) {
    HookStart* const start = static_cast<HookStart*>(param);

    // Create this thread's message queue before reporting back, so Uninstall's WM_QUIT
    // cannot be posted before there is a queue to receive it.
    MSG m;
    (void)PeekMessage(&m, nullptr, 0, 0, PM_NOREMOVE);

    const HHOOK hook = SetWindowsHookEx(WH_KEYBOARD_LL, KeyHook, start->inst, 0);
    start->installed = hook != nullptr;
    VERIFY(SetEvent(start->ready));
    if (!hook) return 0;

    while (GetMessage(&m, nullptr, 0, 0) > 0) DispatchMessage(&m);

    VERIFY(UnhookWindowsHookEx(hook));
    return 0;
}

} // namespace

namespace KeyboardHook {

bool Install(HINSTANCE inst) {
    HookStart start = { .inst = inst, .ready = CreateEvent(nullptr, TRUE, FALSE, nullptr), .installed = false };
    ASSERT(start.ready);
    if (!start.ready) return false;

    g_thread = CreateThread(nullptr, 0, HookThread, &start, 0, &g_threadId);
    ASSERT(g_thread);
    if (g_thread) VERIFY(WaitForSingleObject(start.ready, INFINITE) == WAIT_OBJECT_0);
    VERIFY(CloseHandle(start.ready));

    // A thread that failed to install the hook has already exited (or is about to).
    if (g_thread && !start.installed) {
        VERIFY(WaitForSingleObject(g_thread, INFINITE) == WAIT_OBJECT_0);
        VERIFY(CloseHandle(g_thread));
        g_thread = nullptr;
    }

    return start.installed;
}

void Uninstall() {
    if (!g_thread) return;
    VERIFY(PostThreadMessage(g_threadId, WM_QUIT, 0, 0));
    VERIFY(WaitForSingleObject(g_thread, INFINITE) == WAIT_OBJECT_0);
    VERIFY(CloseHandle(g_thread));
    g_thread = nullptr;
}

bool ShouldShow() {
    // The hook's press generations are authoritative: show while the latest Win press is
    // unreleased. This makes the bar appear reliably on Win-down and stay up for the whole
    // hold, even where the live async key state is unreliable while the key is held.
    //
    // GetAsyncKeyState is used ONLY as a debounced hide backstop for the case where a
    // physical Win up is never seen by the hook -- never delivered (e.g. Win+L switches
    // to the secure desktop) or the release arrived injected and was ignored -- which
    // would otherwise leave the bar stuck on. We clear the press only after the key has
    // read released for kUpTicksToHide consecutive polls (~0.75s); the debounce keeps a
    // transient async "up" blip from hiding the bar mid-hold, and the normal hide path
    // is the hook's physical Win-up edge, not this backstop.
    // Tick the backstop only while the bar is shown AND the key reads physically up;
    // any other state (bar hidden, or key still held) resets the debounce. Checking
    // the generations first also skips the GetAsyncKeyState call entirely while hidden --
    // the common idle state. The !IsWinDown() guard is essential: it must gate the
    // counter so a physically-held key never ticks toward a hide (no mid-hold
    // disappearance). The count is tied to the press it started on, so a newer Win down
    // restarts it, and the clear records that same press: if the hook thread registered
    // a newer one meanwhile, it differs from g_clearGen and stays shown. The result is
    // re-read after any clear for the same reason, so such a press is not hidden for a
    // poll interval.
    DWORD press;
    if (PressPending(press) && !WinAPI::Keyboard::IsWinDown()) {
        if (press != g_upTicksGen) { g_upTicksGen = press; g_upTicks = 0; }
        if (++g_upTicks >= kUpTicksToHide) {
            g_clearGen = press;
            g_upTicks  = 0;
        }
    } else {
        g_upTicks = 0;
    }

    DWORD latest;
    return PressPending(latest);
}

} // namespace KeyboardHook
