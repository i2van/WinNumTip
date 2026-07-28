#pragma once

// Persistent user preferences, stored in an INI file next to the executable (loaded and
// saved with the Get*/WritePrivateProfile* WinAPI). This is the model only; the editing
// UI lives in PreferencesDialog.
//
// Five preferences today:
//  - the overlay "label size" (see below),
//  - "invert colors": whether the strip and number colors are swapped for a highlighted
//    look,
//  - "font": the face + style (weight/italic/underline/strikeout) the numbers are drawn
//    with, chosen from the font dialog; unset means the taskbar's own font (the fallback),
//  - "refresh interval": how often (ms) the shown overlay re-checks the taskbar buttons
//    and rebuilds the bar in place, and
//  - "poll interval": how often (ms) the app reconciles the overlay's visibility from the
//    keyboard hook.
// The label-size preference controls how thick the numbered strip is
// perpendicular to the taskbar's long axis (the label height for a horizontal taskbar,
// the label width for a side-docked one). It is an integer percentage in [0, 100] of a
// full taskbar-button thickness:
//   <= the default's share -> the built-in slim default thickness (unchanged behavior),
//   100                    -> a full taskbar-button-sized cell,
// scaled linearly in between (and the number text scales with it). The strip is never
// thinner than the default, so the Preferences dialog only offers percentages from the
// default's share up to 100. A fresh INI (0) therefore renders as the default. The
// default's share is computed from the live taskbar via Overlay::LabelSizeBounds; see
// Overlay::Refresh.
namespace Preferences {

// The label-size percentage (see the file header for its meaning) is an integer in
// [kMinPercent, kMaxPercent] of a full taskbar-button thickness.
constexpr int kMinPercent = 0;
constexpr int kMaxPercent = 100;

// The overlay refresh-timer interval, in milliseconds: how often the shown overlay
// re-checks the taskbar buttons and rebuilds the bar in place. Lower is more responsive to
// taskbar changes but polls more often.
constexpr int kMinRefreshMs     = 50;
constexpr int kMaxRefreshMs     = 1000;
constexpr int kDefaultRefreshMs = 200;

// The keyboard poll-timer interval, in milliseconds: how often the app reconciles the
// overlay's visibility from the keyboard hook's flag + live key state. Lower shows/hides
// the overlay with less latency but polls more often.
constexpr int kMinPollMs     = 25;
constexpr int kMaxPollMs     = 500;
constexpr int kDefaultPollMs = 75;

// Load preferences from the INI file next to the executable (Get*PrivateProfile*). Call
// once at startup, before the overlay is first shown.
void Load();

// Current label-size percentage in [0, 100] (see the file header for its meaning).
[[nodiscard]] int LabelSizePercent();

// Store the label-size percentage (clamped to [0, 100]) both in memory and in the INI
// file next to the executable (WritePrivateProfileString).
void SetLabelSizePercent(int percent);

// Whether the overlay draws with inverted colors: the strip is filled with the number
// color and the numbers are drawn in the bar color (a highlighted look).
[[nodiscard]] bool InvertColors();

// Store the invert-colors flag both in memory and in the INI file next to the executable.
void SetInvertColors(bool invert);

// Whether the overlay draws the numbers with a user-selected font (FontIsSet) and, if so,
// the LOGFONT describing it. Only the face and style fields (lfFaceName, lfWeight,
// lfItalic, lfUnderline, lfStrikeOut) are meaningful: the overlay drives the glyph height
// from the taskbar font scaled by the label size, so the stored height is ignored there.
// When no font is set the overlay uses the taskbar's own font (the fallback).
[[nodiscard]] bool FontIsSet();
[[nodiscard]] const LOGFONT& Font();

// Store 'lf' as the selected font (an empty lfFaceName clears the selection, same as
// ClearFont) both in memory and in the INI file next to the executable.
void SetFont(const LOGFONT& lf);

// Clear the font selection (revert to the taskbar-font fallback) in memory and the INI.
void ClearFont();

// The overlay refresh-timer interval in milliseconds, in [kMinRefreshMs, kMaxRefreshMs]
// (see the file header). A changed value takes effect when Preferences is applied.
[[nodiscard]] int RefreshIntervalMs();

// Store the refresh-timer interval (clamped to [kMinRefreshMs, kMaxRefreshMs]) both in
// memory and in the INI file next to the executable.
void SetRefreshIntervalMs(int ms);

// The keyboard poll-timer interval in milliseconds, in [kMinPollMs, kMaxPollMs] (see the
// file header). A changed value takes effect when the Preferences dialog is applied.
[[nodiscard]] int PollIntervalMs();

// Store the poll-timer interval (clamped to [kMinPollMs, kMaxPollMs]) both in memory and
// in the INI file next to the executable.
void SetPollIntervalMs(int ms);

} // namespace Preferences
