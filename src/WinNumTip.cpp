// WinNumTip - show Win+<num> badges next to taskbar buttons while Win is held.
//
// Built as C++ without the C runtime (no stdlib): only Win32 / COM / UI Automation.
// This translation unit is the application entry point: it owns the process-wide
// state (COM apartment, UI Automation, the hidden message window) and wires the
// modules together (Taskbar / Overlay / Tray / Keyboard).

#include "stdafx.h"

#include "About.h"
#include "FontPickerHelper.h"
#include "KeyboardHook.h"
#include "NotifyIcon.h"
#include "Overlay.h"
#include "Preferences.h"
#include "PreferencesDialog.h"
#include "resource.h"

// windowsx-style message cracker for the custom WM_TRAYICON message, so it can be
// dispatched with HANDLE_MSG just like the standard ones. Kept here as this is the
// only translation unit that dispatches it.
//   WM_TRAYICON handler signature:       void fn(HWND hwnd, UINT mouseMsg)
#define HANDLE_WM_TRAYICON(hwnd, wParam, lParam, fn) ((fn)((hwnd), (UINT)LOWORD(lParam)), 0L)

namespace {

constexpr LPCTSTR kMsgClass = TEXT("WinNumTipMsg") APP_GUID;
// Single-instance mutex name; the shared app GUID keeps it globally unique.
constexpr LPCTSTR kMutexName = TEXT("WinNumTip-Singleton") APP_GUID;

// Persistent poll that drives the overlay from the keyboard hook's atomic flag. Runs on
// the message window for the whole app lifetime (not just while shown), so a Win-down is
// always picked up and the state self-heals every tick. Its interval is the user's "poll
// interval" preference (Preferences::PollIntervalMs), re-armed when Preferences is accepted.
constexpr UINT_PTR kPollTimer = 1;

HINSTANCE      g_inst   = nullptr;
HWND           g_msgWnd = nullptr;
IUIAutomation* g_uia    = nullptr;

// Arm (or re-arm) the persistent poll timer on the message window with the current "poll
// interval" preference. Re-arming replaces the running timer's period, so this is used both
// at startup and after the preference may have changed (Preferences accepted).
void ArmPollTimer() {
    VERIFY(SetTimer(g_msgWnd, kPollTimer, Preferences::PollIntervalMs(), nullptr));
}

// Standard messages are dispatched through the windowsx.h HANDLE_MSG crackers;
// WM_TRAYICON is a custom (WM_APP-based) message handled the same way.
void OnCommand(HWND hwnd, int id, HWND /*ctl*/, UINT /*notify*/) {
    switch (id) {
        case IDM_PREFERENCES:
            // Re-arm the poll timer with the (possibly changed) interval when the dialog is
            // accepted; the persistent timer would otherwise keep running at the old rate.
            if (PreferencesDialog::Show(g_inst, hwnd) == IDOK) ArmPollTimer();
            break;
        case IDM_ABOUT:       About::Show(g_inst, hwnd);             break;
        case IDM_EXIT:        DestroyWindow(hwnd);                   break;
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
            bool isLeftButton = mouse == WM_LBUTTONUP;
            if (mouse == WM_RBUTTONUP || isLeftButton || mouse == WM_CONTEXTMENU) {
                const HWND fontDialog = FontPickerHelper::ActiveDialog();
                if (fontDialog) (void)FontPickerHelper::ActivateDialog();
                NotifyIcon::ShowMenu(g_inst, !isLeftButton, h, fontDialog);
            }
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
    g_inst = GetModuleHandle(nullptr);
    if (FontPickerHelper::RunIfRequested(g_inst)) {
        // The system font dialog can leave font-cache worker threads inside system DLLs.
        // A normal ExitProcess may deadlock while running their detach callbacks, so the
        // disposable helper terminates after publishing its shared-memory result.
        TerminateProcess(GetCurrentProcess(), 0);
        ExitProcess(0);
    }

    // Enforce a single running instance via a named mutex (UUID-based name).
    CreateMutex(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) ExitProcess(0);

    // Per-Monitor-V2 DPI awareness is declared in WinNumTip.manifest (applied at
    // startup), so no SetProcessDpiAwarenessContext call is needed here.
    // Harden timer callbacks before any SetTimer (the poll timer below and the overlay's
    // refresh timer): by default 64-bit Windows silently swallows exceptions raised inside
    // a timer (or window) callback, which can mask bugs and hide security issues. Opt out of
    // that legacy suppression process-wide so such exceptions surface normally. The flag is
    // unsupported on pre-2004 Windows 10, where the call simply fails (hence no VERIFY).
    BOOL suppressTimerProcExceptions = FALSE;
    SetUserObjectInformationW(GetCurrentProcess(), UOI_TIMERPROC_EXCEPTION_SUPPRESSION, &suppressTimerProcExceptions, sizeof(suppressTimerProcExceptions));

    // Load persisted preferences (INI next to the exe) before the overlay is first shown.
    Preferences::Load();

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

    // Register the common controls we use -- the SysLink (in the About dialog and the
    // Preferences dialog's "Reset to defaults" link) and the trackbar (slider) in the
    // Preferences dialog -- once, so those modal dialogs can be created directly.
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LINK_CLASS | ICC_BAR_CLASSES;
    VERIFY(InitCommonControlsEx(&icc));

    g_msgWnd = CreateWindowEx(WS_EX_TOOLWINDOW, kMsgClass, TEXT(""), WS_POPUP,
                              0, 0, 0, 0, nullptr, nullptr, g_inst, nullptr);
    VERIFY(g_msgWnd != nullptr);
    NotifyIcon::Add(g_inst, g_msgWnd);
    VERIFY(KeyboardHook::Install(g_inst));
    // Drive the overlay by polling the hook's atomic flag + live key state.
    ArmPollTimer();

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
