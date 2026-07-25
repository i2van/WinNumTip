#include "stdafx.h"
#include "PreferencesDialog.h"
#include "DialogIcon.h"
#include "Overlay.h"
#include "Preferences.h"
#include "resource.h"

namespace {

// The live modal Preferences dialog, or null when none is open (enforces a single
// instance and lets a repeat request re-focus the existing one).
HWND g_dlg = nullptr;

// The slider never drops below this percent, so its minimum position ("Default") stays a
// distinct, selectable value even when the default strip's computed share rounds down to 0.
constexpr int kMinSliderPct = 1;

// Trackbar tuning: a tick mark every kSliderTickFreq percent, and kSliderPageStep percent
// per PageUp/PageDown.
constexpr int kSliderTickFreq = 10;
constexpr int kSliderPageStep = 10;

// The trackbar's minimum percentage: the default strip's share of a full taskbar button.
// Percentages below this render identically to the default, so they are excluded from the
// slider; the minimum position is shown as "Default" rather than a number.
int g_minPct = kMinSliderPct;

// Refresh the value readout ("Default" at the minimum, otherwise "NN%") from the trackbar.
void UpdateValueText(HWND dlg) {
    const int pos = static_cast<int>(SendDlgItemMessage(dlg, IDC_SIZE_SLIDER, TBM_GETPOS, 0, 0));
    TCHAR buf[16];
    if (pos <= g_minPct) lstrcpyn(buf, TEXT("Default"), ARRAYSIZE(buf));
    else                 wsprintf(buf, TEXT("%d%%"), pos);
    VERIFY(SetDlgItemText(dlg, IDC_SIZE_VALUE, buf));
}

BOOL OnInitDialog(HWND dlg, HWND /*focus*/, LPARAM lParam) {
    g_dlg = dlg;
    const HINSTANCE inst = reinterpret_cast<HINSTANCE>(lParam);

    // Give the dialog the app icon (its owner is the hidden tool window, which has no
    // class icon), shared with the About dialog.
    DialogIcon::Set(dlg, inst);

    // Resolve the default's share of a full taskbar button from the current taskbar
    // (shared with the renderer); it becomes the slider's minimum so every selectable
    // value changes the strip. The "Label size:" caption is fixed and orientation-neutral
    // (the strip is a height for a horizontal taskbar but a width for a side-docked one),
    // so no runtime relabeling is needed.
    int defThick = 0, btnThick = 0;
    bool vertical = false;
    g_minPct = kMinSliderPct;
    if (Overlay::LabelSizeBounds(defThick, btnThick, vertical) && btnThick > 0) {
        g_minPct = MulDiv(defThick, Preferences::kMaxPercent, btnThick);
        if (g_minPct < kMinSliderPct) g_minPct = kMinSliderPct;
        else if (g_minPct > Preferences::kMaxPercent) g_minPct = Preferences::kMaxPercent;
    }

    int cur = Preferences::LabelSizePercent();
    if (cur < g_minPct) cur = g_minPct; else if (cur > Preferences::kMaxPercent) cur = Preferences::kMaxPercent;

    const HWND tb = GetDlgItem(dlg, IDC_SIZE_SLIDER);
    SendMessage(tb, TBM_SETRANGE,    TRUE, MAKELPARAM(g_minPct, Preferences::kMaxPercent));
    SendMessage(tb, TBM_SETTICFREQ,  kSliderTickFreq, 0);
    SendMessage(tb, TBM_SETPAGESIZE, 0, kSliderPageStep);
    SendMessage(tb, TBM_SETPOS,      TRUE, cur);
    UpdateValueText(dlg);

    VERIFY(CheckDlgButton(dlg, IDC_INVERT, Preferences::InvertColors() ? BST_CHECKED : BST_UNCHECKED));

    // Focus the trackbar so the label size can be adjusted with the arrow keys as soon as
    // the dialog opens; returning FALSE tells the dialog manager to keep this focus rather
    // than moving it to the default first tab-stop itself.
    SetFocus(tb);
    return FALSE;
}

void OnHScroll(HWND dlg, HWND /*ctl*/, UINT /*code*/, int /*pos*/) {
    UpdateValueText(dlg);
}

void OnCommand(HWND dlg, int id, HWND /*ctl*/, UINT /*notify*/) {
    switch (id) {
        case IDOK:
            Preferences::SetLabelSizePercent(
                static_cast<int>(SendDlgItemMessage(dlg, IDC_SIZE_SLIDER, TBM_GETPOS, 0, 0)));
            Preferences::SetInvertColors(IsDlgButtonChecked(dlg, IDC_INVERT) == BST_CHECKED);
            VERIFY(EndDialog(dlg, IDOK));
            break;
        case IDCANCEL:
            VERIFY(EndDialog(dlg, IDCANCEL));
            break;
    }
}

// The "Reset to defaults" SysLink was clicked (mouse) or activated (Enter): restore the
// dialog's controls to their factory defaults -- the slim "Default" strip and colors not
// inverted. Only the controls are reset here; the change is persisted only if the user
// then confirms with OK, so Cancel still discards it (matching the OK/Cancel semantics).
BOOL OnNotify(HWND dlg, int idCtrl, NMHDR* hdr) {
    if (idCtrl == IDC_RESET && (hdr->code == NM_CLICK || hdr->code == NM_RETURN)) {
        SendDlgItemMessage(dlg, IDC_SIZE_SLIDER, TBM_SETPOS, TRUE, g_minPct);
        VERIFY(CheckDlgButton(dlg, IDC_INVERT, BST_UNCHECKED));
        UpdateValueText(dlg);

        return TRUE;
    }

    return FALSE;
}

INT_PTR CALLBACK PreferencesProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        HANDLE_MSG(dlg, WM_INITDIALOG, OnInitDialog);
        HANDLE_MSG(dlg, WM_HSCROLL,    OnHScroll);
        HANDLE_MSG(dlg, WM_COMMAND,    OnCommand);
        HANDLE_MSG(dlg, WM_NOTIFY,     OnNotify);
        default: return FALSE;
    }
}

} // namespace

namespace PreferencesDialog {

void Show(HINSTANCE inst, HWND owner) {
    // Only one Preferences dialog at a time: if it's already up, bring it (via its last
    // active popup) forward.
    if (g_dlg) { SetForegroundWindow(GetLastActivePopup(g_dlg)); return; }
    // The trackbar common-control class is registered once at startup (InitCommonControlsEx
    // in Entry). 'inst' is forwarded so OnInitDialog can load the app icon.
    VERIFY(DialogBoxParam(inst, MAKEINTRESOURCE(IDD_PREFERENCES), owner, PreferencesProc, reinterpret_cast<LPARAM>(inst)) != -1);
    g_dlg = nullptr;
}

} // namespace PreferencesDialog
