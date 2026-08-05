#include "stdafx.h"
#include "DialogIcon.h"
#include "resource.h"

namespace DialogIcon {

void Set(HWND wnd, HINSTANCE inst) {
    // LR_SHARED so these icons are managed by the system and need no DestroyIcon.
    const HICON iconBig   = WinAPI::Icon::Load(inst, IDI_APPICON, SM_CXICON,   SM_CYICON,   LR_SHARED);
    const HICON iconSmall = WinAPI::Icon::Load(inst, IDI_APPICON, SM_CXSMICON, SM_CYSMICON, LR_SHARED);
    WinAPI::Window::SetIcon(wnd, ICON_BIG,   iconBig);
    WinAPI::Window::SetIcon(wnd, ICON_SMALL, iconSmall);
}

} // namespace DialogIcon
