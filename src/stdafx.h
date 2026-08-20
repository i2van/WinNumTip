// stdafx.h - precompiled header for WinNumTip.
// UNICODE / _UNICODE are defined by the project, not here.
#pragma once

// Target Windows 10+ (required for Per-Monitor-V2 DPI, GetSystemMetricsForDpi,
// SystemParametersInfoForDpi, SHQueryUserNotificationState, etc.).
#define WINVER        _WIN32_WINNT_WIN10
#define _WIN32_WINNT  WINVER
#define NTDDI_VERSION NTDDI_WIN10_FE

#include <windows.h>
#include <windowsx.h>   // HANDLE_MSG message crackers
#include <shellapi.h>   // Shell_NotifyIcon / NOTIFYICONDATA / SHAppBarMessage
#include <shlwapi.h>    // wnsprintf (the buffer-size-checked wsprintf; see WinAPI::String::Format)
#include <uxtheme.h>    // themed (native) button drawing + buffered paint
#include <vssym32.h>    // TASKBARPARTS (TBP_*) + TMT_* theme property ids
#include <objbase.h>    // COM (CoInitializeEx / CoCreateInstance)
#include <uiautomation.h>  // IUIAutomation (taskbar button rects)
#include <commctrl.h>   // SysLink control (NMLINK) + InitCommonControlsEx
#include <dlgs.h>       // ChooseFont common-dialog template control ids (stc5, cmb1, ...)

// Custom application message posted by the shell to the message window and dispatched
// there. Defined as a macro (not a namespaced constant) so the windowsx.h HANDLE_MSG
// cracker can token-paste HANDLE_##message and use it as a switch's case label.
#define WM_NOTIFYICON     (WM_APP + 1)   // notification area icon callback

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

// ASSERT(expr): in Debug, break into the debugger when 'expr' is false or zero. In Release it
// expands to nothing at all, so neither the check nor the expression reaches the shipping
// binary. Use it for a pure test on a value already in hand; use VERIFY when the tested
// expression is itself the call that has to run.
#ifdef _DEBUG
    #define ASSERT(expr) ((expr) ? (void)0 : (OutputDebugStringA("ASSERT failed: " #expr "\n"), __debugbreak()))
#else
    #define ASSERT(expr) ((void)0)
#endif

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

