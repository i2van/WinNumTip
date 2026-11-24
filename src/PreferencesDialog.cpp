#include "stdafx.h"
#include "PreferencesDialog.h"
#include "DialogIcon.h"
#include "FontPicker.h"
#include "FontPickerHelper.h"
#include "Overlay.h"
#include "Preferences.h"
#include "Registry.h"
#include "Startup.h"
#include "resource.h"

#define WM_FONT_PICKER_APPLY_SELECTION FontPickerHelper::kApplySelectionMessage
#define HANDLE_WM_FONT_PICKER_APPLY_SELECTION(hwnd, wParam, lParam, fn) \
    ((fn)((hwnd), static_cast<FontPickerHelper::Result>(static_cast<LONG>(wParam))) \
         ? TRUE \
         : FALSE)
#define WM_FONT_PICKER_CLOSED FontPickerHelper::kClosedMessage
#define HANDLE_WM_FONT_PICKER_CLOSED(hwnd, wParam, lParam, fn) ((fn)((hwnd), (wParam)), TRUE)
#define WM_FONT_PICKER_SHOWN FontPickerHelper::kShownMessage
#define HANDLE_WM_FONT_PICKER_SHOWN(hwnd, wParam, lParam, fn) ((fn)((hwnd), (wParam)), TRUE)

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

// Animation-duration drop-down: one item every N milliseconds from kMinAnimationMs to
// kMaxAnimationMs (see InitAnimationDurationCombo).
constexpr int kAnimationStepMs = 50;
static_assert(Preferences::kMinAnimationMs % kAnimationStepMs == 0 &&
              Preferences::kMaxAnimationMs % kAnimationStepMs == 0);

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
    if (pos <= g_minPct) WinAPI::String::Copy(buf, g_defaultText);
    else                 WinAPI::String::Format(buf, g_sizeFormat, pos);
    VERIFY(SetDlgItemText(dlg, IDC_SIZE_VALUE, buf));
}

// Refresh a single-value readout (opacity percentage or timer interval) from its trackbar
// using the control's captured "%d%%"/"%d ms" template.
void UpdateReadoutText(HWND dlg, int sliderId, int valueId, LPCTSTR fmt) {
    const int pos = WinAPI::TrackBar::GetPos(dlg, sliderId);
    TCHAR buf[16];
    WinAPI::String::Format(buf, fmt, pos);
    VERIFY(SetDlgItemText(dlg, valueId, buf));
}

// Read the four render-flag checkboxes into a Preferences::RenderFlags, for comparing
// against (UpdateApplyState) or saving (SaveValues) the persisted flags as one unit.
// hideSeparator reads as forced true whenever compact is checked, matching
// ApplyStripRegion/PaintBorder (compact already draws a border framing each tip
// individually, which doubles as the divider between them, so a separator on top would
// be redundant) and the dialog, which hides its checkbox in that state (see
// UpdateCompactVisibility) rather than leave it showing a value that no longer does
// anything. hideBorder is read as-is: compact still lets it suppress that per-tip
// border, so its checkbox stays visible and independent.
[[nodiscard]] Preferences::RenderFlags ReadFlags(HWND dlg) {
    const bool compact = IsDlgButtonChecked(dlg, IDC_COMPACT_VIEW) == BST_CHECKED;
    return {
        .darkTheme     = IsDlgButtonChecked(dlg, IDC_DARK_THEME) == BST_CHECKED,
        .compact       = compact,
        .hideBorder    = IsDlgButtonChecked(dlg, IDC_HIDE_BORDER) == BST_CHECKED,
        .hideSeparator = compact || IsDlgButtonChecked(dlg, IDC_HIDE_SEPARATOR) == BST_CHECKED,
    };
}

// Read the Start with Windows checkbox: the autostart preference. Not part of RenderFlags,
// which bundles only the overlay's look toggles.
[[nodiscard]] bool ReadStartWithWindows(HWND dlg) {
    return IsDlgButtonChecked(dlg, IDC_START_WITH_WINDOWS) == BST_CHECKED;
}

