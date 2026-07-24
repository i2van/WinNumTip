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

// Custom application messages, shared between the modules that post them (the
// keyboard hook / tray icon) and the message window that dispatches them. Defined as
// macros (not namespaced constants) so the windowsx.h HANDLE_MSG cracker can
// token-paste HANDLE_##message and use them as a switch's case labels.
#define WM_TRAYICON       (WM_APP + 1)   // tray icon callback
#define WM_SHOW_WINNUMTIP (WM_APP + 2)   // hook -> set overlay visibility (wParam: 1=show, 0=hide)

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
