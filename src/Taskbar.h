#pragma once

struct IUIAutomation;

// Taskbar discovery and taskbar-button geometry (UI Automation based).
namespace Taskbar {

// The primary taskbar window (Shell_TrayWnd), or nullptr.
[[nodiscard]] HWND Find();

// True when tips should NOT be drawn: a full-screen app, presentation mode, or
// the taskbar itself is hidden.
[[nodiscard]] bool Obscured(HWND taskbar);

// Taskbar rectangle (screen pixels) and docked edge (ABE_*). Returns false on
// failure.
[[nodiscard]] bool GetPos(RECT& rc, UINT& edge);

// Fill up to 'max' app-button rectangles (left-to-right == Win+1..) into 'out'.
// Returns the count found. Checks the Windows version and queries whichever taskbar
// UI Automation tree matches -- Windows 11's by class name, or Windows 10's
// differently shaped one -- so both are supported without wasting a query neither
// OS can ever answer.
[[nodiscard]] int CollectButtonRects(IUIAutomation* uia, HWND taskbar, RECT* out, int max);

} // namespace Taskbar