// Fill the Tip animation drop-down in Preferences::Animation order, so an item's index is
// its value and its IDS_ANIMATION_* string id is IDS_ANIMATION_SYSTEM + that value. The
// System item names the effect it currently resolves to, e.g. "Windows default (Fade)".
void InitAnimationCombo(HWND dlg, HINSTANCE inst) {
    const HWND combo = GetDlgItem(dlg, IDC_ANIMATION);
    TCHAR name[16];
    TCHAR format[32];
    TCHAR item[48];
    WinAPI::String::Load(inst, IDS_ANIMATION_SYSTEM + static_cast<int>(Preferences::SystemAnimation()), name);
    WinAPI::String::Load(inst, IDS_ANIMATION_SYSTEM, format);
    VERIFY(ComboBox_AddString(combo, WinAPI::String::Format(item, format, name)) >= 0);
    for (int i = static_cast<int>(Preferences::kMinAnimation) + 1; i <= static_cast<int>(Preferences::kMaxAnimation); ++i)
        VERIFY(ComboBox_AddString(combo, WinAPI::String::Load(inst, IDS_ANIMATION_SYSTEM + i, name)) >= 0);
    VERIFY(ComboBox_SetCurSel(combo, static_cast<int>(Preferences::ShowAnimation())) >= 0);
}

// Read the Tip animation drop-down (see InitAnimationCombo).
[[nodiscard]] Preferences::Animation ReadAnimation(HWND dlg) {
    const int sel = ComboBox_GetCurSel(GetDlgItem(dlg, IDC_ANIMATION));
    return sel < 0 ? Preferences::Animation::System : static_cast<Preferences::Animation>(sel);
}

// Enable the animation-duration drop-down (and its labels) only while the Tip animation
// choice (System resolved from the Windows setting) actually animates; None has no duration.
void UpdateAnimationDurationState(HWND dlg) {
    Preferences::Animation animation = ReadAnimation(dlg);
    if (animation == Preferences::Animation::System) animation = Preferences::SystemAnimation();
    const BOOL enable = animation != Preferences::Animation::None;
    EnableWindow(GetDlgItem(dlg, IDC_ANIMATION_DURATION_LABEL), enable);
    EnableWindow(GetDlgItem(dlg, IDC_ANIMATION_DURATION), enable);
    EnableWindow(GetDlgItem(dlg, IDC_ANIMATION_DURATION_UNIT), enable);
}

// Insert an animation-duration item for 'ms' at 'index' (-1 appends), keeping its value in the
// item data. Returns the item's index, or a negative value on failure.
int InsertAnimationDuration(HWND combo, int index, int ms) {
    TCHAR item[16];
    const int inserted = ComboBox_InsertString(combo, index, WinAPI::String::Format(item, TEXT("%d"), ms));
    ASSERT(inserted >= 0);
    if (inserted >= 0) VERIFY(ComboBox_SetItemData(combo, inserted, ms) != CB_ERR);
    return inserted;
}

// Select the animation-duration item for 'ms'. A value that is not one of the steps (a
// hand-edited INI value) gets its own item, inserted in order, so an untouched duration is
// saved back unchanged rather than silently rounded.
void SelectAnimationDuration(HWND dlg, int ms) {
    const HWND combo = GetDlgItem(dlg, IDC_ANIMATION_DURATION);
    const int count = ComboBox_GetCount(combo);
    int index = 0;
    while (index < count && static_cast<int>(ComboBox_GetItemData(combo, index)) < ms) ++index;
    if (index == count || static_cast<int>(ComboBox_GetItemData(combo, index)) != ms)
        index = InsertAnimationDuration(combo, index == count ? -1 : index, ms);
    if (index >= 0) VERIFY(ComboBox_SetCurSel(combo, index) >= 0);
}

// Fill the animation-duration drop-down with kMinAnimationMs..kMaxAnimationMs in
// kAnimationStepMs steps (the "ms" unit is a label after it) and select the current value.
void InitAnimationDurationCombo(HWND dlg) {
    const HWND combo = GetDlgItem(dlg, IDC_ANIMATION_DURATION);
    for (int ms = Preferences::kMinAnimationMs; ms <= Preferences::kMaxAnimationMs; ms += kAnimationStepMs)
        (void)InsertAnimationDuration(combo, -1, ms);
    SelectAnimationDuration(dlg, Preferences::AnimationMs());
}

// Read the animation-duration drop-down (see InitAnimationDurationCombo).
[[nodiscard]] int ReadAnimationMs(HWND dlg) {
    const HWND combo = GetDlgItem(dlg, IDC_ANIMATION_DURATION);
    const int sel = ComboBox_GetCurSel(combo);
    return sel < 0 ? Preferences::AnimationMs() : static_cast<int>(ComboBox_GetItemData(combo, sel));
}

