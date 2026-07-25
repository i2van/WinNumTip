#pragma once

// Persistent user preferences, stored in an INI file next to the executable (loaded and
// saved with the Get*/WritePrivateProfile* WinAPI). This is the model only; the editing
// UI lives in PreferencesDialog.
//
// Two preferences today:
//  - the overlay "label size" (see below), and
//  - "invert colors": whether the strip and number colors are swapped for a highlighted
//    look.
// The only preference today is the overlay "label size": how thick the numbered strip is
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

} // namespace Preferences
