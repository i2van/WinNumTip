// stdafx.h - precompiled header for WinNumTip.
// UNICODE / _UNICODE are defined by the project, not here.
#pragma once

// Target Windows 10+ (required for Per-Monitor-V2 DPI, GetSystemMetricsForDpi,
// SystemParametersInfoForDpi, SHQueryUserNotificationState, etc.).
#define WINVER        0x0A00
#define _WIN32_WINNT  0x0A00
#define NTDDI_VERSION 0x0A000006   // NTDDI_WIN10_RS5

#include <windows.h>
#include <windowsx.h>   // HANDLE_MSG message crackers
#include <shellapi.h>   // Shell_NotifyIcon / NOTIFYICONDATA / SHAppBarMessage
#include <uxtheme.h>    // themed (native) button drawing + buffered paint
#include <vssym32.h>    // TASKBARPARTS (TBP_*) + TMT_* theme property ids
#include <objbase.h>    // COM (CoInitializeEx / CoCreateInstance)
#include <uiautomation.h>  // IUIAutomation (taskbar button rects)
#include <commctrl.h>   // SysLink control (NMLINK) + InitCommonControlsEx
#include <dlgs.h>       // ChooseFont common-dialog template control ids (stc5, cmb1, ...)

// Custom application message posted by the shell to the message window and dispatched
// there. Defined as a macro (not a namespaced constant) so the windowsx.h HANDLE_MSG
// cracker can token-paste HANDLE_##message and use it as a switch's case label.
#define WM_TRAYICON       (WM_APP + 1)   // tray icon callback

// A single app-wide GUID appended to every unique, globally-named resource this
// process creates (window classes, the single-instance mutex, and any future
// events/file-mappings), so those names cannot collide with another process's. Use
// via adjacent string-literal concatenation, e.g. TEXT("WinNumTipOverlay") APP_GUID.
#define APP_GUID TEXT("-3F2E7A94-6B1D-4C8E-9A05-D71E2F6B0C43")

// No-CRT memory support. We build with /NODEFAULTLIB, so ZeroMemory maps to the
// ntdll RtlZeroMemory declared below (RtlFill/RtlMove are declared for symmetry).
// We do NOT provide memset/memcpy: the current code never causes the compiler to
// emit external calls to them (zeroing goes through ZeroMemory/RtlZeroMemory, and
// Release inlines memory ops under /GL). If a future change makes the compiler
// emit memset/memcpy (e.g. a large aggregate init or struct-by-value copy), the
// link will fail with an unresolved symbol -- re-add a small NoCrt.cpp defining
// them (forwarding to RtlFillMemory/RtlMoveMemory), compiled without /GL and
// with #pragma function(memset, memcpy).
#undef RtlFillMemory
#undef RtlMoveMemory
#undef RtlZeroMemory

extern "C" {
    NTSYSAPI void NTAPI RtlFillMemory(void* dst, SIZE_T length, UCHAR fill);
    NTSYSAPI void NTAPI RtlMoveMemory(void* dst, const void* src, SIZE_T length);
    NTSYSAPI void NTAPI RtlZeroMemory(void* dst, SIZE_T length);
}

// VERIFY(expr): evaluate 'expr' exactly once in every build (so the wrapped call
// always runs), and in Debug additionally assert that it succeeded -- breaking
// into the debugger when it is false/zero. In Release it is just the bare
// expression, so calls that "should not fail" stay in the shipping build without
// any assertion overhead. Use it for API calls whose failure is a programming
// error rather than an expected runtime condition.
#ifdef _DEBUG
    #define VERIFY(expr) ((expr) ? (void)0 : (OutputDebugStringA("VERIFY failed: " #expr "\n"), __debugbreak()))
#else
    #define VERIFY(expr) ((void)(expr))
#endif

// VERIFY_SHELLEXEC(expr): like VERIFY, but for ShellExecute-style calls whose success
// is a returned value strictly greater than 32 (values <= 32 are the HINSTANCE error
// sentinels, e.g. SE_ERR_*). Evaluates 'expr' exactly once in every build (so the call
// always runs); in Debug it additionally asserts the result is > 32, breaking into the
// debugger otherwise. In Release it is just the bare call.
#ifdef _DEBUG
    #define VERIFY_SHELLEXEC(expr) (((INT_PTR)(expr) > 32) ? (void)0 : (OutputDebugStringA("VERIFY_SHELLEXEC failed: " #expr "\n"), __debugbreak()))
#else
    #define VERIFY_SHELLEXEC(expr) ((void)(expr))
#endif

// OpenUrl(owner, url): open 'url' with the shell's default handler (a web browser for
// http/https links), owned by 'owner'. Wrapped in VERIFY_SHELLEXEC so a failure (a
// ShellExecute result <= 32) breaks into the debugger in Debug and is a no-op check in
// Release.
inline void OpenUrl(HWND owner, LPCTSTR url) {
    VERIFY_SHELLEXEC(ShellExecute(owner, TEXT("open"), url, nullptr, nullptr, SW_SHOWNORMAL));
}

// OpenUrlOnContextHelp(dlg, cmd, x, y, url): shared WM_SYSCOMMAND body for the DS_CONTEXTHELP
// dialogs. The title-bar "?" (context-help) button opens 'url' -- the dialog's README section
// -- instead of entering Windows' per-control help mode; every other system command (move,
// close, ...) is forwarded to the default handler unchanged. Each dialog's cracker-dispatched
// OnSysCommand forwards here with the URL it documents (see HANDLE_WM_SYSCOMMAND callers).
inline void OpenUrlOnContextHelp(HWND dlg, UINT cmd, int x, int y, LPCTSTR url) {
    if ((cmd & 0xFFF0) == SC_CONTEXTHELP)
        OpenUrl(dlg, url);
    else
        FORWARD_WM_SYSCOMMAND(dlg, cmd, x, y, DefWindowProc);
}

// ForegroundDialog(dlg[, top]): bring an already-open modal dialog back to the foreground on
// a repeat "open" request, so a second instance is never created. Prefers 'top' when given
// -- e.g. a common dialog (ChooseFont) running over 'dlg', whose owner is disabled while the
// common dialog is modal -- otherwise 'dlg's last active popup, which resurfaces a child
// dialog opened from 'dlg' (GetLastActivePopup returns 'dlg' itself when it owns no popups).
inline void ForegroundDialog(HWND dlg, HWND top = nullptr) {
    SetForegroundWindow(top ? top : GetLastActivePopup(dlg));
}

// LoadStr(inst, id, buffer): load the string resource 'id' from 'inst' into the fixed-size
// stack 'buffer', with its capacity deduced as the template size parameter N. buffer[0] is
// cleared first so a missing or empty resource leaves a valid, null-terminated empty string
// rather than uninitialized stack memory -- callers can use the result unconditionally.
// Returns 'buffer' so it can be passed straight to an API (e.g. MessageBox, SetWindowText).
template <int N>
inline LPCTSTR LoadStr(HINSTANCE inst, UINT id, TCHAR (&buffer)[N]) {
    buffer[0] = 0;
    LoadString(inst, id, buffer, N);
    return buffer;
}