// Wrappers that collapse a Win32 call sequence this app repeats verbatim into a single
// operation: the SetFocus dance around disabling a dialog button, the handle-nulling cleanup
// of an owned GDI object, the boilerplate around a *Ex struct, and so on.
//
// Each wrapper sits in a nested namespace named after what it acts on -- the process, a GDI
// object, a window, a trackbar -- and is itself named after the operation, so a call site
// reads as WinAPI::Process::Terminate() or WinAPI::TrackBar::SetPos(). Wrappers that stand
// for a message rather than a function have no API name to borrow, and this way they need
// none: WinAPI::Window::SetIcon is WM_SETICON, WinAPI::TrackBar::SetPos is TBM_SETPOS.
//
// The underlying Win32 call is always written ::qualified, so a wrapper body still shows at a
// glance which raw API it stands for.
namespace WinAPI {

namespace Url {

// Open(owner, url): open 'url' with the shell's default handler (a web browser for
// http/https links), owned by 'owner'. Wrapped in VERIFY_SHELLEXEC so a failure (a
// ShellExecute result <= 32) breaks into the debugger in Debug and is a no-op check in
// Release.
inline void Open(HWND owner, LPCTSTR url) {
    VERIFY_SHELLEXEC(ShellExecute(owner, TEXT("open"), url, nullptr, nullptr, SW_SHOWNORMAL));
}

// OpenOnContextHelp(dlg, cmd, x, y, url): shared WM_SYSCOMMAND body for the DS_CONTEXTHELP
// dialogs. The title-bar "?" (context-help) button opens 'url' -- the dialog's README section
// -- instead of entering Windows' per-control help mode; every other system command (move,
// close, ...) is forwarded to the default handler unchanged. Each dialog's cracker-dispatched
// OnSysCommand forwards here with the URL it documents (see HANDLE_WM_SYSCOMMAND callers).
inline void OpenOnContextHelp(HWND dlg, UINT cmd, int x, int y, LPCTSTR url) {
    if ((cmd & 0xFFF0) == SC_CONTEXTHELP)
        Open(dlg, url);
    else
        FORWARD_WM_SYSCOMMAND(dlg, cmd, x, y, DefWindowProc);
}

} // namespace Url

namespace Dialog {

// Foreground(dlg[, top]): bring an already-open modal dialog back to the foreground on
// a repeat "open" request, so a second instance is never created. Prefers 'top' when given
// -- e.g. a common dialog (ChooseFont) running over 'dlg', whose owner is disabled while the
// common dialog is modal -- otherwise 'dlg's last active popup, which resurfaces a child
// dialog opened from 'dlg' (GetLastActivePopup returns 'dlg' itself when it owns no popups).
inline void Foreground(HWND dlg, HWND top = nullptr) {
    SetForegroundWindow(top ? top : GetLastActivePopup(dlg));
}

} // namespace Dialog

namespace String {

// Load(inst, id, buffer): load the string resource 'id' from 'inst' into the fixed-size
// stack 'buffer', with its capacity deduced as the template size parameter N. buffer[0] is
// cleared first so a missing or empty resource leaves a valid, null-terminated empty string
// rather than uninitialized stack memory -- callers can use the result unconditionally.
// Returns 'buffer' so it can be passed straight to an API (e.g. MessageBox, SetWindowText).
template <int N>
_Ret_z_ LPCTSTR Load(HINSTANCE inst, UINT id, _Out_writes_z_(N) TCHAR (&buffer)[N]) {
    buffer[0] = TEXT('\0');
    VERIFY(LoadString(inst, id, buffer, N));
    return buffer;
}

// Copy(buffer, source): copy 'source' into the fixed-size 'buffer', with its capacity deduced
// as the template size parameter N the same way Load deduces it -- so no call site repeats a
// buffer size, and none can pass a stale one. The bounded lstrcpyn does the work, so a longer
// 'source' is truncated instead of overrunning the buffer and the result is null-terminated.
// Returns 'buffer' so it can be passed straight to an API.
template <int N>
_Ret_z_ LPCTSTR Copy(_Out_writes_z_(N) TCHAR (&buffer)[N], _In_z_ LPCTSTR source) {
    buffer[0] = TEXT('\0');
    VERIFY(lstrcpyn(buffer, source, N));
    return buffer;
}

// Append(buffer, source): append 'source' to the string already in 'buffer' -- the bounded
// lstrcat Win32 does not provide -- limited to the room left in the deduced capacity N, so
// what does not fit is truncated rather than written past the end. 'buffer' must already hold
// a null-terminated string (an empty one makes this a plain Copy). The N - used limit covers
// the terminator: lstrcpyn counts it, and N - used is exactly the number of cells left from
// the old terminator's slot through buffer[N - 1], so a truncated append ends in that last
// cell. Returns 'buffer'.
template <int N>
_Ret_z_ LPCTSTR Append(_Inout_updates_z_(N) TCHAR (&buffer)[N], _In_z_ LPCTSTR source) {
    const int used = lstrlen(buffer);
    VERIFY(lstrcpyn(buffer + used, source, N - used));
    return buffer;
}

// Format(buffer, format, args): substitute 'args' into the printf-style 'format' -- the
// wsprintf placeholder set (%s, %d, %lu, ...) -- writing into the fixed-size 'buffer', with
// its capacity deduced as the template size parameter N exactly as in Load and Copy. The
// bounded wnsprintf does the work, so oversized output is truncated instead of overrunning
// the buffer. buffer[0] is cleared first and the final cell forced to null afterwards --
// wnsprintf does not promise to terminate a truncated result -- so the buffer is a valid
// string on every path, including a format that produced nothing at all. A negative return
// means wnsprintf rejected the call outright, which is a programming error (a malformed
// template, or arguments that do not match it), so Debug breaks on it; Release falls back to
// the raw 'format', since the unsubstituted template is both a valid string and a far more
// diagnosable thing to leave on screen than the partial text a failed call wrote. Returns
// 'buffer' so it can be passed straight to an API (e.g. SetDlgItemText).
template <int N, typename... Args>
_Ret_z_ LPCTSTR Format(_Out_writes_z_(N) TCHAR (&buffer)[N], _In_z_ LPCTSTR format,
                       Args... args) {
    buffer[0] = TEXT('\0');
    const int written = wnsprintf(buffer, N, format, args...);
    buffer[N - 1] = TEXT('\0');

    ASSERT(written > 0);
    if (written <= 0) Copy(buffer, format);

    return buffer;
}

} // namespace String

namespace OS {

// IsWindows10(): true on Windows 10 (build < 22000, the first public Windows 11
// build); false on Windows 11 and later. WinNumTip.manifest opts in to the Windows
// 10/11 compatibility GUID, so VerifyVersionInfo reports the real build number here
// instead of lying about it the way it would for an unmanifested caller.
//
// The result cannot change for the life of the process, so it is computed once and
// cached in a local static -- but as a plain constant-initialized int, not a
// function-local static with a runtime initializer: that form's thread-safe init
// guard needs CRT support (_Init_thread_header and friends) this /NODEFAULTLIB,
// no-CRT build does not link against.
[[nodiscard]] inline bool IsWindows10() {
    static int cached = -1;   // -1 = not yet checked, 0 = false, 1 = true
    if (cached < 0) {
        OSVERSIONINFOEXW vi;
        ZeroMemory(&vi, sizeof(vi));
        vi.dwOSVersionInfoSize = sizeof(vi);
        vi.dwMajorVersion = 10;
        vi.dwMinorVersion = 0;
        vi.dwBuildNumber  = 22000;

        DWORDLONG mask = 0;
        VER_SET_CONDITION(mask, VER_MAJORVERSION, VER_EQUAL);
        VER_SET_CONDITION(mask, VER_MINORVERSION, VER_EQUAL);
        VER_SET_CONDITION(mask, VER_BUILDNUMBER,  VER_LESS);

        cached = VerifyVersionInfoW(&vi, VER_MAJORVERSION | VER_MINORVERSION | VER_BUILDNUMBER, mask) != FALSE
                     ? 1 : 0;
    }
    return cached != 0;
}

} // namespace OS

namespace Keyboard {

// IsWinDown(): true while either physical Windows key is currently held down. Used to gate the
// overlay: the hook trusts it to distinguish a genuine Win release from the synthetic injected
// up sent during Win+<key> chords, and the refresh timer polls it to self-heal when a Win
// key-up is never delivered.
[[nodiscard]] inline bool IsWinDown() {
    // GetAsyncKeyState sets this high-order bit in its result while the key is physically down.
    constexpr int kKeyDownBit = 0x8000;

    return GetAsyncKeyState(VK_LWIN) & kKeyDownBit || GetAsyncKeyState(VK_RWIN) & kKeyDownBit;
}

} // namespace Keyboard

namespace Process {

// Terminate(): leave the current process immediately, without running the loader's DLL detach
// callbacks. The system font dialog can leave font-cache worker threads inside system DLLs,
// and a normal ExitProcess may deadlock while running their detach routines -- so both the
// disposable font-picker helper process and the entry point's helper branch end this way.
// ExitProcess is unreachable and kept only as the compiler-visible [[noreturn]] tail in case
// TerminateProcess ever returns.
[[noreturn]] inline void Terminate() {
    TerminateProcess(GetCurrentProcess(), 0);
    ExitProcess(0);
}

} // namespace Process

namespace CommonControls {

// Init(classes): register the given ICC_* common-control classes (e.g. ICC_LINK_CLASS for
// SysLink, ICC_BAR_CLASSES for the trackbar) so dialogs that use them can be created
// directly. Returns FALSE when the registration fails.
[[nodiscard]] inline BOOL Init(DWORD classes) {
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC  = classes;
    return InitCommonControlsEx(&icc);
}

} // namespace CommonControls

namespace GdiObject {

// Delete(obj): destroy the GDI object 'obj' refers to (font, brush, ...) and null the handle,
// so a repeat call -- or a later cleanup pass -- is a no-op. Wrapped in VERIFY since a failing
// DeleteObject means the handle was invalid or still selected into a DC, which is a
// programming error rather than an expected runtime condition.
template <typename T>
void Delete(T& obj) {
    if (obj) { VERIFY(DeleteObject(obj)); obj = nullptr; }
}

} // namespace GdiObject

namespace Icon {

// Load(inst, id, cxMetric, cyMetric, flags): load icon resource 'id' from 'inst' at the icon
// size named by the SM_CX*/SM_CY* system metrics (SM_CXICON/SM_CYICON for a large icon,
// SM_CXSMICON/SM_CYSMICON for a small one), so it matches the shell's current scaling.
// 'flags' picks the ownership: LR_SHARED for a system-managed icon that needs no cleanup,
// LR_DEFAULTCOLOR for a caller-owned one that must be released with WinAPI::Icon::Destroy.
[[nodiscard]] inline HICON Load(HINSTANCE inst, UINT id, int cxMetric, int cyMetric, UINT flags) {
    return static_cast<HICON>(LoadImage(inst, MAKEINTRESOURCE(id), IMAGE_ICON,
                                          GetSystemMetrics(cxMetric), GetSystemMetrics(cyMetric),
                                          flags));
}

// Destroy(icon): the icon counterpart of WinAPI::GdiObject::Delete -- release an icon the
// caller owns (loaded without LR_SHARED) and null the handle so it cannot be freed twice.
inline void Destroy(HICON& icon) {
    if (icon) { VERIFY(DestroyIcon(icon)); icon = nullptr; }
}

} // namespace Icon

namespace Font {

// GetLogFont(font, lf): describe the GDI font handle 'font' in 'lf'. False when 'font' is null
// or the query fails, in which case 'lf' is left untouched and must not be read. Combined with
// windowsx.h's GetWindowFont it also yields a window's own font, e.g. a dialog's already
// DPI-scaled one used as the baseline that derived fonts keep the face and size of.
[[nodiscard]] inline bool GetLogFont(HFONT font, LOGFONT& lf) {
    return font != nullptr && GetObject(font, sizeof(lf), &lf) != 0;
}

} // namespace Font

namespace Window {

// GetDpi(wnd): the DPI 'wnd' is displayed at, falling back to the 96-dpi baseline when the
// window handle is no longer valid (the API then returns 0), so the result can be passed
// straight to the *ForDpi APIs.
[[nodiscard]] inline UINT GetDpi(HWND wnd) {
    const UINT dpi = GetDpiForWindow(wnd);
    return dpi ? dpi : USER_DEFAULT_SCREEN_DPI;
}

// AllowSetForeground(wnd): let the process owning 'wnd' take the foreground. Only the current
// foreground process can grant this, so it is called from whichever side is active while
// activation is handed over to (or back from) the font-picker helper process. A no-op when
// 'wnd' no longer belongs to a live process.
inline void AllowSetForeground(HWND wnd) {
    DWORD process = 0;
    GetWindowThreadProcessId(wnd, &process);
    if (process) AllowSetForegroundWindow(process);
}

// Enable(button, enabled): enable or disable a dialog push button (e.g. Apply), first moving
// the keyboard focus off it -- to the dialog's default OK button -- when it is about to be
// disabled, so the dialog is never left with the focus on a dead control.
inline void Enable(HWND button, bool enabled) {
    if (!enabled && GetFocus() == button) SetFocus(GetDlgItem(GetParent(button), IDOK));
    EnableWindow(button, enabled ? TRUE : FALSE);
}

// SetIcon(wnd, type, icon): give 'wnd' its ICON_BIG (Alt+Tab and the task switcher) or
// ICON_SMALL (title bar and the taskbar) icon, and do nothing when the icon failed to load, so
// the window keeps the class default rather than being stripped of one.
inline void SetIcon(HWND wnd, UINT type, HICON icon) {
    if (icon) SendMessage(wnd, WM_SETICON, type, reinterpret_cast<LPARAM>(icon));
}

} // namespace Window

// The receiver of WM_CHOOSEFONT_GETLOGFONT. ChooseFont is an object-like macro, so this
// namespace is really named ChooseFontW -- harmless, since every use of the name expands the
// same way, but it is what a compiler diagnostic will call it.
namespace ChooseFont {

// GetLogFont(dialog, lf): read the ChooseFont common dialog's live selection into 'lf'. With
// a custom template comdlg32 does not refresh its own preview, so the selection has to be
// queried this way on every change. 'lf' is cleared first, so every field is defined even if
// the dialog does not fill the whole structure.
inline void GetLogFont(HWND dialog, LOGFONT& lf) {
    ZeroMemory(&lf, sizeof(lf));
    SendMessage(dialog, WM_CHOOSEFONT_GETLOGFONT, 0, reinterpret_cast<LPARAM>(&lf));
}

} // namespace ChooseFont

namespace Button {

// Click(button): press 'button' as if the user had clicked it. BM_CLICK makes the control run
// its own click processing and notify its parent with BN_CLICKED, so a programmatic press
// takes exactly the same path -- and leaves exactly the same state behind -- as a real one,
// rather than duplicating whatever the click handler does.
inline void Click(HWND button) {
    SendMessage(button, BM_CLICK, 0, 0);
}

} // namespace Button

namespace TrackBar {

// Init(trackBar, minPos, maxPos, tickFreq, pageStep, pos): one-shot setup of a trackbar
// (slider) -- its inclusive range, tick-mark spacing, PageUp/PageDown step and initial thumb
// position. Every Preferences slider is configured exactly this way.
inline void Init(HWND trackBar, int minPos, int maxPos, int tickFreq, int pageStep, int pos) {
    SendMessage(trackBar, TBM_SETRANGE,    TRUE, MAKELPARAM(minPos, maxPos));
    SendMessage(trackBar, TBM_SETTICFREQ,  tickFreq, 0);
    SendMessage(trackBar, TBM_SETPAGESIZE, 0, pageStep);
    SendMessage(trackBar, TBM_SETPOS,      TRUE, pos);
}

// GetPos(dlg, id) / SetPos(dlg, id, pos): read / move the thumb of the trackbar control 'id'
// in 'dlg'.
[[nodiscard]] inline int GetPos(HWND dlg, int id) {
    return static_cast<int>(SendDlgItemMessage(dlg, id, TBM_GETPOS, 0, 0));
}

inline void SetPos(HWND dlg, int id, int pos) {
    SendDlgItemMessage(dlg, id, TBM_SETPOS, TRUE, pos);
}

} // namespace TrackBar

} // namespace WinAPI
