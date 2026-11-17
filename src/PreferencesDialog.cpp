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

// Animation-duration trackbar tuning: every arrow key, PageUp/PageDown and drag moves the thumb
// in steps of N milliseconds (see SnapAnimationSlider).
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

// The animation-duration trackbar's tooltips (its "Duration:" label shows no value): g_hoverTip shows its value while the
// mouse rests over it, g_dragTip tracks the thumb while it is dragged. Both are owned by the
// dialog (destroyed with it) and show IDS_ANIMATION_DURATION, loaded into g_durationFormat.
HWND  g_hoverTip;
HWND  g_dragTip;
TCHAR g_durationFormat[48];

// Whether g_dragTip is showing (a drag is in progress), and its screen y fixed at drag start.
bool g_dragTipActive;
int  g_dragTipY;
// Whether g_hoverTip has been shown (TTN_SHOW), so its window rect holds a real position.
bool g_hoverTipShown;

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

// Enable the animation-duration trackbar (and its label) only while the drop-down's choice
// (System resolved from the Windows setting) actually animates; None has no duration.
void UpdateAnimationSliderState(HWND dlg) {
    Preferences::Animation animation = ReadAnimation(dlg);
    if (animation == Preferences::Animation::System) animation = Preferences::SystemAnimation();
    const BOOL enable = animation != Preferences::Animation::None;
    EnableWindow(GetDlgItem(dlg, IDC_ANIMATION_DURATION_LABEL), enable);
    EnableWindow(GetDlgItem(dlg, IDC_ANIMATION_SLIDER), enable);
}

// A TTTOOLINFO naming the animation-duration trackbar as the tool (by window handle).
[[nodiscard]] TTTOOLINFO AnimationToolInfo(HWND dlg, UINT flags, LPTSTR text) {
    // Zeroed with ZeroMemory rather than brace-initialized, which would emit a memset call
    // this CRT-less build does not provide.
    TTTOOLINFO ti;
    ZeroMemory(&ti, sizeof(ti));
    ti.cbSize   = sizeof(ti);
    ti.uFlags   = TTF_IDISHWND | flags;
    ti.hwnd     = dlg;
    ti.uId      = reinterpret_cast<UINT_PTR>(GetDlgItem(dlg, IDC_ANIMATION_SLIDER));
    ti.lpszText = text;
    return ti;
}

// Create a dialog-owned tooltip window with the animation-duration trackbar as its one tool.
[[nodiscard]] HWND CreateAnimationTip(HWND dlg, HINSTANCE inst, UINT flags) {
    const HWND tip = CreateWindowEx(WS_EX_TOPMOST, TOOLTIPS_CLASS, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                    CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                    dlg, nullptr, inst, nullptr);
    ASSERT(tip);
    if (!tip) return nullptr;

    TCHAR empty[1];
    empty[0] = TEXT('\0');
    TTTOOLINFO ti = AnimationToolInfo(dlg, flags, empty);
    VERIFY(SendMessage(tip, TTM_ADDTOOL, 0, reinterpret_cast<LPARAM>(&ti)));
    return tip;
}

// Create the hover tooltip (subclassing the trackbar to see the mouse) and the tracking one
// shown while dragging (see TrackAnimationTip).
void InitAnimationTips(HWND dlg, HINSTANCE inst) {
    WinAPI::String::Load(inst, IDS_ANIMATION_DURATION, g_durationFormat);
    g_hoverTip = CreateAnimationTip(dlg, inst, TTF_SUBCLASS);
    g_dragTip  = CreateAnimationTip(dlg, inst, TTF_TRACK | TTF_ABSOLUTE);
}