// Show or hide the Hide separator checkbox based on whether Compact view is currently
// checked: compact forces it on anyway (see ReadFlags), so it would offer no real choice
// while checked. Hide border stays visible regardless -- compact still honors it, just
// against each tip's own border instead of the whole strip's. The separator checkbox's
// own checked state is left untouched underneath, so unchecking Compact view again brings
// back whatever it was set to before.
void UpdateCompactVisibility(HWND dlg) {
    const bool compact = IsDlgButtonChecked(dlg, IDC_COMPACT_VIEW) == BST_CHECKED;
    ShowWindow(GetDlgItem(dlg, IDC_HIDE_SEPARATOR), compact ? SW_HIDE : SW_SHOW);
}

// The saved tip size as a size-slider position: clamped up to the slider's minimum (see
// g_minPct).
[[nodiscard]] int SavedSizeSliderPos() {
    return max(g_minPct, Preferences::TipSizePercent());
}

void UpdateApplyState(HWND dlg) {
    const bool changed =
        WinAPI::TrackBar::GetPos(dlg, IDC_SIZE_SLIDER) != SavedSizeSliderPos() ||
        WinAPI::TrackBar::GetPos(dlg, IDC_OPACITY_SLIDER) != Preferences::OpacityPercent() ||
        WinAPI::TrackBar::GetPos(dlg, IDC_REFRESH_SLIDER) != Preferences::RefreshIntervalMs() ||
        WinAPI::TrackBar::GetPos(dlg, IDC_POLL_SLIDER) != Preferences::PollIntervalMs() ||
        ReadFlags(dlg) != Preferences::Flags() ||
        ReadAnimation(dlg) != Preferences::ShowAnimation() ||
        ReadAnimationMs(dlg) != Preferences::AnimationMs() ||
        ReadStartWithWindows(dlg) != Preferences::StartWithWindows() ||
        FontPicker::HasChanges();
    WinAPI::Window::Enable(GetDlgItem(dlg, IDC_APPLY), changed);
}

