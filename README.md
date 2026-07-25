# WinNumTip

**WinNumTip** shows the [`Win`+`number (0..9)` shortcut](https://support.microsoft.com/en-us/windows/keyboard-shortcuts-in-windows-dcc61a57-8ff0-cffe-9796-cb9706c75eec#taskbar)
for each app pinned to the taskbar. While you hold the `Win` key, a small numbered
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
> to toggle the overlay and **does not** record or transmit any keystrokes.

## Features

- Numbered badges over taskbar buttons while the `Win` key is held.
- Works with the taskbar docked on any edge (bottom, top, left, right).
- Adjustable badge size via the tray icon's **Preferences** dialog - from the
  default slim strip up to the full taskbar-button size (label height for a
  horizontal taskbar, label width for a vertical one); the numbers scale to match.
- **Invert colors** option (same dialog) that swaps the strip and number colors for
  a highlighted look.
- Preferences are saved next to the app in `WinNumTip.ini`.

## Requirements

- Windows 10/11

## Troubleshooting

Sometimes numbered strip does not arrear, to fix that `Alt`+`Tab` couple of times.

## How to build

Open `WinNumTip.slnx` in Visual Studio and build, or from a developer prompt for `x64` build:

```powershell
msbuild WinNumTip.slnx /t:Build /p:Configuration=Release /p:Platform=x64
```

Artifacts can be found in `build\<Platform>\Release` dir.

### Platforms supported

- `x64`
- `Win32`
- `ARM64`