// Refresh both tooltips' text from the animation-duration trackbar.
void UpdateAnimationTips(HWND dlg) {
    TCHAR text[64];
    WinAPI::String::Format(text, g_durationFormat, WinAPI::TrackBar::GetPos(dlg, IDC_ANIMATION_SLIDER));
    const HWND tips[] = { g_hoverTip, g_dragTip };
    for (const HWND tip : tips) {
        if (!tip) continue;
        TTTOOLINFO ti = AnimationToolInfo(dlg, 0, text);
        SendMessage(tip, TTM_UPDATETIPTEXT, 0, reinterpret_cast<LPARAM>(&ti));
    }
}

// Show the tracking tooltip centered on the trackbar's thumb while it is dragged (hiding the
// hover one), or hide it once the drag ends. Only x follows the thumb: y is fixed when the drag
// starts (at the hover tip's, else above the trackbar), so the tip slides without bobbing.
void TrackAnimationTip(HWND dlg, bool dragging) {
    if (!g_dragTip) return;

    TTTOOLINFO ti = AnimationToolInfo(dlg, 0, nullptr);
    if (!dragging) {
        SendMessage(g_dragTip, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&ti));
        g_dragTipActive = false;
        return;
    }

    if (g_hoverTip) SendMessage(g_hoverTip, TTM_POP, 0, 0);

    const HWND slider = GetDlgItem(dlg, IDC_ANIMATION_SLIDER);
    RECT thumb;
    SendMessage(slider, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
    VERIFY(ClientToScreen(slider, reinterpret_cast<POINT*>(&thumb.left)));
    VERIFY(ClientToScreen(slider, reinterpret_cast<POINT*>(&thumb.right)));
    const int center = (thumb.left + thumb.right) / 2;
    if (!g_dragTipActive) {
        // Keep the y the hover tip was last shown at, so the tip does not jump vertically as
        // the drag starts (a hidden window keeps its position). If it never showed (a quick
        // drag), show the drag tip first so its real window size (text, margins, frame) can
        // be measured, then fix y so the whole tip sits above the trackbar, clear of the thumb.
        RECT tip;
        if (g_hoverTipShown && g_hoverTip) {
            VERIFY(GetWindowRect(g_hoverTip, &tip));
            g_dragTipY = tip.top;
            SendMessage(g_dragTip, TTM_TRACKPOSITION, 0, MAKELPARAM(center, g_dragTipY));
            SendMessage(g_dragTip, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&ti));
        } else {
            SendMessage(g_dragTip, TTM_TRACKPOSITION, 0, MAKELPARAM(center, thumb.top));
            SendMessage(g_dragTip, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&ti));
            RECT bar;
            VERIFY(GetWindowRect(g_dragTip, &tip));
            VERIFY(GetWindowRect(slider, &bar));
            g_dragTipY = bar.top - (tip.bottom - tip.top);
        }
        g_dragTipActive = true;
    }

    RECT tip;
    VERIFY(GetWindowRect(g_dragTip, &tip));
    SendMessage(g_dragTip, TTM_TRACKPOSITION, 0, MAKELPARAM(center - (tip.right - tip.left) / 2, g_dragTipY));
}