void SaveValues(HWND dlg) {
    Preferences::SetTipSizePercent(WinAPI::TrackBar::GetPos(dlg, IDC_SIZE_SLIDER));
    Preferences::SetOpacityPercent(WinAPI::TrackBar::GetPos(dlg, IDC_OPACITY_SLIDER));
    Preferences::SetRefreshIntervalMs(WinAPI::TrackBar::GetPos(dlg, IDC_REFRESH_SLIDER));
    Preferences::SetPollIntervalMs(WinAPI::TrackBar::GetPos(dlg, IDC_POLL_SLIDER));
    Preferences::SetFlags(ReadFlags(dlg));
    Preferences::SetShowAnimation(ReadAnimation(dlg));
    Preferences::SetAnimationMs(ReadAnimationMs(dlg));
    // Persist the autostart flag, then reconcile the registry entry with it right away:
    // create/correct the entry when enabled, remove it when disabled.
    const bool startWithWindows = ReadStartWithWindows(dlg);
    Preferences::SetStartWithWindows(startWithWindows);
    Startup::Sync(startWithWindows);
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

    // Add a title-bar system-menu command that opens Explorer at the persisted config file.
    const HMENU systemMenu = GetSystemMenu(dlg, FALSE);
    ASSERT(systemMenu);
    if (systemMenu) {
        TCHAR text[48];
        WinAPI::String::Load(inst, IDS_CONFIG_LOCATION, text);
        VERIFY(AppendMenu(systemMenu, MF_SEPARATOR, 0, nullptr));
        VERIFY(AppendMenu(systemMenu, MF_STRING, IDM_CONFIG_LOCATION, text));
    }

    // Capture the value-readout templates from the dialog resource before the first update
    // overwrites them, and load the size readout's "Default" text from the string table.
    WinAPI::String::Load(inst, IDS_DEFAULT, g_defaultText);
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
        g_minPct = max(kMinSliderPct, min(g_minPct, Preferences::kMaxPercent));
    }

    const HWND tb = GetDlgItem(dlg, IDC_SIZE_SLIDER);
    WinAPI::TrackBar::Init(tb, g_minPct, Preferences::kMaxPercent, kSliderTickFreq, kSliderPageStep,
                           SavedSizeSliderPos());
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

    VERIFY(CheckDlgButton(dlg, IDC_DARK_THEME, Preferences::Flags().darkTheme ? BST_CHECKED : BST_UNCHECKED));
    VERIFY(CheckDlgButton(dlg, IDC_COMPACT_VIEW, Preferences::Flags().compact ? BST_CHECKED : BST_UNCHECKED));
    VERIFY(CheckDlgButton(dlg, IDC_HIDE_BORDER, Preferences::Flags().hideBorder ? BST_CHECKED : BST_UNCHECKED));
    VERIFY(CheckDlgButton(dlg, IDC_HIDE_SEPARATOR, Preferences::Flags().hideSeparator ? BST_CHECKED : BST_UNCHECKED));
    VERIFY(CheckDlgButton(dlg, IDC_START_WITH_WINDOWS, Preferences::StartWithWindows() ? BST_CHECKED : BST_UNCHECKED));
    UpdateCompactVisibility(dlg);
    InitAnimationCombo(dlg, inst);
    InitAnimationDurationCombo(dlg);
    UpdateAnimationDurationState(dlg);

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
            // The font dialog closes with Preferences, discarding its unapplied selection.
            FontPicker::Close();
            VERIFY(EndDialog(dlg, IDOK));
            break;
        case IDC_APPLY:
            SaveValues(dlg);
            UpdateApplyState(dlg);
            NotifyApplied(dlg);
            break;
        // The font preview is clickable too (FontPreview shows a hand cursor over it as the
        // hint): a click (STN_CLICKED, from the SS_NOTIFY style) opens the same font picker as
        // the Select... button beside it.
        case IDC_FONT_NAME:
            if (notify != STN_CLICKED) break;
            [[fallthrough]];
        case IDC_FONT_BUTTON:
            FontPicker::Open(dlg);
            break;
        case IDC_COMPACT_VIEW:
            if (notify == BN_CLICKED) UpdateCompactVisibility(dlg);
            [[fallthrough]];
        case IDC_DARK_THEME:
        case IDC_HIDE_BORDER:
        case IDC_HIDE_SEPARATOR:
        case IDC_START_WITH_WINDOWS:
            if (notify == BN_CLICKED) UpdateApplyState(dlg);
            break;
        case IDC_ANIMATION:
            if (notify != CBN_SELCHANGE) break;
            UpdateAnimationDurationState(dlg);
            UpdateApplyState(dlg);
            break;
        case IDC_ANIMATION_DURATION:
            if (notify == CBN_SELCHANGE) UpdateApplyState(dlg);
            break;
        case IDCANCEL:
            FontPicker::Close();
            VERIFY(EndDialog(dlg, IDCANCEL));
            break;
    }
}

