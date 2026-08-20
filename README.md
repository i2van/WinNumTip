# WinNumTip

[![Latest build](https://github.com/i2van/WinNumTip/workflows/build/badge.svg)](https://github.com/i2van/WinNumTip/actions)
[![Latest release](https://img.shields.io/github/downloads/i2van/WinNumTip/total.svg)](https://github.com/i2van/WinNumTip/releases/latest)
[![License](https://img.shields.io/badge/license-MIT-yellow)](https://opensource.org/licenses/MIT)

**WinNumTip** shows the [`Win`+`number (0..9)` shortcut](https://support.microsoft.com/en-us/windows/keyboard-shortcuts-in-windows-dcc61a57-8ff0-cffe-9796-cb9706c75eec#taskbar)
for each app pinned to the taskbar. While you hold the `Win` key, a numbered
strip appears over the taskbar buttons; release the key and it disappears.

Hold `Win` and press a number to launch (or switch to) the app in that taskbar
position - **WinNumTip** just reminds you which number is which.

## Disclaimer

> [!IMPORTANT]
> **WinNumTip** installs a global keyboard [hook](https://learn.microsoft.com/en-us/windows/win32/winmsg/about-hooks)
> so it can detect when the `Win` key is held. This is a **normal** Windows mechanism,
> but because keyboard hooks are also used by keyloggers, some antivirus tools **may** flag the app.
>
> **WinNumTip** reads **only** the `Win` key state
> to toggle the strip and **does not** record or transmit any keystrokes.

## Features

- Numbered tips over taskbar buttons while the `Win` key is held.
- Works with the taskbar docked on any edge.
- Adjustable tip size via the notification area icon's **Preferences** dialog - from the
  default slim strip up to the full taskbar-button size (tip height for a
  horizontal taskbar, tip width for a vertical one); the numbers scale to match.
- Adjustable tip font, inverted colors, compact view, independent border/separator hiding,
  tip opacity and tunable refresh/`Win` key poll intervals.
- **Update** link in the **About** dialog whenever a newer release is published.

## Preferences

Double-click the notification area icon, right-click it and choose **Preferences...** or click
**Preferences** in the **About** dialog to open the settings dialog.

Settings are saved next to the app in `WinNumTip.ini`. Click **Apply** to save them
without closing the dialog or **OK** to save and close; applied settings take effect
immediately.

> [!TIP]
> Hold `Win` to keep the strip showing, then adjust settings in **Preferences** and click
> **Apply** to preview the change live on the taskbar without releasing `Win` or closing
> the dialog. In the font selector, double-click an entry in the **Font** or **Font
> style** list to apply it the same way.

- **Tip size** - how thick the numbered strip is across the taskbar: its height on a
  horizontal taskbar, its width on a side-docked one. Ranges from **Default** (the slim
  built-in strip) up to a full **Taskbar button** cell and the numbers scale to match.

  > `WinNumTip.ini` key: `TipSizePercent`

- **Tip font** - click **Select...** to pick the typeface and style (weight, italic, underline,
  strikeout) the numbers are drawn with. When no font is chosen the taskbar's own font is used
  (**Default**). In the font selector, click **Apply** - or double-click an entry in the
  **Font** or **Font style** list - to save the current selection without closing the selector.

  > `WinNumTip.ini` keys: `FontFace`, `FontWeight`, `FontItalic`, `FontUnderline`, `FontStrikeOut`

- **Invert colors** - swaps the strip and number colors for a highlighted look. Off by
  default.

  > `WinNumTip.ini` key: `InvertColors`

- **Compact view** - shrinks the strip's window down to a border and a small square around
  each digit.

  > `WinNumTip.ini` key: `CompactView`

- **Hide border** - hides the strip's outer border or (in **Compact view** mode) each tip's
  own border individually. Off by default (border shown).

  > `WinNumTip.ini` key: `HideBorder`

- **Hide separator** - hides the divider lines between number tips. Off by default
  (separators shown).

  > `WinNumTip.ini` key: `HideSeparator`

- **Tip opacity** - how opaque the strip window is, as a percentage. Range `10`-`100`%,
  default `100`% (fully opaque).

  > `WinNumTip.ini` key: `OpacityPercent`

- **Tips refresh** - how often, in milliseconds, the numbered tips re-sync with the
  taskbar buttons while shown, so the numbers stay aligned when a button appears,
  disappears or moves. Range `50`-`1000` ms, default `200` ms; lower is more responsive
  but polls more often.

  > `WinNumTip.ini` key: `RefreshIntervalMs`

- **Win key poll** - how often, in milliseconds, the app checks the `Win` key state to show
  or hide the strip. Range `25`-`500` ms, default `75` ms; lower reacts faster to pressing
  or releasing `Win` but polls more often.

  > `WinNumTip.ini` key: `PollIntervalMs`

- **Reset to defaults** - restores every option above to its factory default in the dialog;
  the change is saved only if you then click **Apply** or **OK**.

## Updates

Every time the **About** dialog is opened update check is performed until a newer version is
found, an [Update](https://github.com/i2van/WinNumTip/releases/latest) link appears if new version is available.

## Requirements

- Windows 10/11

## Troubleshooting

Sometimes numbered strip does not arrear, to fix that `Alt`+`Tab` couple of times.

## How to build

Open `WinNumTip.slnx` in Visual Studio and build or from a developer prompt for `x64` build:

```powershell
msbuild WinNumTip.slnx /t:Build /p:Configuration=Release /p:Platform=x64
```

Artifacts can be found in `build\<Platform>\Release` dir.

### Platforms supported

- `x64`
- `Win32`
- `ARM64`
