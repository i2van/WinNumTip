#include "stdafx.h"
#include "DialogIcon.h"
#include "resource.h"

namespace DialogIcon {

void Set(HWND wnd, HINSTANCE inst) {
    // LR_SHARED so these icons are managed by the system and need no DestroyIcon.
    const HICON iconBig   = static_cast<HICON>(LoadImage(inst, MAKEINTRESOURCE(IDI_APPICON), IMAGE_ICON, GetSystemMetrics(SM_CXICON),   GetSystemMetrics(SM_CYICON),   LR_SHARED));
    const HICON iconSmall = static_cast<HICON>(LoadImage(inst, MAKEINTRESOURCE(IDI_APPICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    if (iconBig)   SendMessage(wnd, WM_SETICON, ICON_BIG,   reinterpret_cast<LPARAM>(iconBig));
    if (iconSmall) SendMessage(wnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(iconSmall));
}

} // namespace DialogIcon
