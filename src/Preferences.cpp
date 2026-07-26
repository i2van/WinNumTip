#include "stdafx.h"
#include "Preferences.h"

namespace {

// INI section/key for the persisted preferences. The file lives next to the executable.
constexpr LPCTSTR kSection      = TEXT("Preferences");
constexpr LPCTSTR kKeyLabelSize = TEXT("LabelSizePercent");
constexpr LPCTSTR kKeyInvert    = TEXT("InvertColors");
constexpr LPCTSTR kKeyBold      = TEXT("BoldFont");
constexpr LPCTSTR kKeyRefreshMs = TEXT("RefreshIntervalMs");
constexpr LPCTSTR kKeyPollMs    = TEXT("PollIntervalMs");
constexpr LPCTSTR kIniName      = TEXT("WinNumTip.ini");

// Full path of the INI file next to the executable, built once by Load().
TCHAR g_iniPath[MAX_PATH] = { 0 };

// Cached label-size percentage in [0, 100]; mirrors the INI value.
int g_labelPercent = 0;

// Cached invert-colors flag; mirrors the INI value.
bool g_invert = false;

// Cached bold-font flag; mirrors the INI value.
bool g_bold = false;

// Cached refresh-timer interval in ms; mirrors the INI value.
int g_refreshMs = Preferences::kDefaultRefreshMs;

// Cached poll-timer interval in ms; mirrors the INI value.
int g_pollMs = Preferences::kDefaultPollMs;

int ClampPercent(int v) {
    if (v < Preferences::kMinPercent) return Preferences::kMinPercent;
    if (v > Preferences::kMaxPercent) return Preferences::kMaxPercent;
    return v;
}

// Clamp 'v' to the inclusive [lo, hi] range.
int Clamp(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
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

// Persist a boolean preference as "1"/"0" under the given key in the INI file.
void WriteBool(LPCTSTR key, bool value) {
    WritePrivateProfileString(kSection, key, value ? TEXT("1") : TEXT("0"), g_iniPath);
}

// Persist an integer preference under the given key in the INI file.
void WriteInt(LPCTSTR key, int value) {
    TCHAR buf[16];
    wsprintf(buf, TEXT("%d"), value);
    WritePrivateProfileString(kSection, key, buf, g_iniPath);
}

// Read an integer preference under the given key from the INI file, or 'def' if absent.
int ReadInt(LPCTSTR key, int def) {
    return static_cast<int>(GetPrivateProfileInt(kSection, key, def, g_iniPath));
}

// Read a boolean preference (stored as a nonzero/zero integer) under the given key from
// the INI file, or 'def' if absent.
bool ReadBool(LPCTSTR key, bool def) {
    return ReadInt(key, def ? 1 : 0) != 0;
}

} // namespace

namespace Preferences {

void Load() {
    BuildIniPath();
    g_labelPercent = ClampPercent(ReadInt(kKeyLabelSize, 0));
    g_invert       = ReadBool(kKeyInvert, false);
    g_bold         = ReadBool(kKeyBold, false);
    g_refreshMs    = Clamp(ReadInt(kKeyRefreshMs, kDefaultRefreshMs), kMinRefreshMs, kMaxRefreshMs);
    g_pollMs       = Clamp(ReadInt(kKeyPollMs, kDefaultPollMs), kMinPollMs, kMaxPollMs);
}

int LabelSizePercent() {
    return g_labelPercent;
}

void SetLabelSizePercent(int percent) {
    g_labelPercent = ClampPercent(percent);
    WriteInt(kKeyLabelSize, g_labelPercent);
}

bool InvertColors() {
    return g_invert;
}

void SetInvertColors(bool invert) {
    g_invert = invert;
    WriteBool(kKeyInvert, invert);
}

bool BoldFont() {
    return g_bold;
}

void SetBoldFont(bool bold) {
    g_bold = bold;
    WriteBool(kKeyBold, bold);
}

int RefreshIntervalMs() {
    return g_refreshMs;
}

void SetRefreshIntervalMs(int ms) {
    g_refreshMs = Clamp(ms, kMinRefreshMs, kMaxRefreshMs);
    WriteInt(kKeyRefreshMs, g_refreshMs);
}

int PollIntervalMs() {
    return g_pollMs;
}

void SetPollIntervalMs(int ms) {
    g_pollMs = Clamp(ms, kMinPollMs, kMaxPollMs);
    WriteInt(kKeyPollMs, g_pollMs);
}

} // namespace Preferences
