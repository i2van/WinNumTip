#include "stdafx.h"
#include "PreferencesDialog.h"
#include "DialogIcon.h"
#include "FontPicker.h"
#include "FontPickerHelper.h"
#include "Overlay.h"
#include "Preferences.h"
#include "resource.h"

#define WM_FONT_PICKER_APPLY_SELECTION FontPickerHelper::kApplySelectionMessage
#define HANDLE_WM_FONT_PICKER_APPLY_SELECTION(hwnd, wParam, lParam, fn) \
    ((fn)((hwnd), static_cast<FontPickerHelper::Result>(static_cast<LONG>(wParam))) \
         ? TRUE \
         : FALSE)

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

// Timer-interval trackbar tuning (the ms ranges live in Preferences): a tick mark and a
// PageUp/PageDown step every N milliseconds.
constexpr int kRefreshTickFreq = 50;
constexpr int kRefreshPageStep = 50;
constexpr int kPollTickFreq    = 25;
constexpr int kPollPageStep    = 25;

// The trackbar's minimum percentage: the default strip's share of a full taskbar button.
// Percentages below this render identically to the default, so they are excluded from the
// slider; the minimum position is shown as "Default" rather than a number.
int g_minPct = kMinSliderPct;

// Value-readout templates captured from the dialog's own statics in OnInitDialog (before the
// first update overwrites them) so all display text lives in WinNumTip.rc: the size readout's
// "%d%%" and each interval readout's "%d ms". g_defaultLabel holds IDS_DEFAULT ("Default"),
// shown for the size readout at the slider minimum.
TCHAR g_defaultLabel[16];
TCHAR g_sizeFormat[16];
TCHAR g_refreshFormat[16];
TCHAR g_pollFormat[16];

// Refresh the size readout from the trackbar: "Default" at the minimum, otherwise the "%d%%"
// template applied to the percentage.
void UpdateValueText(HWND dlg) {
    const int pos = static_cast<int>(SendDlgItemMessage(dlg, IDC_SIZE_SLIDER, TBM_GETPOS, 0, 0));
    TCHAR buf[16];
    if (pos <= g_minPct) lstrcpyn(buf, g_defaultLabel, ARRAYSIZE(buf));
    else                 wsprintf(buf, g_sizeFormat, pos);
    VERIFY(SetDlgItemText(dlg, IDC_SIZE_VALUE, buf));
}

// Refresh a timer-interval readout from its trackbar using the control's captured "%d ms"
// template.
void UpdateMsText(HWND dlg, int sliderId, int valueId, LPCTSTR fmt) {
    const int pos = static_cast<int>(SendDlgItemMessage(dlg, sliderId, TBM_GETPOS, 0, 0));
    TCHAR buf[16];
    wsprintf(buf, fmt, pos);
    VERIFY(SetDlgItemText(dlg, valueId, buf));
}

void SetApplyEnabled(HWND dlg, bool enabled) {
    const HWND apply = GetDlgItem(dlg, IDC_APPLY);
    if (!enabled && GetFocus() == apply) SetFocus(GetDlgItem(dlg, IDOK));
    EnableWindow(apply, enabled ? TRUE : FALSE);
}

void UpdateApplyState(HWND dlg) {
    int savedSize = Preferences::LabelSizePercent();
    if (savedSize < g_minPct) savedSize = g_minPct;
    else if (savedSize > Preferences::kMaxPercent) savedSize = Preferences::kMaxPercent;

    const bool changed =
        static_cast<int>(SendDlgItemMessage(dlg, IDC_SIZE_SLIDER, TBM_GETPOS, 0, 0)) != savedSize ||
        static_cast<int>(SendDlgItemMessage(dlg, IDC_REFRESH_SLIDER, TBM_GETPOS, 0, 0)) != Preferences::RefreshIntervalMs() ||
        static_cast<int>(SendDlgItemMessage(dlg, IDC_POLL_SLIDER, TBM_GETPOS, 0, 0)) != Preferences::PollIntervalMs() ||
        (IsDlgButtonChecked(dlg, IDC_INVERT) == BST_CHECKED) != Preferences::InvertColors() ||
        FontPicker::HasChanges();
    SetApplyEnabled(dlg, changed);
}

void SaveValues(HWND dlg) {
    Preferences::SetLabelSizePercent(
        static_cast<int>(SendDlgItemMessage(dlg, IDC_SIZE_SLIDER, TBM_GETPOS, 0, 0)));
    Preferences::SetRefreshIntervalMs(
        static_cast<int>(SendDlgItemMessage(dlg, IDC_REFRESH_SLIDER, TBM_GETPOS, 0, 0)));
    Preferences::SetPollIntervalMs(
        static_cast<int>(SendDlgItemMessage(dlg, IDC_POLL_SLIDER, TBM_GETPOS, 0, 0)));
    Preferences::SetInvertColors(IsDlgButtonChecked(dlg, IDC_INVERT) == BST_CHECKED);
    FontPicker::Save();
}

