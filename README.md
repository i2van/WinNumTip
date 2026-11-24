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
- Adjustable tip size via the notification area icon's [Preferences](#preferences) dialog - from the
  slim built-in strip up to the full taskbar-button size (tip height for a
  horizontal taskbar, tip width for a vertical one); the numbers scale to match.
- Adjustable tip font, dark theme, compact view, independent border/separator hiding,
  show animation, tip opacity and tunable refresh/`Win` key poll intervals.
- Optional start with Windows for the current user.
- [Update](https://github.com/i2van/WinNumTip/releases/latest) link in the **About** dialog whenever a newer release is published.

## Installation

**WinNumTip** is portable: there is nothing to set up system-wide, the app runs from wherever
it is unpacked and keeps its settings in `WinNumTip.ini` next to the executable.

### winget

#### Install

```powershell
winget install --id i2van.WinNumTip
```

#### Update

```powershell
winget upgrade --id i2van.WinNumTip
```

> [!IMPORTANT]
> Exit **WinNumTip** first - right-click the notification area icon and choose **Exit**.

#### Uninstall

```powershell
winget uninstall --id i2van.WinNumTip
```

> [!IMPORTANT]
> Exit **WinNumTip** first - right-click the notification area icon and choose **Exit**.
>
> If **Start with Windows** is enabled, turn it off in [Preferences](#preferences) beforehand,
> so no leftover startup entry points at the removed app.

### Manual

Download `WinNumTip-<platform>.zip` for your [platform](#platforms-supported) from the
[latest release](https://github.com/i2van/WinNumTip/releases/latest), unpack it anywhere and
run `WinNumTip.exe`.

To update, exit the app and unpack the newer release over the previous one; the
`WinNumTip.ini` settings file is left untouched.

## Preferences

Double-click the notification area icon, right-click it and choose **Preferences...** or click
**Preferences** in the **About** dialog to open the settings dialog.

Settings are saved next to the app in `WinNumTip.ini`. Click **Apply** to save them
without closing the dialog or **OK** to save and close; applied settings take effect
immediately.

On/off settings are saved as `yes` or `no`; when editing the file by hand, `on`/`off`,
`true`/`false` and `1`/`0` are accepted too, in any letter case.

To find settings file, open the dialog's window menu (click its title-bar icon or press
`Alt`+`Space`) and choose **Navigate to file**: Windows Explorer opens with
`WinNumTip.ini` selected or the app's folder when no settings have been saved yet.

> [!TIP]
> Hold `Win` to keep the strip showing, then adjust settings in **Preferences** and click
> **Apply** to preview the change live on the taskbar without releasing `Win` or closing
> the dialog. In the font selector, double-click an entry in the **Font** or **Font
> style** list to apply it the same way.

### Tip size

How thick the numbered strip is across the taskbar: its height on a
horizontal taskbar, its width on a side-docked one. Ranges from **Default** (the slim
built-in strip) up to a full **Taskbar button** cell and the numbers scale to match.
`80`% by default.

> `WinNumTip.ini` key: `TipSizePercent`

### Tip font

Click **Select...** to pick the typeface and style (weight, italic, underline,
strikeout) the numbers are drawn with. When no font is chosen the taskbar's own font is used
(**Default**). In the font selector, click **Apply** - or double-click an entry in the
**Font** or **Font style** list - to save the current selection without closing the selector.
The selector opens next to **Preferences** (centered over it when there is no room on either
side), leaves **Preferences** usable, and closes along with it, discarding any selection not
yet applied.

> `WinNumTip.ini` keys: `FontFace`, `FontWeight`, `FontItalic`, `FontUnderline`, `FontStrikeOut`

### Dark theme

Draws the strip dark with light numbers. Defaults to the Windows mode
(**Settings** > **Personalization** > **Colors** > **Choose your default Windows mode**)
on the first run (no `WinNumTip.ini` yet) and on **Reset to defaults**.

> `WinNumTip.ini` key: `DarkTheme`

### Compact view

Shrinks the strip's window down to a border and a small square around
each digit.

> `WinNumTip.ini` key: `CompactView`

### Hide border

Hides the strip's outer border or (in **Compact view** mode) each tip's
own border individually. Off by default (border shown).

> `WinNumTip.ini` key: `HideBorder`

### Hide separator

Hides the divider lines between number tips. Off by default
(separators shown).

> `WinNumTip.ini` key: `HideSeparator`

### Tip animation

The standard Windows effect used when the strip appears (hiding is
always instant): **Expand** (from the center), **Fade**, **None**, or **Roll** or
**Slide** (unfolding away from the taskbar). The default, **Windows default**, follows the
system settings - none when **Animation effects** or **Fade or slide ToolTips into view**
is turned off, otherwise fade or slide as Windows does for tooltips; the drop-down shows
the effect it currently resolves to. An explicit choice is used regardless of those
settings.

The **Duration** drop-down next to the effect sets how long it takes: `50`-`500` ms in `50`
ms steps, default `200` ms. It is disabled when the effect is **None**.

> `WinNumTip.ini` keys: `Animation` (`System`, `Expand`, `Fade`, `None`, `Roll` or `Slide`),
> `AnimationMs`

### Tip opacity

How opaque the strip window is, as a percentage. Range `10`-`100`%,
default `100`% (fully opaque).

> `WinNumTip.ini` key: `OpacityPercent`

### Tip refresh

How often, in milliseconds, the numbered tips re-sync with the
taskbar buttons while shown, so the numbers stay aligned when a button appears,
disappears or moves. Range `50`-`1000` ms, default `200` ms; lower is more responsive
but polls more often.

> `WinNumTip.ini` key: `RefreshIntervalMs`

### Win key poll

How often, in milliseconds, the app checks the `Win` key state to show
or hide the strip. Range `25`-`500` ms, default `75` ms; lower reacts faster to pressing
or releasing `Win` but polls more often.

> `WinNumTip.ini` key: `PollIntervalMs`

### Start with Windows

Starts **WinNumTip** automatically for the current user.

> `WinNumTip.ini` key: `StartWithWindows`

### Reset to defaults

Restores every option above to its factory default in the dialog;
the change is saved only if you then click **Apply** or **OK**.

## Updates

Every time the **About** dialog is opened update check is performed until a newer version is
found, an [Update](https://github.com/i2van/WinNumTip/releases/latest) link appears if new version is available.

The link opens the [latest release](https://github.com/i2van/WinNumTip/releases/latest) page,
or the [winget update](#update) instructions when **WinNumTip** was
[installed via winget](#winget) - the way that copy of the app is actually updated.

## Requirements

- Windows 10/11

## Troubleshooting

If the numbered strip does not appear, press `Alt`+`Tab` a couple of times.

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
