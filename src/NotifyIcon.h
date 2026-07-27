#pragma once

// Notification-area (system tray) icon and its context menu.
namespace NotifyIcon {

// Add the notification-area icon, routing its callbacks to 'msgWnd'. Also registers
// for the "TaskbarCreated" broadcast so the icon can be restored if Explorer restarts.
void Add(HINSTANCE inst, HWND msgWnd);

// Remove the notification-area icon.
void Remove();

// Show the context menu at the cursor. Commands are posted to 'commandTarget'; when
// 'menuOwner' is supplied, it stays active behind the menu instead of commandTarget.
void ShowMenu(HINSTANCE inst, bool show, HWND commandTarget, HWND menuOwner = nullptr);

// If 'msg' is the "TaskbarCreated" broadcast (sent when Explorer/taskbar restarts),
// re-add the icon and return true; otherwise return false. Call from the window proc.
[[nodiscard]] bool HandleTaskbarCreated(UINT msg);

} // namespace NotifyIcon
