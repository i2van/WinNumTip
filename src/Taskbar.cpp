#include "stdafx.h"
#include "Taskbar.h"

namespace {

LPCTSTR const kTaskButtonClass = TEXT("Taskbar.TaskListButtonAutomationPeer");

} // namespace

namespace Taskbar {

HWND Find() {
    return FindWindow(TEXT("Shell_TrayWnd"), nullptr);
}

bool Obscured(HWND tray) {
    if (!IsWindowVisible(tray)) return true;
    QUERY_USER_NOTIFICATION_STATE state = QUNS_ACCEPTS_NOTIFICATIONS;
    if (SUCCEEDED(SHQueryUserNotificationState(&state))) {
        if (state == QUNS_RUNNING_D3D_FULL_SCREEN ||
            state == QUNS_PRESENTATION_MODE ||
            state == QUNS_BUSY) {
            return true;
        }
    }

    return false;
}

bool GetPos(RECT& rc, UINT& edge) {
    APPBARDATA abd;
    ZeroMemory(&abd, sizeof(abd));
    abd.cbSize = sizeof(abd);
    if (!SHAppBarMessage(ABM_GETTASKBARPOS, &abd)) return false;
    rc = abd.rc;
    edge = abd.uEdge;

    return true;
}

int CollectButtonRects(IUIAutomation* uia, HWND tray, RECT* out, int max) {
    if (!uia) return 0;

    int count = 0;
    IUIAutomationElement* trayEl = nullptr;
    if (FAILED(uia->ElementFromHandle(tray, &trayEl)) || !trayEl) return 0;

    VARIANT vName;
    vName.vt = VT_BSTR;
    vName.bstrVal = SysAllocString(kTaskButtonClass);

    // Match the taskbar app buttons by class name, but only those that are actually
    // on-screen: an app configured to close/minimize to the tray (e.g. Outlook) leaves
    // an off-screen button peer in the UI Automation tree, which would otherwise draw a
    // phantom number over an empty spot on the taskbar.
    VARIANT vOnScreen;
    vOnScreen.vt = VT_BOOL;
    vOnScreen.boolVal = VARIANT_FALSE;   // IsOffscreen == FALSE

    IUIAutomationCondition* condName = nullptr;
    IUIAutomationCondition* condOnScreen = nullptr;
    IUIAutomationCondition* cond = nullptr;
    if (SUCCEEDED(uia->CreatePropertyCondition(UIA_ClassNamePropertyId, vName, &condName)) && condName &&
        SUCCEEDED(uia->CreatePropertyCondition(UIA_IsOffscreenPropertyId, vOnScreen, &condOnScreen)) && condOnScreen) {
        uia->CreateAndCondition(condName, condOnScreen, &cond);
    }

    if (cond) {
        IUIAutomationElementArray* arr = nullptr;
        if (SUCCEEDED(trayEl->FindAll(TreeScope_Descendants, cond, &arr)) && arr) {
            int len = 0;
            arr->get_Length(&len);
            for (int i = 0; i < len && count < max; ++i) {
                IUIAutomationElement* be = nullptr;
                if (SUCCEEDED(arr->GetElement(i, &be)) && be) {
                    RECT r;
                    // Skip degenerate (empty) rects as a second guard against
                    // hidden/collapsed buttons.
                    if (SUCCEEDED(be->get_CurrentBoundingRectangle(&r)) &&
                        r.right > r.left && r.bottom > r.top)
                        out[count++] = r;
                    be->Release();
                }
            }
            arr->Release();
        }
        cond->Release();
    }
    if (condOnScreen) condOnScreen->Release();
    if (condName) condName->Release();

    SysFreeString(vName.bstrVal);
    trayEl->Release();

    return count;
}

} // namespace Taskbar