// Round the animation-duration trackbar to the nearest kAnimationStepMs: the arrow keys and
// PageUp/PageDown already step by it, but a drag or a click lands on any millisecond.
void SnapAnimationSlider(HWND dlg) {
    const int pos = WinAPI::TrackBar::GetPos(dlg, IDC_ANIMATION_SLIDER);
    const int snapped = (pos + kAnimationStepMs / 2) / kAnimationStepMs * kAnimationStepMs;
    if (snapped != pos) WinAPI::TrackBar::SetPos(dlg, IDC_ANIMATION_SLIDER, snapped);
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

void UpdateApplyState(HWND dlg) {
    const int tipPercent = Preferences::TipSizePercent();
    const int savedSize = max(g_minPct, min(tipPercent, Preferences::kMaxPercent));

    const bool changed =
        WinAPI::TrackBar::GetPos(dlg, IDC_SIZE_SLIDER) != savedSize ||
        WinAPI::TrackBar::GetPos(dlg, IDC_OPACITY_SLIDER) != Preferences::OpacityPercent() ||
        WinAPI::TrackBar::GetPos(dlg, IDC_REFRESH_SLIDER) != Preferences::RefreshIntervalMs() ||
        WinAPI::TrackBar::GetPos(dlg, IDC_POLL_SLIDER) != Preferences::PollIntervalMs() ||
        ReadFlags(dlg) != Preferences::Flags() ||
        ReadAnimation(dlg) != Preferences::ShowAnimation() ||
        WinAPI::TrackBar::GetPos(dlg, IDC_ANIMATION_SLIDER) != Preferences::AnimationMs() ||
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
    Preferences::SetAnimationMs(WinAPI::TrackBar::GetPos(dlg, IDC_ANIMATION_SLIDER));
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

    const int tipPercent = Preferences::TipSizePercent();
    const int cur = max(g_minPct, min(tipPercent, Preferences::kMaxPercent));

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

    VERIFY(CheckDlgButton(dlg, IDC_DARK_THEME, Preferences::Flags().darkTheme ? BST_CHECKED : BST_UNCHECKED));
    VERIFY(CheckDlgButton(dlg, IDC_COMPACT_VIEW, Preferences::Flags().compact ? BST_CHECKED : BST_UNCHECKED));
    VERIFY(CheckDlgButton(dlg, IDC_HIDE_BORDER, Preferences::Flags().hideBorder ? BST_CHECKED : BST_UNCHECKED));
    VERIFY(CheckDlgButton(dlg, IDC_HIDE_SEPARATOR, Preferences::Flags().hideSeparator ? BST_CHECKED : BST_UNCHECKED));
    VERIFY(CheckDlgButton(dlg, IDC_START_WITH_WINDOWS, Preferences::StartWithWindows() ? BST_CHECKED : BST_UNCHECKED));
    UpdateCompactVisibility(dlg);
    InitAnimationCombo(dlg, inst);

    // Animation-duration slider: its value has no readout, so it is shown in tooltips.
    const HWND animationSlider = GetDlgItem(dlg, IDC_ANIMATION_SLIDER);
    WinAPI::TrackBar::Init(animationSlider, Preferences::kMinAnimationMs, Preferences::kMaxAnimationMs,
                           kAnimationStepMs, kAnimationStepMs, Preferences::AnimationMs());
    SendMessage(animationSlider, TBM_SETLINESIZE, 0, kAnimationStepMs);
    InitAnimationTips(dlg, inst);
    UpdateAnimationTips(dlg);
    UpdateAnimationSliderState(dlg);

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

void OnHScroll(HWND dlg, HWND ctl, UINT code, int /*pos*/) {
    if (ctl == GetDlgItem(dlg, IDC_ANIMATION_SLIDER)) {
        SnapAnimationSlider(dlg);
        UpdateAnimationTips(dlg);
        if (code == TB_THUMBTRACK || code == TB_ENDTRACK) TrackAnimationTip(dlg, code == TB_THUMBTRACK);
    }
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
        // The font preview is clickable too (FontPreview shows a hand cursor over it as the
        // hint): a click (STN_CLICKED, from the SS_NOTIFY style) opens the same font picker as
        // the Select... button beside it.
        case IDC_FONT_NAME:
            if (notify != STN_CLICKED) break;
            [[fallthrough]];
        case IDC_FONT_BUTTON:
            FontPicker::Open(dlg);
            UpdateApplyState(dlg);
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
            UpdateAnimationSliderState(dlg);
            UpdateApplyState(dlg);
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
// dialog's controls to their factory defaults -- the slim "Default" strip, no render
// flags set (dark theme following the Windows mode, border/separator shown, not compact), the
// Windows default animation at the default duration, start with Windows off, and the fallback (taskbar) font. Only the
// controls are reset here; the change is persisted only
// if the user then confirms with OK or Apply, so Cancel still discards unapplied changes.
BOOL OnNotify(HWND dlg, int idCtrl, NMHDR* hdr) {
    if (hdr->code == TTN_SHOW && hdr->hwndFrom == g_hoverTip) {
        g_hoverTipShown = true;
        return FALSE;
    }

    if (idCtrl == IDC_RESET && (hdr->code == NM_CLICK || hdr->code == NM_RETURN)) {
        WinAPI::TrackBar::SetPos(dlg, IDC_SIZE_SLIDER, g_minPct);
        WinAPI::TrackBar::SetPos(dlg, IDC_OPACITY_SLIDER, Preferences::kDefaultOpacityPercent);
        WinAPI::TrackBar::SetPos(dlg, IDC_REFRESH_SLIDER, Preferences::kDefaultRefreshMs);
        WinAPI::TrackBar::SetPos(dlg, IDC_POLL_SLIDER, Preferences::kDefaultPollMs);
        VERIFY(CheckDlgButton(dlg, IDC_DARK_THEME, Registry::SystemUsesDarkTheme() ? BST_CHECKED : BST_UNCHECKED));
        VERIFY(CheckDlgButton(dlg, IDC_COMPACT_VIEW, BST_UNCHECKED));
        VERIFY(CheckDlgButton(dlg, IDC_HIDE_BORDER, BST_UNCHECKED));
        VERIFY(CheckDlgButton(dlg, IDC_HIDE_SEPARATOR, BST_UNCHECKED));
        VERIFY(CheckDlgButton(dlg, IDC_START_WITH_WINDOWS, BST_UNCHECKED));
        VERIFY(ComboBox_SetCurSel(GetDlgItem(dlg, IDC_ANIMATION), static_cast<int>(Preferences::Animation::System)) >= 0);
        WinAPI::TrackBar::SetPos(dlg, IDC_ANIMATION_SLIDER, Preferences::kDefaultAnimationMs);
        UpdateAnimationSliderState(dlg);
        UpdateAnimationTips(dlg);
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

// Release the font-picker resources (preview + sample fonts) as the dialog closes.
void OnDestroy(HWND /*dlg*/) {
    // The tooltips are owned by the dialog, so Windows destroys them along with it.
    g_hoverTip = nullptr;
    g_dragTip  = nullptr;
    g_dragTipActive = false;
    g_hoverTipShown = false;
    FontPicker::Cleanup();
}

// If shell/menu activation lands on the disabled Preferences dialog while the helper font
// dialog is open, redirect activation back to the helper on its own UI thread.
void OnActivate(HWND /*dlg*/, UINT state, HWND /*other*/, BOOL /*minimized*/) {
    if (state != WA_INACTIVE) (void)FontPickerHelper::ActivateDialog();
}

// WM_SYSCOMMAND: intercept the title-bar "?" (context-help) button that the DS_CONTEXTHELP
// style adds, opening the README's Preferences section (see WinAPI::Url::OpenOnContextHelp).
void OnSysCommand(HWND dlg, UINT cmd, int x, int y) {
    WinAPI::Url::OpenOnContextHelp(dlg, cmd, x, y, TEXT("https://github.com/i2van/WinNumTip/blob/main/README.md#preferences"));
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

// Bring Preferences and, when open, the helper process's ChooseFont dialog forward together,
// with the font dialog ending up the active one on top. The font dialog is owned by
// Preferences, so foregrounding it brings Preferences along right behind it. Returns false
// when Preferences is not open.
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

    // Skip entirely once the font dialog is already foreground: this is called from several
    // overlapping paths (WM_NOTIFYICON's own click handling, Show's repeat-open request, ...),
    // and a tray-icon click/double-click can drive several of them within milliseconds of each
    // other.
    if (GetForegroundWindow() == fontDialog) return true;

    // Hand activation to the font dialog; its owner, Preferences, comes forward with it.
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
