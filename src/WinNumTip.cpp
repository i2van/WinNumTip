// WinNumTip - show Win+<num> badges next to taskbar buttons while Win is held.
//
// Built as C++ without the C runtime (no stdlib): only Win32 / COM / UI Automation.
// This translation unit is the application entry point: it owns the process-wide
// state (COM apartment, UI Automation, the hidden message window) and wires the
// modules together (Taskbar / Overlay / Tray / Keyboard).

#include "stdafx.h"
#include "resource.h"
#include "Overlay.h"
#include "NotifyIcon.h"
#include "KeyboardHook.h"
#include "About.h"

// windowsx-style message cracker for the custom WM_TRAYICON message, so it can be
// dispatched with HANDLE_MSG just like the standard ones. Kept here as this is the
// only translation unit that dispatches it.
//   WM_TRAYICON handler signature:       void fn(HWND hwnd, UINT mouseMsg)
#define HANDLE_WM_TRAYICON(hwnd, wParam, lParam, fn) ((fn)((hwnd), (UINT)LOWORD(lParam)), 0L)

namespace {

LPCTSTR const kMsgClass = TEXT("WinNumTipMsg") APP_GUID;
// Single-instance mutex name; the shared app GUID keeps it globally unique.
LPCTSTR const kMutexName = TEXT("WinNumTip-Singleton") APP_GUID;

// Persistent poll that drives the overlay from the keyboard hook's atomic flag.
// Runs on the message window for the whole app lifetime (not just while shown), so a
// Win-down is always picked up and the state self-heals every tick.
constexpr UINT_PTR kPollTimer = 1;
constexpr UINT     kPollMs     = 75;

HINSTANCE      g_inst   = nullptr;
HWND           g_msgWnd = nullptr;
IUIAutomation* g_uia    = nullptr;

// Standard messages are dispatched through the windowsx.h HANDLE_MSG crackers;
// WM_TRAYICON is a custom (WM_APP-based) message handled the same way.
void OnCommand(HWND hwnd, int id, HWND /*ctl*/, UINT /*notify*/) {
    switch (id) {
        case IDM_ABOUT: About::Show(g_inst, hwnd); break;
        case IDM_EXIT:  DestroyWindow(hwnd);       break;
    }
}

void OnDestroy(HWND /*hwnd*/) {
    NotifyIcon::Remove();
    PostQuitMessage(0);
}

// Persistent poll (kPollTimer): reconcile the desired overlay visibility from the
// hook's atomic flag + live key state and apply it. Show/Hide are idempotent, so this
// is cheap when nothing changes and re-attempts Show if a previous create failed.
void OnTimer(HWND /*hwnd*/, UINT id) {
    if (id != kPollTimer) return;
    if (KeyboardHook::ShouldShow()) Overlay::Show(g_uia, g_inst);
    else                            Overlay::Hide();
}

LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // "TaskbarCreated" is a runtime-registered broadcast (Explorer restart), so it
    // can't be a HANDLE_MSG case label; handle it before the switch.
    if (NotifyIcon::HandleTaskbarCreated(msg)) return 0;

    switch (msg) {
        HANDLE_MSG(hwnd, WM_TIMER, OnTimer);
        HANDLE_MSG(hwnd, WM_TRAYICON, [](HWND h, UINT mouse) {
            if (mouse == WM_RBUTTONUP || mouse == WM_LBUTTONUP || mouse == WM_CONTEXTMENU)
                NotifyIcon::ShowMenu(g_inst, h);
        });
        HANDLE_MSG(hwnd, WM_COMMAND, OnCommand);
        HANDLE_MSG(hwnd, WM_DESTROY, OnDestroy);
        default: return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Entry point (no CRT). Linked via /ENTRY:Entry.
// ---------------------------------------------------------------------------
extern "C" void Entry() {
    // Enforce a single running instance via a named mutex (UUID-based name).
    CreateMutex(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) ExitProcess(0);

    // Per-Monitor-V2 DPI awareness is declared in WinNumTip.manifest (applied at
    // startup), so no SetProcessDpiAwarenessContext call is needed here.
    g_inst = GetModuleHandle(nullptr);

    // Hidden message-only window that hosts the tray icon and drives the overlay.
    WNDCLASSEX mc;
    ZeroMemory(&mc, sizeof(mc));
    mc.cbSize = sizeof(mc);
    mc.lpfnWndProc = MsgWndProc;
    mc.hInstance = g_inst;
    mc.lpszClassName = kMsgClass;
    VERIFY(RegisterClassEx(&mc));

    // Everything runs on this single (main) thread: COM apartment, UI Automation,
    // and the message pump. The overlay window is created on demand (on Win-down)
    // and destroyed on Win-up.
    VERIFY(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)));
    VERIFY(SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                     __uuidof(IUIAutomation), (void**)&g_uia)));
    Overlay::Init(g_inst);

    // Register the common controls we use (the SysLink in the About dialog) once, so
    // About::Show can create its modal dialog directly.
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LINK_CLASS;
    VERIFY(InitCommonControlsEx(&icc));

    g_msgWnd = CreateWindowEx(WS_EX_TOOLWINDOW, kMsgClass, TEXT(""), WS_POPUP,
                              0, 0, 0, 0, nullptr, nullptr, g_inst, nullptr);
    VERIFY(g_msgWnd != nullptr);
    NotifyIcon::Add(g_inst, g_msgWnd);
    VERIFY(KeyboardHook::Install(g_inst));
    // Drive the overlay by polling the hook's atomic flag + live key state.
    VERIFY(SetTimer(g_msgWnd, kPollTimer, kPollMs, nullptr));

    MSG m;
    while (GetMessage(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }

    KeyboardHook::Uninstall();
    Overlay::Shutdown();
    if (g_uia) { g_uia->Release(); g_uia = nullptr; }
    CoUninitialize();
    ExitProcess(0);
}
