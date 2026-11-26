#include "stdafx.h"
#include "Startup.h"
#include "Registry.h"

namespace {

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
    ASSERT(n != 0);
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
        Registry::SetStartupCommand(nullptr);
        return;
    }

    TCHAR cmd[kCmdChars];
    if (!BuildCommand(cmd)) return;

    Registry::SetStartupCommand(cmd);
}

} // namespace Startup