void NotifyApplied(HWND dlg) {
    const HWND owner = GetWindow(dlg, GW_OWNER);
    if (owner) {
        FORWARD_WM_COMMAND(owner, IDC_APPLY, GetDlgItem(dlg, IDC_APPLY),
                           BN_CLICKED, SendMessage);
    }
}

BOOL OnInitDialog(HWND dlg, HWND /*focus*/, LPARAM lParam) {
    g_dlg = dlg;
    const HINSTANCE inst = reinterpret_cast<HINSTANCE>(lParam);

    // Give the dialog the app icon (its owner is the hidden tool window, which has no
    // class icon), shared with the About dialog.
    DialogIcon::Set(dlg, inst);

    // Capture the value-readout templates from the dialog resource before the first update
    // overwrites them, and load the size readout's "Default" text from the string table.
    LoadStr(inst, IDS_DEFAULT, g_defaultLabel);
    VERIFY(GetDlgItemText(dlg, IDC_SIZE_VALUE,    g_sizeFormat,    ARRAYSIZE(g_sizeFormat)));
    VERIFY(GetDlgItemText(dlg, IDC_REFRESH_VALUE, g_refreshFormat, ARRAYSIZE(g_refreshFormat)));
    VERIFY(GetDlgItemText(dlg, IDC_POLL_VALUE,    g_pollFormat,    ARRAYSIZE(g_pollFormat)));

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

    // Timer-interval sliders: fixed ms ranges from Preferences, seeded from the saved
    // values, each with its own "NNN ms" readout.
    const HWND rtb = GetDlgItem(dlg, IDC_REFRESH_SLIDER);
    SendMessage(rtb, TBM_SETRANGE,    TRUE, MAKELPARAM(Preferences::kMinRefreshMs, Preferences::kMaxRefreshMs));
    SendMessage(rtb, TBM_SETTICFREQ,  kRefreshTickFreq, 0);
    SendMessage(rtb, TBM_SETPAGESIZE, 0, kRefreshPageStep);
    SendMessage(rtb, TBM_SETPOS,      TRUE, Preferences::RefreshIntervalMs());
    UpdateMsText(dlg, IDC_REFRESH_SLIDER, IDC_REFRESH_VALUE, g_refreshFormat);

    const HWND ptb = GetDlgItem(dlg, IDC_POLL_SLIDER);
    SendMessage(ptb, TBM_SETRANGE,    TRUE, MAKELPARAM(Preferences::kMinPollMs, Preferences::kMaxPollMs));
    SendMessage(ptb, TBM_SETTICFREQ,  kPollTickFreq, 0);
    SendMessage(ptb, TBM_SETPAGESIZE, 0, kPollPageStep);
    SendMessage(ptb, TBM_SETPOS,      TRUE, Preferences::PollIntervalMs());
    UpdateMsText(dlg, IDC_POLL_SLIDER, IDC_POLL_VALUE, g_pollFormat);

    VERIFY(CheckDlgButton(dlg, IDC_INVERT, Preferences::InvertColors() ? BST_CHECKED : BST_UNCHECKED));

    // Seed and show the font preview from the saved selection (or the fallback when none is
    // set); the font picker owns this state.
    FontPicker::Init(dlg, inst);
    UpdateApplyState(dlg);

    // Focus the trackbar so the label size can be adjusted with the arrow keys as soon as
    // the dialog opens; returning FALSE tells the dialog manager to keep this focus rather
    // than moving it to the default first tab-stop itself.
    SetFocus(tb);
    return FALSE;
}

void OnHScroll(HWND dlg, HWND /*ctl*/, UINT /*code*/, int /*pos*/) {
    UpdateValueText(dlg);
    UpdateMsText(dlg, IDC_REFRESH_SLIDER, IDC_REFRESH_VALUE, g_refreshFormat);
    UpdateMsText(dlg, IDC_POLL_SLIDER, IDC_POLL_VALUE, g_pollFormat);
    UpdateApplyState(dlg);
}

void OnCommand(HWND dlg, int id, HWND /*ctl*/, UINT notify) {
    switch (id) {
        case IDOK:
            SaveValues(dlg);
            VERIFY(EndDialog(dlg, IDOK));
            break;
        case IDC_APPLY: {
            SaveValues(dlg);
            UpdateApplyState(dlg);
            NotifyApplied(dlg);
            break;
        }
        case IDC_FONT_BUTTON:
            FontPicker::Open(dlg);
            UpdateApplyState(dlg);
            break;
        case IDC_INVERT:
            if (notify == BN_CLICKED) UpdateApplyState(dlg);
            break;
        case IDCANCEL:
            VERIFY(EndDialog(dlg, IDCANCEL));
            break;
    }
}

