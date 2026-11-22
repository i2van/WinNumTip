#pragma once

// Notification area icon and its context menu.
namespace NotifyIcon {

// Add the notification area icon, routing its callbacks to 'msgWnd'. Also registers
// for the "TaskbarCreated" broadcast so the icon can be restored if Explorer restarts.
void Add(HINSTANCE inst, HWND msgWnd);

// Remove the notification area icon.
void Remove();

// Show the context menu at the cursor, owned by 'commandTarget' (a hidden window of the
// calling thread), which receives its commands as posted WM_COMMANDs. With 'show' false the
// target is only made foreground.
void ShowMenu(HINSTANCE inst, bool show, HWND commandTarget);

// If 'msg' is the "TaskbarCreated" broadcast (sent when Explorer/taskbar restarts),
// re-add the icon and return true; otherwise return false. Call from the window proc.
[[nodiscard]] bool HandleTaskbarCreated(UINT msg);

} // namespace NotifyIcon
