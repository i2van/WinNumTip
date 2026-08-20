#include "stdafx.h"
#include "About.h"
#include "DialogIcon.h"
#include "UpdateCheck.h"
#include "resource.h"

namespace {

// The live modal About dialog, or null when none is open. Enforces "only one About
// dialog" and lets a repeat request re-focus the existing one instead of opening another.
HWND g_dlg;
// Bold font applied to the app-name header; owned here and freed on WM_DESTROY.
HFONT g_boldFont;

// Fill the single "%s" in control 'id''s .rc template text with 'value' (e.g. turn the
// About dialog's "WinNumTip %s" into "WinNumTip 1.1"). The display text stays in the
// resource (editable there); only the build-supplied value is substituted here.
void FormatDlgItemText(HWND dlg, int id, LPCTSTR value) {
    TCHAR fmt[64];
    const UINT gotFmt = GetDlgItemText(dlg, id, fmt, ARRAYSIZE(fmt));
    ASSERT(gotFmt);
    if (gotFmt) {
        TCHAR text[128];
        VERIFY(SetDlgItemText(dlg, id, WinAPI::String::Format(text, fmt, value)));
    }
}

// Show -- or leave hidden -- the "Update" link that offers the newer release the background
// check found (see UpdateCheck). Where the link sits is IDD_ABOUT's business: it has a fixed
// spot after the app-name/version text, outside the name label's own rectangle so that
// neither control can paint over the other, and nothing here moves or resizes it.
void ApplyUpdateState(HWND dlg) {
    const HWND link = GetDlgItem(dlg, IDC_ABOUT_UPDATE);
    ASSERT(link);
    ShowWindow(link, UpdateCheck::IsAvailable() ? SW_SHOW : SW_HIDE);
}

BOOL OnInitDialog(HWND dlg, HWND /*focus*/, LPARAM lParam) {
    g_dlg = dlg;
    const HINSTANCE inst = reinterpret_cast<HINSTANCE>(lParam);

    // Give the dialog the app icon: it is owned by the hidden tool window (which has no
    // class icon), so without this Alt+Tab / the taskbar show the generic default icon.
    DialogIcon::Set(dlg, inst);

    // Bold the app-name header and the "Update" link that follows it: derive a bold copy of
    // the dialog's own (already DPI-scaled) font so the weight change keeps the same face
    // and size. One font serves both controls -- it is owned here and freed on WM_DESTROY.
    LOGFONT lf;
    if (WinAPI::Font::GetLogFont(GetWindowFont(dlg), lf)) {
        lf.lfWeight = FW_BOLD;
        g_boldFont = CreateFontIndirect(&lf);
        ASSERT(g_boldFont);
        if (g_boldFont) {
            SetWindowFont(GetDlgItem(dlg, IDC_ABOUT_NAME),   g_boldFont, TRUE);
            SetWindowFont(GetDlgItem(dlg, IDC_ABOUT_UPDATE), g_boldFont, TRUE);
        }
    }

    SendMessage(dlg, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(GetDlgItem(dlg, IDC_ABOUT_README)), TRUE);

    FormatDlgItemText(dlg, IDC_ABOUT_NAME, APP_VERSION);

    // Offer the update straight away when a check earlier in this session already found one
    // (so a re-opened About does not have to wait for the network again), then ask github.com
    // once more unless that has already happened: an open that starts without an update in
    // hand re-checks, so a release published while the app has been running is picked up
    // without restarting it. The check runs on its own thread and reports back with
    // WM_UPDATECHECK, so nothing here blocks the dialog from appearing.
    ApplyUpdateState(dlg);
    UpdateCheck::Start(dlg);

    return FALSE;
}

void OnDestroy(HWND dlg) {
    // Stop the check (if one is still in flight) from posting WM_UPDATECHECK to this dialog
    // once it is gone; its result stays cached for the next time About is opened.
    UpdateCheck::Stop(dlg);
    WinAPI::GdiObject::Delete(g_boldFont);
}

BOOL OnNotify(HWND dlg, int idCtrl, NMHDR* hdr) {
    // The "Preferences" action link (no href) closes About and opens the Preferences dialog
    // the way the notification area menu does: post IDM_PREFERENCES to our owner (the
    // hidden message window), which runs the same WM_COMMAND path (see WinNumTip.cpp's
    // OnCommand).
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

    // A SysLink (the description's "Win+number shortcut", README/Project site, or the
    // "Update" link offering a newer release) was clicked (mouse) or activated (Enter): open
    // its URL, embedded in the control's <a href="..."> markup (see IDD_ABOUT).
    if ((idCtrl == IDC_ABOUT_DESC ||
         idCtrl == IDC_ABOUT_README ||
         idCtrl == IDC_ABOUT_PROJECT_SITE ||
         idCtrl == IDC_ABOUT_UPDATE) &&
        (hdr->code == NM_CLICK || hdr->code == NM_RETURN)) {
        const PNMLINK link = reinterpret_cast<PNMLINK>(hdr);
        WinAPI::Url::Open(dlg, link->item.szUrl);

        return TRUE;
    }

    return FALSE;
}

void OnCommand(HWND dlg, int id, HWND /*ctl*/, UINT /*notify*/) {
    if (id == IDOK || id == IDCANCEL) VERIFY(EndDialog(dlg, id));
}

// WM_SYSCOMMAND: intercept the title-bar "?" (context-help) button that the DS_CONTEXTHELP
// style adds, opening the README (see WinAPI::Url::OpenOnContextHelp).
void OnSysCommand(HWND dlg, UINT cmd, int x, int y) {
    WinAPI::Url::OpenOnContextHelp(dlg, cmd, x, y, TEXT("https://github.com/i2van/WinNumTip/blob/main/README.md"));
}

INT_PTR CALLBACK AboutProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        HANDLE_MSG(dlg, WM_INITDIALOG, OnInitDialog);
        HANDLE_MSG(dlg, WM_DESTROY,    OnDestroy);
        HANDLE_MSG(dlg, WM_NOTIFY,     OnNotify);
        HANDLE_MSG(dlg, WM_COMMAND,    OnCommand);
        case WM_UPDATECHECK:
            // The background release check finished (UpdateCheck::Start, posted from its
            // worker thread): reveal the "Update" link when it found a newer release.
            ApplyUpdateState(dlg);
            return TRUE;
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
    // re-invoking About (e.g. from the notification area) resurfaces that Preferences
    // dialog rather than hiding it behind About.
    if (g_dlg) { WinAPI::Dialog::Foreground(g_dlg); return; }
    VERIFY(DialogBoxParam(inst, MAKEINTRESOURCE(IDD_ABOUT), owner, AboutProc, reinterpret_cast<LPARAM>(inst)) != -1);
    g_dlg = nullptr;
}

} // namespace About
