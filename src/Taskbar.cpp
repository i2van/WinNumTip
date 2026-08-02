#include "stdafx.h"
#include "Taskbar.h"

namespace {

constexpr LPCTSTR kTaskButtonClass = TEXT("Taskbar.TaskListButtonAutomationPeer");

// Collect the non-degenerate bounding rectangles of every element under 'root' that
// matches 'cond', writing up to 'max' of them to 'out' (in tree order == Win+0..9).
// When 'clip' is non-null a button is kept only if its rectangle intersects *clip --
// an on-screen guard used by the lenient fallback so genuine off-taskbar phantom peers
// are still dropped geometrically. Returns the number of rectangles written.
[[nodiscard]] int CollectRects(IUIAutomationElement* root, IUIAutomationCondition* cond,
                               const RECT* clip, RECT* out, int max) {
    int count = 0;
    IUIAutomationElementArray* arr = nullptr;
    if (SUCCEEDED(root->FindAll(TreeScope_Descendants, cond, &arr)) && arr) {
        int len = 0;
        arr->get_Length(&len);
        for (int i = 0; i < len && count < max; ++i) {
            IUIAutomationElement* be = nullptr;
            if (SUCCEEDED(arr->GetElement(i, &be)) && be) {
                RECT r;
                RECT tmp;
                // Skip degenerate (empty) rects as a second guard against
                // hidden/collapsed buttons, and (in the fallback) any button whose
                // rect does not land on the taskbar.
                if (SUCCEEDED(be->get_CurrentBoundingRectangle(&r)) &&
                    r.right > r.left && r.bottom > r.top &&
                    (!clip || IntersectRect(&tmp, &r, clip)))
                    out[count++] = r;
                be->Release();
            }
        }
        arr->Release();
    }

    return count;
}

} // namespace

namespace Taskbar {

HWND Find() {
    return FindWindow(TEXT("Shell_TrayWnd"), nullptr);
}

bool Obscured(HWND taskbar) {
    if (!IsWindowVisible(taskbar)) return true;
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

int CollectButtonRects(IUIAutomation* uia, HWND taskbar, RECT* out, int max) {
    if (!uia) return 0;

    IUIAutomationElement* taskbarEl = nullptr;
    if (FAILED(uia->ElementFromHandle(taskbar, &taskbarEl)) || !taskbarEl) return 0;

    // Match the taskbar app buttons by class name.
    VARIANT vName;
    vName.vt = VT_BSTR;
    vName.bstrVal = SysAllocString(kTaskButtonClass);

    // On-screen guard for the strict query: an app configured to close/minimize to the
    // notification area (e.g. Outlook) can leave an off-screen button peer in the UI
    // Automation tree, which would otherwise draw a phantom number over an empty taskbar
    // spot.
    VARIANT vOnScreen;
    vOnScreen.vt = VT_BOOL;
    vOnScreen.boolVal = VARIANT_FALSE;   // IsOffscreen == FALSE

    IUIAutomationCondition* condName = nullptr;
    IUIAutomationCondition* condOnScreen = nullptr;
    IUIAutomationCondition* condStrict = nullptr;
    uia->CreatePropertyCondition(UIA_ClassNamePropertyId, vName, &condName);
    uia->CreatePropertyCondition(UIA_IsOffscreenPropertyId, vOnScreen, &condOnScreen);
    if (condName && condOnScreen)
        uia->CreateAndCondition(condName, condOnScreen, &condStrict);

    int count = 0;

    // Primary: buttons matched by class name AND reported on-screen by UI Automation.
    // This is the common path and keeps phantom notification area peers out.
    if (condStrict)
        count = CollectRects(taskbarEl, condStrict, nullptr, out, max);

    // Fallback: some taskbar states transiently report every button's UI Automation
    // IsOffscreen as TRUE (e.g. right after a foreground / virtual-desktop switch, or
    // when the desktop has focus), which makes the strict query -- and therefore the
    // whole overlay -- come up empty until an Alt+Tab wakes the tree. That was the
    // "sometimes the strip does not appear" glitch. When nothing matched, retry on
    // class name alone and keep only buttons whose rectangle actually sits on the
    // taskbar, so real buttons still show while genuine off-taskbar phantoms are
    // excluded geometrically.
    if (count == 0 && condName) {
        RECT taskbarRc;
        if (GetWindowRect(taskbar, &taskbarRc))
            count = CollectRects(taskbarEl, condName, &taskbarRc, out, max);
    }

    if (condStrict) condStrict->Release();
    if (condOnScreen) condOnScreen->Release();
    if (condName) condName->Release();

    SysFreeString(vName.bstrVal);
    taskbarEl->Release();

    return count;
}

} // namespace Taskbar
