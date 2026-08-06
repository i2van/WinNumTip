#include "stdafx.h"
#include "NotifyIcon.h"
#include "resource.h"

namespace {

// NOTIFYICONDATA uID for our single notification area icon.
constexpr UINT kNotifyIconId = 1;

NOTIFYICONDATA g_nid;
HINSTANCE      g_inst = nullptr;
HWND           g_msgWnd = nullptr;
UINT           g_taskbarCreated = 0;   // "TaskbarCreated" broadcast (Explorer restart)
HICON          g_ownedIcon = nullptr;  // icon we must DestroyIcon (from LoadImage); null if shared

} // namespace

namespace NotifyIcon {

void Add(HINSTANCE inst, HWND msgWnd) {
    g_inst = inst;
    g_msgWnd = msgWnd;
    // Registered once; the same id is returned for every caller in the session.
    if (!g_taskbarCreated) g_taskbarCreated = RegisterWindowMessage(TEXT("TaskbarCreated"));

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = msgWnd;
    g_nid.uID = kNotifyIconId;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_NOTIFYICON;
    // LoadImage (LR_DEFAULTCOLOR, not LR_SHARED) yields a caller-owned icon we must
    // DestroyIcon; free any previously-owned one first (Add re-runs on Explorer
    // restart). The LoadIcon fallback returns a shared icon that must not be destroyed.
    WinAPI::Icon::Destroy(g_ownedIcon);
    g_nid.hIcon = WinAPI::Icon::Load(inst, IDI_APPICON, SM_CXSMICON, SM_CYSMICON, LR_DEFAULTCOLOR);
    if (g_nid.hIcon) g_ownedIcon = g_nid.hIcon;
    else             g_nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);

    LoadStr(inst, IDS_TOOLTIP, g_nid.szTip);

    VERIFY(Shell_NotifyIcon(NIM_ADD, &g_nid));
}

void Remove() {
    VERIFY(Shell_NotifyIcon(NIM_DELETE, &g_nid));
    WinAPI::Icon::Destroy(g_ownedIcon);
}

bool HandleTaskbarCreated(UINT msg) {
    if (msg == 0 || msg != g_taskbarCreated) return false;
    // Explorer restarted and dropped our icon; re-create it.
    Add(g_inst, g_msgWnd);

    return true;
}

void ShowMenu(HINSTANCE inst, bool show, HWND commandTarget, HWND menuOwner) {
    if (!menuOwner || !IsWindow(menuOwner)) menuOwner = commandTarget;

    // Only call when needed: menuOwner may already be the foreground window (e.g. the
    // font-picker dialog, just foregrounded by the WM_NOTIFYICON handler's
    // PreferencesDialog::ActivateDialog). That dialog treats any further SetForegroundWindow
    // on it as a fresh, OS-driven activation (see FontPickerHelper's OnChooseFontActivate) and
    // reacts by bouncing Preferences back to the foreground on top of it -- so a redundant
    // call here would undo the z-order the caller just established. The invariant this needs
    // -- menuOwner must *be* foreground before TrackPopupMenu, so the menu dismisses correctly
    // -- already holds once it's already the foreground window.
    if (GetForegroundWindow() != menuOwner) SetForegroundWindow(menuOwner);

    if(!show) return;

    const HMENU menu = LoadMenu(inst, MAKEINTRESOURCE(IDR_NOTIFYICONMENU));
    if (!menu) return;
    const HMENU sub = GetSubMenu(menu, 0);
    VERIFY(SetMenuDefaultItem(sub, IDM_PREFERENCES, FALSE));
    POINT pt;
    VERIFY(GetCursorPos(&pt));

    const UINT command = TrackPopupMenu(sub, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                        pt.x, pt.y, 0, menuOwner, nullptr);
    PostMessage(commandTarget, WM_NULL, 0, 0);
    VERIFY(DestroyMenu(menu));
    if (command) VERIFY(PostMessage(commandTarget, WM_COMMAND, MAKEWPARAM(command, 0), 0));
}

} // namespace NotifyIcon
