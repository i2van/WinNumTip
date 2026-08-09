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

#define WM_FONT_PICKER_ACTIVATE_OWNER FontPickerHelper::kActivateOwnerMessage
#define HANDLE_WM_FONT_PICKER_ACTIVATE_OWNER(hwnd, wParam, lParam, fn) ((fn)(hwnd), 0L)

namespace {

// The live modal Preferences dialog, or null when none is open (enforces a single
// instance and lets a repeat request re-focus the existing one).
HWND g_dlg;

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

// Opacity trackbar tuning: a tick mark and a PageUp/PageDown step every N percent.
constexpr int kOpacityTickFreq = 10;
constexpr int kOpacityPageStep = 10;

// The trackbar's minimum percentage: the default strip's share of a full taskbar button.
// Percentages below this render identically to the default, so they are excluded from the
// slider; the minimum position is shown as "Default" rather than a number.
int g_minPct = kMinSliderPct;

// Value-readout templates captured from the dialog's own statics in OnInitDialog (before the
// first update overwrites them) so all display text lives in WinNumTip.rc: the size and
// opacity readouts' "%d%%" and each interval readout's "%d ms". g_defaultText holds
// IDS_DEFAULT ("Default"), shown for the size readout at the slider minimum.
TCHAR g_defaultText[16];
TCHAR g_sizeFormat[16];
TCHAR g_opacityFormat[16];
TCHAR g_refreshFormat[16];
TCHAR g_pollFormat[16];

// Refresh the size readout from the trackbar: "Default" at the minimum, otherwise the "%d%%"
// template applied to the percentage.
void UpdateValueText(HWND dlg) {
    const int pos = WinAPI::TrackBar::GetPos(dlg, IDC_SIZE_SLIDER);
    TCHAR buf[16];
    if (pos <= g_minPct) lstrcpyn(buf, g_defaultText, ARRAYSIZE(buf));
    else                 wsprintf(buf, g_sizeFormat, pos);
    VERIFY(SetDlgItemText(dlg, IDC_SIZE_VALUE, buf));
}

// Refresh a single-value readout (opacity percentage or timer interval) from its trackbar
// using the control's captured "%d%%"/"%d ms" template.
void UpdateReadoutText(HWND dlg, int sliderId, int valueId, LPCTSTR fmt) {
    const int pos = WinAPI::TrackBar::GetPos(dlg, sliderId);
    TCHAR buf[16];
    wsprintf(buf, fmt, pos);
    VERIFY(SetDlgItemText(dlg, valueId, buf));
}

void UpdateApplyState(HWND dlg) {
    int savedSize = Preferences::TipSizePercent();
    if (savedSize < g_minPct) savedSize = g_minPct;
    else if (savedSize > Preferences::kMaxPercent) savedSize = Preferences::kMaxPercent;

    const bool changed =
        WinAPI::TrackBar::GetPos(dlg, IDC_SIZE_SLIDER) != savedSize ||
        WinAPI::TrackBar::GetPos(dlg, IDC_OPACITY_SLIDER) != Preferences::OpacityPercent() ||
        WinAPI::TrackBar::GetPos(dlg, IDC_REFRESH_SLIDER) != Preferences::RefreshIntervalMs() ||
        WinAPI::TrackBar::GetPos(dlg, IDC_POLL_SLIDER) != Preferences::PollIntervalMs() ||
        (IsDlgButtonChecked(dlg, IDC_INVERT) == BST_CHECKED) != Preferences::InvertColors() ||
        FontPicker::HasChanges();
    WinAPI::Window::Enable(GetDlgItem(dlg, IDC_APPLY), changed);
}

void SaveValues(HWND dlg) {
    Preferences::SetTipSizePercent(WinAPI::TrackBar::GetPos(dlg, IDC_SIZE_SLIDER));
    Preferences::SetOpacityPercent(WinAPI::TrackBar::GetPos(dlg, IDC_OPACITY_SLIDER));
    Preferences::SetRefreshIntervalMs(WinAPI::TrackBar::GetPos(dlg, IDC_REFRESH_SLIDER));
    Preferences::SetPollIntervalMs(WinAPI::TrackBar::GetPos(dlg, IDC_POLL_SLIDER));
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
    LoadStr(inst, IDS_DEFAULT, g_defaultText);
    VERIFY(GetDlgItemText(dlg, IDC_SIZE_VALUE,    g_sizeFormat,    ARRAYSIZE(g_sizeFormat)));
    VERIFY(GetDlgItemText(dlg, IDC_OPACITY_VALUE, g_opacityFormat, ARRAYSIZE(g_opacityFormat)));
    VERIFY(GetDlgItemText(dlg, IDC_REFRESH_VALUE, g_refreshFormat, ARRAYSIZE(g_refreshFormat)));
    VERIFY(GetDlgItemText(dlg, IDC_POLL_VALUE,    g_pollFormat,    ARRAYSIZE(g_pollFormat)));

    // Resolve the default's share of a full taskbar button from the current taskbar
    // (shared with the renderer); it becomes the slider's minimum so every selectable
    // value changes the strip. The "Tip size:" caption is fixed and orientation-neutral
    // (the strip is a height for a horizontal taskbar but a width for a side-docked one),
    // so no runtime relabeling is needed.
    int defThick = 0, btnThick = 0;
    bool vertical = false;
    g_minPct = kMinSliderPct;
    if (Overlay::TipSizeBounds(defThick, btnThick, vertical) && btnThick > 0) {
        g_minPct = MulDiv(defThick, Preferences::kMaxPercent, btnThick);
        if (g_minPct < kMinSliderPct) g_minPct = kMinSliderPct;
        else if (g_minPct > Preferences::kMaxPercent) g_minPct = Preferences::kMaxPercent;
    }

    int cur = Preferences::TipSizePercent();
    if (cur < g_minPct) cur = g_minPct; else if (cur > Preferences::kMaxPercent) cur = Preferences::kMaxPercent;

    const HWND tb = GetDlgItem(dlg, IDC_SIZE_SLIDER);
    WinAPI::TrackBar::Init(tb, g_minPct, Preferences::kMaxPercent, kSliderTickFreq, kSliderPageStep, cur);
    UpdateValueText(dlg);

    // Opacity slider: fixed percentage range from Preferences, seeded from the saved value.
    WinAPI::TrackBar::Init(GetDlgItem(dlg, IDC_OPACITY_SLIDER),
                           Preferences::kMinOpacityPercent, Preferences::kMaxOpacityPercent,
                           kOpacityTickFreq, kOpacityPageStep, Preferences::OpacityPercent());
    UpdateReadoutText(dlg, IDC_OPACITY_SLIDER, IDC_OPACITY_VALUE, g_opacityFormat);

    // Timer-interval sliders: fixed ms ranges from Preferences, seeded from the saved
    // values, each with its own "NNN ms" readout.
    WinAPI::TrackBar::Init(GetDlgItem(dlg, IDC_REFRESH_SLIDER),
                           Preferences::kMinRefreshMs, Preferences::kMaxRefreshMs,
                           kRefreshTickFreq, kRefreshPageStep, Preferences::RefreshIntervalMs());
    UpdateReadoutText(dlg, IDC_REFRESH_SLIDER, IDC_REFRESH_VALUE, g_refreshFormat);

    WinAPI::TrackBar::Init(GetDlgItem(dlg, IDC_POLL_SLIDER),
                           Preferences::kMinPollMs, Preferences::kMaxPollMs,
                           kPollTickFreq, kPollPageStep, Preferences::PollIntervalMs());
    UpdateReadoutText(dlg, IDC_POLL_SLIDER, IDC_POLL_VALUE, g_pollFormat);

    VERIFY(CheckDlgButton(dlg, IDC_INVERT, Preferences::InvertColors() ? BST_CHECKED : BST_UNCHECKED));

    // Seed and show the font preview from the saved selection (or the fallback when none is
    // set); the font picker owns this state.
    FontPicker::Init(dlg, inst);
    UpdateApplyState(dlg);

    // Focus the trackbar so the tip size can be adjusted with the arrow keys as soon as
    // the dialog opens. Dialogs opened from mouse input initially hide focus cues, so clear
    // that UI state after initialization to paint the trackbar's focus rectangle. Returning
    // FALSE tells the dialog manager to keep this focus rather than moving it to the default
    // first tab-stop itself.
    SetFocus(tb);
    VERIFY(PostMessage(dlg, WM_UPDATEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0));
    return FALSE;
}

void OnHScroll(HWND dlg, HWND /*ctl*/, UINT /*code*/, int /*pos*/) {
    UpdateValueText(dlg);
    UpdateReadoutText(dlg, IDC_OPACITY_SLIDER, IDC_OPACITY_VALUE, g_opacityFormat);
    UpdateReadoutText(dlg, IDC_REFRESH_SLIDER, IDC_REFRESH_VALUE, g_refreshFormat);
    UpdateReadoutText(dlg, IDC_POLL_SLIDER, IDC_POLL_VALUE, g_pollFormat);
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
        ASSERT(chosen || useDefault);
        return false;
    }

