#include "stdafx.h"
#include "About.h"
#include "DialogIcon.h"
#include "resource.h"

// APP_VER_STR is injected by the build from Directory.Build.props (see WinNumTip.vcxproj).
// Stringize it to fill the About dialog's "%s" placeholder.
#define APP_STRINGIZE2(x) #x
#define APP_STRINGIZE(x)  APP_STRINGIZE2(x)

namespace {

// The live modal About dialog, or null when none is open. Enforces "only one About
// dialog" and lets a repeat request re-focus the existing one instead of opening another.
HWND g_dlg = nullptr;
// Bold font applied to the app-name header; owned here and freed on WM_DESTROY.
HFONT g_boldFont = nullptr;

// Fill the single "%s" in control 'id''s .rc template text with 'value' (e.g. turn the
// About dialog's "WinNumTip %s" into "WinNumTip 1.1"). The display text stays in the
// resource (editable there); only the build-supplied value is substituted here.
void FormatDlgItemText(HWND dlg, int id, LPCTSTR value) {
    TCHAR fmt[64];
    if (GetDlgItemText(dlg, id, fmt, ARRAYSIZE(fmt))) {
        TCHAR text[128];
        wsprintf(text, fmt, value);
        VERIFY(SetDlgItemText(dlg, id, text));
    }
}

BOOL OnInitDialog(HWND dlg, HWND /*focus*/, LPARAM lParam) {
    g_dlg = dlg;
    const HINSTANCE inst = reinterpret_cast<HINSTANCE>(lParam);

    // Give the dialog the app icon: it is owned by the hidden tool window (which has no
    // class icon), so without this Alt+Tab / the taskbar show the generic default icon.
    DialogIcon::Set(dlg, inst);

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

    FormatDlgItemText(dlg, IDC_ABOUT_NAME, TEXT(APP_STRINGIZE(APP_VER_STR)));

    return FALSE;
}

void OnDestroy(HWND /*dlg*/) {
    if (g_boldFont) { DeleteObject(g_boldFont); g_boldFont = nullptr; }
}

BOOL OnNotify(HWND dlg, int idCtrl, NMHDR* hdr) {
    // The "Preferences" action link (no href) closes About and opens the Preferences dialog
    // the way the tray menu does: post IDM_PREFERENCES to our owner (the hidden message
    // window), which runs the same WM_COMMAND path (see WinNumTip.cpp's OnCommand).
    // EndDialog merely flags About's modal loop to exit -- the flag is checked before the
    // loop retrieves its next message -- so About tears down first and the posted command is
    // picked up afterwards by the main message loop. Preferences therefore opens fresh and
    // owned by the message window (a sibling, same as from the menu), never nested inside a
    // still-open About. The owner is captured before EndDialog since 'dlg' is then closing.
    if (idCtrl == IDC_ABOUT_PREFS && (hdr->code == NM_CLICK || hdr->code == NM_RETURN)) {
        const HWND owner = GetWindow(dlg, GW_OWNER);
        VERIFY(EndDialog(dlg, IDCANCEL));
        VERIFY(PostMessage(owner, WM_COMMAND, MAKEWPARAM(IDM_PREFERENCES, 0), 0));

        return TRUE;
    }

    // A SysLink (the description's "Win+number shortcut", or README/Project site) was
    // clicked (mouse) or activated (Enter): open its URL, embedded in the control's
    // <a href="..."> markup (see IDD_ABOUT).
    if ((idCtrl == IDC_ABOUT_DESC || idCtrl == IDC_ABOUT_LINK) &&
        (hdr->code == NM_CLICK || hdr->code == NM_RETURN)) {
        const PNMLINK link = reinterpret_cast<PNMLINK>(hdr);
        OpenUrl(dlg, link->item.szUrl);

        return TRUE;
    }

    return FALSE;
}

void OnCommand(HWND dlg, int id, HWND /*ctl*/, UINT /*notify*/) {
    if (id == IDOK || id == IDCANCEL) VERIFY(EndDialog(dlg, id));
}

// WM_SYSCOMMAND: intercept the title-bar "?" (context-help) button that the DS_CONTEXTHELP
// style adds, opening the README (see OpenUrlOnContextHelp).
void OnSysCommand(HWND dlg, UINT cmd, int x, int y) {
    OpenUrlOnContextHelp(dlg, cmd, x, y, TEXT("https://github.com/i2van/WinNumTip/blob/main/README.md"));
}

INT_PTR CALLBACK AboutProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        HANDLE_MSG(dlg, WM_INITDIALOG, OnInitDialog);
        HANDLE_MSG(dlg, WM_DESTROY,    OnDestroy);
        HANDLE_MSG(dlg, WM_NOTIFY,     OnNotify);
        HANDLE_MSG(dlg, WM_COMMAND,    OnCommand);
        case WM_SYSCOMMAND:
            // Returning TRUE suppresses the dialog manager's own default for WM_SYSCOMMAND, so
            // the "?" button never enters help mode; OnSysCommand forwards the commands it does
            // not consume (move, close, ...) to the default handler itself.
            (void)HANDLE_WM_SYSCOMMAND(dlg, wParam, lParam, OnSysCommand);
            return TRUE;
        default: return FALSE;
    }
}

} // namespace

namespace About {

void Show(HINSTANCE inst, HWND owner) {
    // Only one About dialog at a time: if it's already up, bring it forward -- but via its
    // last active popup, so if a Preferences dialog was opened from it (and is thus on top),
    // re-invoking About (e.g. from the tray) resurfaces that Preferences dialog rather than
    // hiding it behind About.
    if (g_dlg) { ForegroundDialog(g_dlg); return; }
    VERIFY(DialogBoxParam(inst, MAKEINTRESOURCE(IDD_ABOUT), owner, AboutProc, reinterpret_cast<LPARAM>(inst)) != -1);
    g_dlg = nullptr;
}

} // namespace About