// The font dialog closed: adopt its OK selection, if any (see FontPicker::OnClosed).
void OnFontPickerClosed(HWND dlg, WPARAM session) {
    FontPicker::OnClosed(dlg, session);
    UpdateApplyState(dlg);
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
// dialog's controls to their factory defaults -- the default tip size, no render flags set
// (dark theme following the Windows mode, border/separator shown, not compact), the Windows
// default animation at the default duration, start with Windows off, and the fallback
// (taskbar) font. Only the controls are reset here; the change is persisted only if the user
// then confirms with OK or Apply, so Cancel still discards unapplied changes.
BOOL OnNotify(HWND dlg, int idCtrl, NMHDR* hdr) {
    if (idCtrl == IDC_RESET && (hdr->code == NM_CLICK || hdr->code == NM_RETURN)) {
        WinAPI::TrackBar::SetPos(dlg, IDC_SIZE_SLIDER, max(g_minPct, Preferences::kDefaultTipSizePercent));
        WinAPI::TrackBar::SetPos(dlg, IDC_OPACITY_SLIDER, Preferences::kDefaultOpacityPercent);
        WinAPI::TrackBar::SetPos(dlg, IDC_REFRESH_SLIDER, Preferences::kDefaultRefreshMs);
        WinAPI::TrackBar::SetPos(dlg, IDC_POLL_SLIDER, Preferences::kDefaultPollMs);
        VERIFY(CheckDlgButton(dlg, IDC_DARK_THEME, Registry::SystemUsesDarkTheme() ? BST_CHECKED : BST_UNCHECKED));
        VERIFY(CheckDlgButton(dlg, IDC_COMPACT_VIEW, BST_UNCHECKED));
        VERIFY(CheckDlgButton(dlg, IDC_HIDE_BORDER, BST_UNCHECKED));
        VERIFY(CheckDlgButton(dlg, IDC_HIDE_SEPARATOR, BST_UNCHECKED));
        VERIFY(CheckDlgButton(dlg, IDC_START_WITH_WINDOWS, BST_UNCHECKED));
        VERIFY(ComboBox_SetCurSel(GetDlgItem(dlg, IDC_ANIMATION), static_cast<int>(Preferences::Animation::System)) >= 0);
        SelectAnimationDuration(dlg, Preferences::kDefaultAnimationMs);
        UpdateAnimationDurationState(dlg);
        UpdateCompactVisibility(dlg);
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

// Release the font-picker resources (preview + sample fonts, and the font dialog should it
// still be open) as the dialog closes.
void OnDestroy(HWND /*dlg*/) {
    FontPicker::Cleanup();
}

// Open Explorer with the INI file selected when it exists, otherwise open the folder it
// will be created in (the executable's).
void NavigateToConfigFile(HWND dlg) {
    const LPCTSTR configPath = Preferences::ConfigFilePath();

    if (Preferences::ConfigFileExists()) {
        WinAPI::Explorer::Select(dlg, configPath);
        return;
    }

    TCHAR folder[MAX_PATH];
    WinAPI::String::Copy(folder, configPath);
    // A bare file name (no directory) means the INI lives in the current directory.
    *PathFindFileName(folder) = TEXT('\0');
    if (!folder[0]) WinAPI::String::Copy(folder, TEXT("."));
    WinAPI::Explorer::Open(dlg, folder);
}

// WM_SYSCOMMAND: intercept the title-bar "?" (context-help) button that the DS_CONTEXTHELP
// style adds, opening the README's Preferences section (see WinAPI::Url::OpenOnContextHelp).
// The custom IDM_CONFIG_LOCATION system-menu command navigates to the INI file instead; its
// id keeps the low four bits clear, as those are reserved for the system (hence the mask).
void OnSysCommand(HWND dlg, UINT cmd, int x, int y) {
    if ((cmd & 0xFFF0) == IDM_CONFIG_LOCATION)
        NavigateToConfigFile(dlg);
    else
        WinAPI::Url::OpenOnContextHelp(dlg, cmd, x, y, TEXT("https://github.com/i2van/WinNumTip/blob/main/README.md#preferences"));
}

INT_PTR CALLBACK PreferencesProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        HANDLE_MSG(dlg, WM_INITDIALOG, OnInitDialog);
        HANDLE_MSG(dlg, WM_HSCROLL,    OnHScroll);
        HANDLE_MSG(dlg, WM_COMMAND,    OnCommand);
        HANDLE_MSG(dlg, WM_NOTIFY,     OnNotify);
        HANDLE_MSG(dlg, WM_DESTROY,    OnDestroy);
        HANDLE_MSG(dlg, WM_FONT_PICKER_APPLY_SELECTION, OnFontPickerApply);
        HANDLE_MSG(dlg, WM_FONT_PICKER_CLOSED, OnFontPickerClosed);
        HANDLE_MSG(dlg, WM_FONT_PICKER_SHOWN, FontPicker::OnShown);
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
// with the font dialog ending up the active one on top: Preferences is foregrounded first, and
// only then is the font dialog activated. Returns false when Preferences is not open.
bool ActivateDialog() {
    if (!g_dlg) return false;

    const HWND fontDialog = FontPickerHelper::ActiveDialog();
    if (!fontDialog) {
        // Nothing to keep in order behind -- just foreground Preferences normally, skipping
        // when it already is (see below for why that matters).
        if (GetForegroundWindow() == g_dlg) return true;
        WinAPI::Dialog::Foreground(g_dlg);
        return true;
    }

    // Skip entirely once the font dialog is already foreground: a tray-icon double-click
    // delivers two button-ups within milliseconds of each other.
    if (GetForegroundWindow() == fontDialog) return true;

    SetForegroundWindow(g_dlg);
    (void)FontPickerHelper::ActivateDialog();
    return true;
}

INT_PTR Show(HINSTANCE inst, HWND owner) {
    // Only one Preferences dialog at a time: if it's already up, make it the active window
    // -- even over an open ChooseFont dialog, which is not owned by Preferences and so is not
    // its last active popup -- unless a popup of its own (e.g. a message box) is up over it.
    // Report IDCANCEL since no new choice was made.
    if (g_dlg) {
        const HWND target = GetLastActivePopup(g_dlg);
        if (GetForegroundWindow() != target) WinAPI::Dialog::Foreground(g_dlg, target);
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