[[nodiscard]] bool OnFontPickerApply(HWND dlg, FontPickerHelper::Result result) {
    const bool chosen = result == FontPickerHelper::Result::Chosen;
    const bool useDefault = result == FontPickerHelper::Result::Default;
    if (!chosen && !useDefault) {
        VERIFY(chosen || useDefault);
        return false;
    }

    LOGFONT selected;
    ZeroMemory(&selected, sizeof(selected));
    if (chosen) {
        const bool read = FontPickerHelper::ReadAppliedFont(selected);
        VERIFY(read);
        if (!read) return false;
    }

    FontPicker::ApplySelection(dlg, chosen ? &selected : nullptr);
    UpdateApplyState(dlg);
    NotifyApplied(dlg);
    return true;
}

// The "Reset to defaults" SysLink was clicked (mouse) or activated (Enter): restore the
// dialog's controls to their factory defaults -- the slim "Default" strip, colors not
// inverted, and the fallback (taskbar) font. Only the controls are reset here; the change
// is persisted only if the user then confirms with OK or Apply, so Cancel still discards
// unapplied changes.
BOOL OnNotify(HWND dlg, int idCtrl, NMHDR* hdr) {
    if (idCtrl == IDC_RESET && (hdr->code == NM_CLICK || hdr->code == NM_RETURN)) {
        SendDlgItemMessage(dlg, IDC_SIZE_SLIDER, TBM_SETPOS, TRUE, g_minPct);
        SendDlgItemMessage(dlg, IDC_REFRESH_SLIDER, TBM_SETPOS, TRUE, Preferences::kDefaultRefreshMs);
        SendDlgItemMessage(dlg, IDC_POLL_SLIDER, TBM_SETPOS, TRUE, Preferences::kDefaultPollMs);
        VERIFY(CheckDlgButton(dlg, IDC_INVERT, BST_UNCHECKED));
        UpdateValueText(dlg);
        UpdateMsText(dlg, IDC_REFRESH_SLIDER, IDC_REFRESH_VALUE, g_refreshFormat);
        UpdateMsText(dlg, IDC_POLL_SLIDER, IDC_POLL_VALUE, g_pollFormat);
        FontPicker::Reset(dlg);
        UpdateApplyState(dlg);

        return TRUE;
    }

    return FALSE;
}

// Release the font-picker resources (preview + sample fonts) as the dialog closes.
void OnDestroy(HWND /*dlg*/) {
    FontPicker::Cleanup();
}

// If shell/menu activation lands on the disabled Preferences dialog while the helper font
// dialog is open, redirect activation back to the helper on its own UI thread.
void OnActivate(HWND /*dlg*/, UINT state, HWND /*other*/, BOOL /*minimized*/) {
    if (state != WA_INACTIVE) (void)FontPickerHelper::ActivateDialog();
}

// WM_SYSCOMMAND: intercept the title-bar "?" (context-help) button that the DS_CONTEXTHELP
// style adds, opening the README's Preferences section (see OpenUrlOnContextHelp).
void OnSysCommand(HWND dlg, UINT cmd, int x, int y) {
    OpenUrlOnContextHelp(dlg, cmd, x, y, TEXT("https://github.com/i2van/WinNumTip/blob/main/README.md#preferences"));
}

INT_PTR CALLBACK PreferencesProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        HANDLE_MSG(dlg, WM_INITDIALOG, OnInitDialog);
        HANDLE_MSG(dlg, WM_HSCROLL,    OnHScroll);
        HANDLE_MSG(dlg, WM_COMMAND,    OnCommand);
        HANDLE_MSG(dlg, WM_NOTIFY,     OnNotify);
        HANDLE_MSG(dlg, WM_ACTIVATE,   OnActivate);
        HANDLE_MSG(dlg, WM_DESTROY,    OnDestroy);
        HANDLE_MSG(dlg, WM_FONT_PICKER_APPLY_SELECTION, OnFontPickerApply);
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

namespace PreferencesDialog {

INT_PTR Show(HINSTANCE inst, HWND owner) {
    // Only one Preferences dialog at a time: if it's already up, bring the helper process's
    // ChooseFont dialog forward when present, otherwise foreground Preferences itself. Report
    // IDCANCEL since no new choice was made.
    if (g_dlg) {
        if (!FontPickerHelper::ActivateDialog()) ForegroundDialog(g_dlg);
        return IDCANCEL;
    }
    // The trackbar common-control class is registered once at startup (InitCommonControlsEx
    // in Entry). 'inst' is forwarded so OnInitDialog can load the app icon.
    const INT_PTR result = DialogBoxParam(inst, MAKEINTRESOURCE(IDD_PREFERENCES), owner, PreferencesProc, reinterpret_cast<LPARAM>(inst));
    VERIFY(result != -1);
    g_dlg = nullptr;
    return result;
}

} // namespace PreferencesDialog
