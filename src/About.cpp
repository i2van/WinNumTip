#include "stdafx.h"
#include "About.h"
#include "resource.h"

namespace {

// The live modal About dialog, or null when none is open. Enforces "only one About
// dialog" and lets a repeat request re-focus the existing one instead of opening another.
HWND g_dlg = nullptr;
// Bold font applied to the app-name header; owned here and freed on WM_DESTROY.
HFONT g_boldFont = nullptr;

BOOL OnInitDialog(HWND dlg, HWND /*focus*/, LPARAM lParam) {
    g_dlg = dlg;
    const HINSTANCE inst = reinterpret_cast<HINSTANCE>(lParam);

    // Give the dialog the app icon: it is owned by the hidden tool window (which has no
    // class icon), so without this Alt+Tab / the taskbar show the generic default icon.
    const HICON iconBig   = static_cast<HICON>(LoadImage(inst, MAKEINTRESOURCE(IDI_APPICON), IMAGE_ICON, GetSystemMetrics(SM_CXICON),   GetSystemMetrics(SM_CYICON),   LR_SHARED));
    const HICON iconSmall = static_cast<HICON>(LoadImage(inst, MAKEINTRESOURCE(IDI_APPICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    if (iconBig)   SendMessage(dlg, WM_SETICON, ICON_BIG,   reinterpret_cast<LPARAM>(iconBig));
    if (iconSmall) SendMessage(dlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(iconSmall));

    // Bold the app-name header: derive a bold copy of the dialog's own (already
    // DPI-scaled) font so the weight change keeps the same face and size.
    const HFONT base = reinterpret_cast<HFONT>(SendMessage(dlg, WM_GETFONT, 0, 0));
    LOGFONT lf;
    if (base && GetObject(base, sizeof(lf), &lf)) {
        lf.lfWeight = FW_BOLD;
        g_boldFont = CreateFontIndirect(&lf);
        if (g_boldFont)
            SendDlgItemMessage(dlg, IDC_ABOUT_NAME, WM_SETFONT, reinterpret_cast<WPARAM>(g_boldFont), TRUE);
    }

    SendMessage(dlg, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(GetDlgItem(dlg, IDC_ABOUT_LINK)), TRUE);

    return FALSE;
}

void OnDestroy(HWND /*dlg*/) {
    if (g_boldFont) { DeleteObject(g_boldFont); g_boldFont = nullptr; }
}

BOOL OnNotify(HWND dlg, int idCtrl, NMHDR* hdr) {
    // A SysLink (the description's "Win+number shortcut", or README/Project site) was
    // clicked (mouse) or activated (Enter): open its URL, embedded in the control's
    // <a href="..."> markup (see IDD_ABOUT).
    if ((idCtrl == IDC_ABOUT_DESC || idCtrl == IDC_ABOUT_LINK) &&
        (hdr->code == NM_CLICK || hdr->code == NM_RETURN)) {
        const PNMLINK link = reinterpret_cast<PNMLINK>(hdr);
        VERIFY_SHELLEXEC(ShellExecute(dlg, TEXT("open"), link->item.szUrl, nullptr, nullptr, SW_SHOWNORMAL));

        return TRUE;
    }

    return FALSE;
}

void OnCommand(HWND dlg, int id, HWND /*ctl*/, UINT /*notify*/) {
    if (id == IDOK || id == IDCANCEL) EndDialog(dlg, id);
}

INT_PTR CALLBACK AboutProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        HANDLE_MSG(dlg, WM_INITDIALOG, OnInitDialog);
        HANDLE_MSG(dlg, WM_DESTROY,    OnDestroy);
        HANDLE_MSG(dlg, WM_NOTIFY,     OnNotify);
        HANDLE_MSG(dlg, WM_COMMAND,    OnCommand);
        default: return FALSE;
    }
}

} // namespace

namespace About {

void Show(HINSTANCE inst, HWND owner) {
    // Only one About dialog at a time: if it's already up, just bring it forward.
    if (g_dlg) { SetForegroundWindow(g_dlg); return; }
    // The SysLink window class is registered once at startup (InitCommonControlsEx in
    // Entry), so the modal dialog can be created directly here. 'inst' is forwarded as
    // the init param so OnInitDialog can load the app icon (WM_SETICON).
    DialogBoxParam(inst, MAKEINTRESOURCE(IDD_ABOUT), owner, AboutProc, reinterpret_cast<LPARAM>(inst));
    g_dlg = nullptr;
}

} // namespace About
