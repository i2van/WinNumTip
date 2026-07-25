#include "stdafx.h"
#include "Preferences.h"

namespace {

// INI section/key for the persisted preferences. The file lives next to the executable.
constexpr LPCTSTR kSection      = TEXT("Preferences");
constexpr LPCTSTR kKeyLabelSize = TEXT("LabelSizePercent");
constexpr LPCTSTR kKeyInvert    = TEXT("InvertColors");
constexpr LPCTSTR kIniName      = TEXT("WinNumTip.ini");

// Full path of the INI file next to the executable, built once by Load().
TCHAR g_iniPath[MAX_PATH] = { 0 };

// Cached label-size percentage in [0, 100]; mirrors the INI value.
int g_labelPercent = 0;

// Cached invert-colors flag; mirrors the INI value.
bool g_invert = false;

int ClampPercent(int v) {
    if (v < Preferences::kMinPercent) return Preferences::kMinPercent;
    if (v > Preferences::kMaxPercent) return Preferences::kMaxPercent;
    return v;
}

// Resolve the INI path to "<exe dir>\WinNumTip.ini". Falls back to a bare file name
// (current directory) if the module path has no directory component.
void BuildIniPath() {
    g_iniPath[0] = 0;
    const DWORD n = GetModuleFileName(nullptr, g_iniPath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { lstrcpyn(g_iniPath, kIniName, ARRAYSIZE(g_iniPath)); return; }

    int slash = -1;
    for (int i = 0; g_iniPath[i]; ++i)
        if (g_iniPath[i] == TEXT('\\')) slash = i;
    // Truncate after the last backslash (or to empty if none), then append the file name
    // with a bounded copy into whatever room is left in the buffer.
    g_iniPath[slash + 1] = 0;
    lstrcpyn(g_iniPath + slash + 1, kIniName, static_cast<int>(ARRAYSIZE(g_iniPath)) - (slash + 1));
}

} // namespace

namespace Preferences {

void Load() {
    BuildIniPath();
    g_labelPercent = ClampPercent(GetPrivateProfileInt(kSection, kKeyLabelSize, 0, g_iniPath));
    g_invert       = GetPrivateProfileInt(kSection, kKeyInvert, 0, g_iniPath) != 0;
}

int LabelSizePercent() {
    return g_labelPercent;
}

void SetLabelSizePercent(int percent) {
    g_labelPercent = ClampPercent(percent);
    TCHAR buf[16];
    wsprintf(buf, TEXT("%d"), g_labelPercent);
    WritePrivateProfileString(kSection, kKeyLabelSize, buf, g_iniPath);
}

bool InvertColors() {
    return g_invert;
}

void SetInvertColors(bool invert) {
    g_invert = invert;
    WritePrivateProfileString(kSection, kKeyInvert, invert ? TEXT("1") : TEXT("0"), g_iniPath);
}

} // namespace Preferences
