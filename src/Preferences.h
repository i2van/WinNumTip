#pragma once

// Persistent user preferences, stored in an INI file next to the executable (loaded and
// saved with the Get*/WritePrivateProfile* WinAPI). This is the model only; the editing
// UI lives in PreferencesDialog.
//
// Six preferences today:
//  - the overlay "tip size" (see below),
//  - "opacity": how opaque the overlay window is, as a percentage (see below),
//  - "render flags": four on/off toggles for the overlay's look, packed as single-bit
//    fields in one RenderFlags struct (see below) -- "invert colors" swaps the strip and
//    number colors for a highlighted look; "hide border" / "hide separator" each strip
//    away one visual layer (the strip's border, the divider lines between number tips)
//    for a progressively more minimal look; and "compact" shrinks the strip's window
//    down to a border and a small square around each digit, letting whatever is behind
//    the overlay show through everywhere else -- framing each digit's square with its
//    own border individually instead of framing the whole strip with one, which already
//    makes a separator between tips redundant, so checking it forces (and, in the
//    dialog, hides) "hide separator" too, though "hide border" still applies, now to
//    each of those per-tip borders,
//  - "font": the face + style (weight/italic/underline/strikeout) the numbers are drawn
//    with, chosen from the font dialog; unset means the taskbar's own font (the fallback),
//  - "refresh interval": how often (ms) the shown overlay re-checks the taskbar buttons
//    and rebuilds the bar in place, and
//  - "poll interval": how often (ms) the app reconciles the overlay's visibility from the
//    keyboard hook.
// The tip-size preference controls how thick the numbered strip is
// perpendicular to the taskbar's long axis (the tip height for a horizontal taskbar,
// the tip width for a side-docked one). It is an integer percentage in [0, 100] of a
// full taskbar-button thickness:
//   <= the default's share -> the built-in slim default thickness (unchanged behavior),
//   100                    -> a full taskbar-button-sized cell,
// scaled linearly in between (and the number text scales with it). The strip is never
// thinner than the default, so the Preferences dialog only offers percentages from the
// default's share up to 100. A fresh INI (0) therefore renders as the default. The
// default's share is computed from the live taskbar via Overlay::TipSizeBounds; see
// Overlay::Refresh.
namespace Preferences {

// The tip-size percentage (see the file header for its meaning) is an integer in
// [kMinPercent, kMaxPercent] of a full taskbar-button thickness.
constexpr int kMinPercent = 0;
constexpr int kMaxPercent = 100;

// The overlay opacity percentage: how opaque the overlay window is, applied as its
// layered-window alpha (100 = fully opaque, unchanged from the pre-opacity behavior).
// Kept away from 0 so the overlay can never be made fully invisible while still active.
constexpr int kMinOpacityPercent     = 10;
constexpr int kMaxOpacityPercent     = 100;
constexpr int kDefaultOpacityPercent = 100;

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

// Current tip-size percentage in [0, 100] (see the file header for its meaning).
[[nodiscard]] int TipSizePercent();

// Store the tip-size percentage (clamped to [0, 100]) both in memory and in the INI
// file next to the executable (WritePrivateProfileString).
void SetTipSizePercent(int percent);

// Current overlay opacity percentage in [kMinOpacityPercent, kMaxOpacityPercent] (see the
// file header for its meaning). 100 (fully opaque) matches the overlay's pre-opacity look.
[[nodiscard]] int OpacityPercent();

// Store the opacity percentage (clamped to [kMinOpacityPercent, kMaxOpacityPercent]) both
// in memory and in the INI file next to the executable.
void SetOpacityPercent(int percent);

// The overlay's on/off rendering toggles (see the file header), packed as single-bit
// fields and bundled in one struct rather than kept as separate bools so the model, the
// dialog, and the INI persistence can load/save/compare them together.
struct RenderFlags {
    // Strip filled with the number color, numbers drawn in the bar color (a highlighted
    // look).
    bool invertColors : 1 = false;
    // Shrink the strip's window down to a border and a small square around each digit,
    // letting whatever is behind the overlay show through everywhere else (see
    // ApplyStripRegion). The border frames each digit's square individually rather than
    // the whole strip (see PaintBorder), which already makes a separator between tips
    // redundant, so this forces hideSeparator true as well (the dialog hides that
    // checkbox while this is checked); hideBorder still applies as normal, now to those
    // per-tip borders.
    bool compact       : 1 = false;
    // Suppress the border: the whole strip's outer border normally, or each tip's own
    // border individually when compact is set (see PaintBorder).
    bool hideBorder    : 1 = false;
    // Suppress the divider line(s) between number tips.
    bool hideSeparator : 1 = false;

    [[nodiscard]] friend bool operator==(const RenderFlags&, const RenderFlags&) = default;
};

// Current rendering toggles (see RenderFlags).
[[nodiscard]] const RenderFlags& Flags();

// Store 'flags' both in memory and in the INI file next to the executable.
void SetFlags(const RenderFlags& flags);

// Whether the overlay draws the numbers with a user-selected font (FontIsSet) and, if so,
// the LOGFONT describing it. Only the face and style fields (lfFaceName, lfWeight,
// lfItalic, lfUnderline, lfStrikeOut) are meaningful: the overlay drives the glyph height
// from the taskbar font scaled by the tip size, so the stored height is ignored there.
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
