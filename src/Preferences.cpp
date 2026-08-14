#include "stdafx.h"
#include "Preferences.h"

namespace {

// INI section/key for the persisted preferences. The file lives next to the executable.
constexpr LPCTSTR kSection          = TEXT("Preferences");
constexpr LPCTSTR kKeyTipSize       = TEXT("TipSizePercent");
constexpr LPCTSTR kLegacyKeyTipSize = TEXT("LabelSizePercent");
constexpr LPCTSTR kKeyOpacity       = TEXT("OpacityPercent");
constexpr LPCTSTR kKeyInvert        = TEXT("InvertColors");
constexpr LPCTSTR kKeyHideBorder    = TEXT("HideBorder");
constexpr LPCTSTR kKeyHideSeparator = TEXT("HideSeparator");
constexpr LPCTSTR kKeyCompactView   = TEXT("CompactView");
constexpr LPCTSTR kKeyFontFace      = TEXT("FontFace");
constexpr LPCTSTR kKeyFontWeight    = TEXT("FontWeight");
constexpr LPCTSTR kKeyFontItalic    = TEXT("FontItalic");
constexpr LPCTSTR kKeyFontUnderline = TEXT("FontUnderline");
constexpr LPCTSTR kKeyFontStrikeOut = TEXT("FontStrikeOut");
constexpr LPCTSTR kKeyRefreshMs = TEXT("RefreshIntervalMs");
constexpr LPCTSTR kKeyPollMs    = TEXT("PollIntervalMs");
constexpr LPCTSTR kIniName      = TEXT("WinNumTip.ini");

// Full path of the INI file next to the executable, built once by Load().
TCHAR g_iniPath[MAX_PATH];

// Cached tip-size percentage in [0, 100]; mirrors the INI value.
int g_tipPercent;

// Cached opacity percentage; mirrors the INI value.
int g_opacityPercent = Preferences::kDefaultOpacityPercent;

// Cached rendering toggles; mirrors the INI values.
Preferences::RenderFlags g_flags;

// Cached selected font (only lfFaceName + the weight/italic/underline/strikeout style
// fields are meaningful; height/charset are the overlay's/dialog's concern). Valid only
// when g_fontSet is true; otherwise the overlay falls back to the taskbar font.
LOGFONT g_font;
bool g_fontSet;

// Cached refresh-timer interval in ms; mirrors the INI value.
int g_refreshMs = Preferences::kDefaultRefreshMs;

// Cached poll-timer interval in ms; mirrors the INI value.
int g_pollMs = Preferences::kDefaultPollMs;

// Clamp 'v' to the inclusive [lo, hi] range.
[[nodiscard]] int Clamp(int v, int lo, int hi) {
    return max(lo, min(v, hi));
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
[[nodiscard]] int ReadInt(LPCTSTR key, int def) {
    return static_cast<int>(GetPrivateProfileInt(kSection, key, def, g_iniPath));
}

// Read a boolean preference (stored as a nonzero/zero integer) under the given key from
// the INI file, or 'def' if absent.
[[nodiscard]] bool ReadBool(LPCTSTR key, bool def) {
    return ReadInt(key, def ? 1 : 0) != 0;
}

// Read an integer preference under the given key from the INI file (or 'def' if absent),
// clamped to [lo, hi] (defaulting to the tip-size percentage bounds so the common case
// needs no explicit bounds).
[[nodiscard]] int ReadPercent(LPCTSTR key, int def, int lo = Preferences::kMinPercent, int hi = Preferences::kMaxPercent) {
    return Clamp(ReadInt(key, def), lo, hi);
}

// Persist a string preference under the given key in the INI file. A null 'value' removes
// the key from the section.
void WriteString(LPCTSTR key, LPCTSTR value) {
    WritePrivateProfileString(kSection, key, value, g_iniPath);
}

// Read a string preference under the given key into 'buf' (capacity 'cch' chars),
// substituting 'def' when the key is absent. The buffer is always null-terminated.
void ReadString(LPCTSTR key, LPCTSTR def, LPTSTR buf, int cch) {
    GetPrivateProfileString(kSection, key, def, buf, static_cast<DWORD>(cch), g_iniPath);
}

} // namespace

namespace Preferences {

void Load() {
    BuildIniPath();
    const int legacyTipPercent = ReadPercent(kLegacyKeyTipSize, 0);
    g_tipPercent = ReadPercent(kKeyTipSize, legacyTipPercent);
    g_opacityPercent = ReadPercent(kKeyOpacity, kDefaultOpacityPercent, kMinOpacityPercent, kMaxOpacityPercent);
    g_flags.invertColors = ReadBool(kKeyInvert, false);
    g_flags.hideBorder    = ReadBool(kKeyHideBorder, false);
    g_flags.hideSeparator = ReadBool(kKeyHideSeparator, false);
    g_flags.compact       = ReadBool(kKeyCompactView, false);
    g_refreshMs    = ReadPercent(kKeyRefreshMs, kDefaultRefreshMs, kMinRefreshMs, kMaxRefreshMs);
    g_pollMs       = ReadPercent(kKeyPollMs, kDefaultPollMs, kMinPollMs, kMaxPollMs);

    ZeroMemory(&g_font, sizeof(g_font));
    TCHAR face[LF_FACESIZE];
    ReadString(kKeyFontFace, TEXT(""), face, ARRAYSIZE(face));
    g_fontSet = (face[0] != 0);
    if (g_fontSet) {
        lstrcpyn(g_font.lfFaceName, face, LF_FACESIZE);
        g_font.lfWeight    = ReadInt(kKeyFontWeight, FW_NORMAL);
        g_font.lfItalic    = static_cast<BYTE>(ReadBool(kKeyFontItalic, false) ? 1 : 0);
        g_font.lfUnderline = static_cast<BYTE>(ReadBool(kKeyFontUnderline, false) ? 1 : 0);
        g_font.lfStrikeOut = static_cast<BYTE>(ReadBool(kKeyFontStrikeOut, false) ? 1 : 0);
        g_font.lfCharSet   = DEFAULT_CHARSET;
    }
}

int TipSizePercent() {
    return g_tipPercent;
}

void SetTipSizePercent(int percent) {
    g_tipPercent = Clamp(percent, kMinPercent, kMaxPercent);
    WriteInt(kKeyTipSize, g_tipPercent);
    WriteString(kLegacyKeyTipSize, nullptr);
}

int OpacityPercent() {
    return g_opacityPercent;
}

void SetOpacityPercent(int percent) {
    g_opacityPercent = Clamp(percent, kMinOpacityPercent, kMaxOpacityPercent);
    WriteInt(kKeyOpacity, g_opacityPercent);
}

const RenderFlags& Flags() {
    return g_flags;
}

void SetFlags(const RenderFlags& flags) {
    g_flags = flags;
    WriteBool(kKeyInvert, flags.invertColors);
    WriteBool(kKeyHideBorder, flags.hideBorder);
    WriteBool(kKeyHideSeparator, flags.hideSeparator);
    WriteBool(kKeyCompactView, flags.compact);
}

bool FontIsSet() {
    return g_fontSet;
}

const LOGFONT& Font() {
    return g_font;
}

void SetFont(const LOGFONT& lf) {
    if (lf.lfFaceName[0] == 0) { ClearFont(); return; }

    ZeroMemory(&g_font, sizeof(g_font));
    lstrcpyn(g_font.lfFaceName, lf.lfFaceName, LF_FACESIZE);
    g_font.lfWeight    = lf.lfWeight;
    g_font.lfItalic    = lf.lfItalic;
    g_font.lfUnderline = lf.lfUnderline;
    g_font.lfStrikeOut = lf.lfStrikeOut;
    g_font.lfCharSet   = DEFAULT_CHARSET;
    g_fontSet          = true;

    WriteString(kKeyFontFace, g_font.lfFaceName);
    WriteInt(kKeyFontWeight, g_font.lfWeight);
    WriteBool(kKeyFontItalic, g_font.lfItalic != 0);
    WriteBool(kKeyFontUnderline, g_font.lfUnderline != 0);
    WriteBool(kKeyFontStrikeOut, g_font.lfStrikeOut != 0);
}

void ClearFont() {
    ZeroMemory(&g_font, sizeof(g_font));
    g_fontSet = false;
    WriteString(kKeyFontFace, nullptr);
    WriteString(kKeyFontWeight, nullptr);
    WriteString(kKeyFontItalic, nullptr);
    WriteString(kKeyFontUnderline, nullptr);
    WriteString(kKeyFontStrikeOut, nullptr);
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
