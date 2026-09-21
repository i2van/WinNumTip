#include "stdafx.h"
#include "Startup.h"

namespace {

// The registry hive the startup key lives under: the current user's, so no elevation is
// needed to write it. Not constexpr: the SDK macro casts an integer to a pointer, which a
// constant expression cannot hold (C2131); MSVC still folds this to compile-time data, so
// no dynamic initializer is involved (this no-CRT build never runs those).
const HKEY kRunRoot = HKEY_CURRENT_USER;
// The current user's startup list: values under this key are launched at sign-in for this
// user only.
constexpr LPCTSTR kRunKey   = TEXT("Software\\Microsoft\\Windows\\CurrentVersion\\Run");
// The startup value name registered for this app.
constexpr LPCTSTR kRunValue = TEXT("WinNumTip");

// Capacity of the Run-value command buffer: the exe path (at most MAX_PATH - 1 chars,
// GetModuleFileName's limit) + the 2 surrounding quotes + the null terminator.
constexpr int kCmdChars = MAX_PATH + 2;

// Build the command the Run value must hold: the current executable's full path, quoted so
// spaces in directory names survive command-line parsing. Returns false when the module
// path cannot be resolved (or does not fit), in which case 'cmd' is left empty and the
// registry is left untouched.
[[nodiscard]] bool BuildCommand(_Out_writes_z_(kCmdChars) TCHAR (&cmd)[kCmdChars]) {
    cmd[0] = TEXT('\0');

    TCHAR exe[MAX_PATH];
    const DWORD n = GetModuleFileName(nullptr, exe, ARRAYSIZE(exe));
    if (n == 0 || n >= ARRAYSIZE(exe)) return false;

    WinAPI::String::Copy(cmd, TEXT("\""));
    WinAPI::String::Append(cmd, exe);
    WinAPI::String::Append(cmd, TEXT("\""));
    return true;
}

} // namespace

namespace Startup {

void Sync(bool enabled) {
    if (!enabled) {
        // Best effort: removing an entry that was never created (or is already gone) is
        // expected to fail with ERROR_FILE_NOT_FOUND, so no VERIFY here.
        (void)RegDeleteKeyValue(kRunRoot, kRunKey, kRunValue);
        return;
    }

    TCHAR cmd[kCmdChars];
    if (!BuildCommand(cmd)) return;

    // Compare the existing value first so the common case -- entry present and current --
    // does not rewrite the registry on every launch. Any failure to read (missing value,
    // wrong type, too long for the buffer) just means the entry needs rewriting.
    TCHAR existing[ARRAYSIZE(cmd)];
    DWORD size = sizeof(existing);
    if (RegGetValue(kRunRoot, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr,
                    existing, &size) == ERROR_SUCCESS &&
        lstrcmpi(existing, cmd) == 0) {
        return;
    }

    // Create or correct the entry (RegSetKeyValue creates the key path as needed).
    VERIFY(RegSetKeyValue(kRunRoot, kRunKey, kRunValue, REG_SZ, cmd,
                          static_cast<DWORD>((lstrlen(cmd) + 1) * sizeof(TCHAR))) == ERROR_SUCCESS);
}

} // namespace Startup
