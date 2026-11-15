#include "stdafx.h"
#include "Registry.h"

namespace {

// A zero SystemUsesLightTheme DWORD means the taskbar/system UI is dark.
constexpr LPCTSTR kPersonalizeKey   = TEXT(R"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)");
constexpr LPCTSTR kSystemLightTheme = TEXT("SystemUsesLightTheme");
constexpr LPCTSTR kRunKey           = TEXT(R"(Software\Microsoft\Windows\CurrentVersion\Run)");
constexpr LPCTSTR kRunValue         = TEXT("WinNumTip");

} // namespace

namespace Registry {

bool SystemUsesDarkTheme() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    return RegGetValue(HKEY_CURRENT_USER, kPersonalizeKey, kSystemLightTheme, RRF_RT_REG_DWORD,
                       nullptr, &value, &size) == ERROR_SUCCESS && value == 0;
}

void SetStartupCommand(LPCTSTR command) {
    if (!command) {
        // A missing entry is expected, so no VERIFY here.
        (void)RegDeleteKeyValue(HKEY_CURRENT_USER, kRunKey, kRunValue);
        return;
    }

    // The executable's MAX_PATH buffer plus two quotes and its existing null terminator.
    TCHAR existing[MAX_PATH + 2];
    DWORD size = sizeof(existing);
    if (RegGetValue(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr,
                    existing, &size) == ERROR_SUCCESS &&
        lstrcmpi(existing, command) == 0) {
        return;
    }

    // RegSetKeyValue creates the key path as needed.
    VERIFY(RegSetKeyValue(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, command,
                          static_cast<DWORD>((lstrlen(command) + 1) * sizeof(TCHAR))) == ERROR_SUCCESS);
}

} // namespace Registry