    LOGFONT selected;
    ZeroMemory(&selected, sizeof(selected));
    if (chosen) {
        const bool read = FontPickerHelper::ReadAppliedFont(selected);
        ASSERT(read);
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
        WinAPI::TrackBar::SetPos(dlg, IDC_SIZE_SLIDER, g_minPct);
        WinAPI::TrackBar::SetPos(dlg, IDC_OPACITY_SLIDER, Preferences::kDefaultOpacityPercent);
        WinAPI::TrackBar::SetPos(dlg, IDC_REFRESH_SLIDER, Preferences::kDefaultRefreshMs);
        WinAPI::TrackBar::SetPos(dlg, IDC_POLL_SLIDER, Preferences::kDefaultPollMs);
        VERIFY(CheckDlgButton(dlg, IDC_INVERT, BST_UNCHECKED));
        UpdateValueText(dlg);
        UpdateReadoutText(dlg, IDC_OPACITY_SLIDER, IDC_OPACITY_VALUE, g_opacityFormat);
        UpdateReadoutText(dlg, IDC_REFRESH_SLIDER, IDC_REFRESH_VALUE, g_refreshFormat);
        UpdateReadoutText(dlg, IDC_POLL_SLIDER, IDC_POLL_VALUE, g_pollFormat);
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

// WM_FONT_PICKER_ACTIVATE_OWNER (posted by the helper process's font dialog when the OS
// itself activates it -- see OnChooseFontActivate -- rather than through
// FontPickerHelper::ActivateDialog): foreground Preferences so it isn't left behind other
// apps. Preferences is still disabled while the font dialog is open, so this immediately
// re-triggers OnActivate above, which hands activation straight back to the font dialog --
// net effect, both windows come forward together with the font dialog back on top.
void OnActivateOwner(HWND dlg) {
    ForegroundDialog(dlg);
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
        HANDLE_MSG(dlg, WM_FONT_PICKER_ACTIVATE_OWNER, OnActivateOwner);
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

// Bring Preferences and, when open, the helper process's ChooseFont dialog forward together,
// with the font dialog ending up the active one on top. The two are unrelated top-level
// windows (the font dialog is owned by a hidden window in the helper process, not by
// Preferences), so without this pairing a tray-icon click / repeat open request would surface
// only whichever one FontPickerHelper::ActivateDialog reaches, leaving the other behind other
// apps. Returns false when Preferences is not open.
bool ActivateDialog() {
    if (!g_dlg) return false;

    const HWND fontDialog = FontPickerHelper::ActiveDialog();
    if (!fontDialog) {
        // Nothing to keep in order behind -- just foreground Preferences normally, skipping
        // when it already is (see below for why that matters).
        if (GetForegroundWindow() == g_dlg) return true;
        ForegroundDialog(g_dlg);
        return true;
    }

    // Skip entirely once the font dialog is already foreground: this is called from several
    // overlapping paths (WM_NOTIFYICON's own click handling, Show's repeat-open request, ...),
    // and a tray-icon click/double-click can drive several of them within milliseconds of each
    // other.
    if (GetForegroundWindow() == fontDialog) return true;

    // Just hand activation to the font dialog -- FontPickerHelper::ActivateDialog's own
    // OnActivateChooseFont slots Preferences in right behind it once the helper process
    // genuinely holds the foreground (see there for why it has to happen on that side, not
    // here): done from this (main) process instead, before it holds any foreground rights of
    // its own, the same z-order move silently fails to rise above whatever app was truly
    // foreground, while activating Preferences here first to get around that would flash it as
    // briefly active in its own right before the font dialog retakes it.
    (void)FontPickerHelper::ActivateDialog();
    return true;
}

INT_PTR Show(HINSTANCE inst, HWND owner) {
    // Only one Preferences dialog at a time: if it's already up, bring both it and, when
    // present, the helper process's ChooseFont dialog to the foreground. Report IDCANCEL
    // since no new choice was made.
    if (g_dlg) {
        (void)ActivateDialog();
        return IDCANCEL;
    }
    // The trackbar common-control class is registered once at startup (InitCommonControlsEx
    // in Entry). 'inst' is forwarded so OnInitDialog can load the app icon.
    const INT_PTR result = DialogBoxParam(inst, MAKEINTRESOURCE(IDD_PREFERENCES), owner, PreferencesProc, reinterpret_cast<LPARAM>(inst));
    ASSERT(result != -1);
    g_dlg = nullptr;
    return result;
}

} // namespace PreferencesDialog
