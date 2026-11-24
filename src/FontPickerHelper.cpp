#include "stdafx.h"
#include "FontPickerHelper.h"
#include "DialogIcon.h"
#include "resource.h"

// windowsx-style crackers for helper-only messages. The WM_SETTEXT alias preserves
// DefSubclassProc's result, which the standard HANDLE_WM_SETTEXT cracker discards.
#define WM_FONT_SAMPLE_SETTEXT WM_SETTEXT
#define HANDLE_WM_FONT_SAMPLE_SETTEXT(hwnd, wParam, lParam, fn) ((fn)((hwnd), reinterpret_cast<LPCTSTR>(lParam)))
#define WM_CHOOSEFONT_COMMAND WM_COMMAND
#define HANDLE_WM_CHOOSEFONT_COMMAND(hwnd, wParam, lParam, fn) \
    ((fn)((hwnd), static_cast<int>(LOWORD(wParam)), reinterpret_cast<HWND>(lParam), \
          static_cast<UINT>(HIWORD(wParam))))
#define WM_FONT_PICKER_ACTIVATE (WM_APP + 0x100)
#define HANDLE_WM_FONT_PICKER_ACTIVATE(hwnd, wParam, lParam, fn) ((fn)(hwnd), 0L)
#define WM_FONT_PICKER_SYNC_APPLIED (WM_APP + 0x103)
#define HANDLE_WM_FONT_PICKER_SYNC_APPLIED(hwnd, wParam, lParam, fn) ((fn)(hwnd), 0L)

namespace {

constexpr LPCTSTR kHelperSwitch = TEXT("--font-picker");
constexpr DWORD kExchangeVersion = 6;

// How long Close waits for a terminated helper to go away, so its dialog does not outlive
// Preferences on screen.
constexpr DWORD kCloseWaitMs = 1000;

// Page-file-backed shared state exchanged with the short-lived helper process.
struct Exchange {
    DWORD cbSize;
    DWORD version;
    HWND owner;
    // The main process's session serial, echoed back in kShownMessage.
    LONG serial;
    volatile HWND dialog;
    LOGFONT font;
    // The owner's persisted font, the baseline the Apply button measures the live selection
    // against -- distinct from 'font', which is merely what the dialog opens on.
    LOGFONT applied;
    LOGFONT fallback;
    LONG initialDefault;
    LONG appliedDefault;
    volatile LONG status;
};

Exchange* g_exchange;
LONG g_exchangeSerial;

// Main-process state for the open helper session (see FontPickerHelper::Open). g_session is
// read by the thread-pool wait callback, so it changes only while no wait is registered.
struct Session {
    HWND owner;
    LONG serial;
};

Session g_session;
HINSTANCE g_sessionInst;
HANDLE g_mapping;
HANDLE g_process;
HANDLE g_processWait;

// Helper-side state for the live ChooseFont dialog. g_lastAppliedFont/g_lastAppliedDefault
// track the owner's persisted font -- seeded from it, then advanced by each Apply -- so the
// Apply button is enabled exactly while the live selection would change something.
HINSTANCE g_inst;
HWND g_parentDialog;
LOGFONT g_fallbackFont;
bool g_resetToFallback;
LOGFONT g_lastAppliedFont;
bool g_lastAppliedDefault;
bool g_applySucceeded;
HFONT g_sampleFont;
LOGFONT g_dialogResult;
LOGFONT* g_chooseFontResult;
FontPickerHelper::Result g_pendingDialogResult = FontPickerHelper::Result::Pending;

constexpr LPCTSTR kFontTemplate = TEXT("FONTSELECTORDLG");
constexpr LPCTSTR kFontSample = TEXT("1234567890");
constexpr UINT_PTR kSampleSubclassId = 1;
constexpr UINT_PTR kDlgSubclassId = 2;
constexpr UINT_PTR kApplyButtonSubclassId = 3;

void SetDialog(HWND dialog) {
    if (g_exchange) g_exchange->dialog = dialog;
}

[[noreturn]] void Finish(FontPickerHelper::Result result, const LOGFONT* selected = nullptr) {
    if (g_exchange) {
        if (selected)
            MoveMemory(&g_exchange->font, selected, sizeof(g_exchange->font));
        g_exchange->status = static_cast<LONG>(result);
        g_exchange->dialog = nullptr;
        if (IsWindow(g_exchange->owner)) {
            WinAPI::Window::AllowSetForeground(g_exchange->owner);
            (void)EnableWindow(g_exchange->owner, TRUE);
            SetForegroundWindow(g_exchange->owner);
        }
    }
    WinAPI::Process::Terminate();
}

LRESULT CALLBACK FontSampleSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR id, DWORD_PTR ref);

// One measured run of the sample text: the advance extent GDI lays it out in, and the ink
// overhang its outer glyphs lean outside that box. An italic or script face paints past its
// advance width -- the leading or trailing digit can lean well past it -- so sizing and
// centering by the advance alone either clips that overhang against the Sample box, or
// (once it fits) leaves the ink looking off-center because DrawText centers the advance box
// rather than the pixels actually painted. Mirrors FontPreview.cpp's TextRun; kept separate
// since this file measures a single fixed run rather than laying out two.
struct SampleRun {
    SIZE extent;
    int leftBearing;
    int rightBearing;

    // The width the glyphs actually cover, overhangs included.
    [[nodiscard]] int InkWidth() const { return leftBearing + extent.cx + rightBearing; }
};

// Ink overhangs of 'text' at the DC's current font, as positive pixel counts. Only the outer
// glyphs can push ink outside the run: an interior overhang is absorbed by the neighboring
// character's advance. GetCharABCWidths reports the bearings as a negative A (ink left of the
// origin) or C (ink past the advance), and serves TrueType faces only -- a bitmap face
// synthesizes its slant and already carries it in the advance GetTextExtentPoint32 returns, so
// leaving both bearings at zero is the right answer there.
void MeasureSampleBearings(HDC dc, LPCTSTR text, int length, SampleRun& run) {
    run.leftBearing = run.rightBearing = 0;
    if (length <= 0) return;

    ABC abc;
    if (GetCharABCWidths(dc, text[0], text[0], &abc) && abc.abcA < 0)
        run.leftBearing = -abc.abcA;

    const TCHAR last = text[length - 1];
    if (GetCharABCWidths(dc, last, last, &abc) && abc.abcC < 0)
        run.rightBearing = -abc.abcC;
}

// Measure 'text' with 'font' selected into 'dc': its advance extent and ink bearings together,
// so a caller always has both figures from the same font selection. False when the font
// cannot be selected, measured or deselected again; in the last case 'font' is still
// selected into 'dc', so a caller must release the DC before deleting the font.
[[nodiscard]] bool MeasureSampleRun(HDC dc, HFONT font, LPCTSTR text, SampleRun& run) {
    const HGDIOBJ previous = SelectObject(dc, font);
    const bool selected = previous && previous != HGDI_ERROR;
    ASSERT(selected);
    if (!selected) return false;

    const int length = lstrlen(text);
    const bool measured = !!GetTextExtentPoint32(dc, text, length, &run.extent);
    ASSERT(measured);
    if (measured) MeasureSampleBearings(dc, text, length, run);
    const HGDIOBJ restored = SelectObject(dc, previous);
    const bool restoreSucceeded = restored && restored != HGDI_ERROR;
    ASSERT(restoreSucceeded);
    return measured && restoreSucceeded;
}

LRESULT OnFontSampleSetText(HWND hwnd, LPCTSTR /*text*/) {
    return DefSubclassProc(hwnd, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(kFontSample));
}

void OnFontSampleNcDestroy(HWND hwnd) {
    VERIFY(RemoveWindowSubclass(hwnd, FontSampleSubclassProc, kSampleSubclassId));
    FORWARD_WM_NCDESTROY(hwnd, DefSubclassProc);
}

// Paint the Sample control ourselves rather than let comdlg32's stock static class do it: that
// class centers (and clips) strictly by the text's advance width, with no notion of how far an
// italic or script face's outer glyphs paint past it. Insetting the layout box by each side's
// own overhang before centering -- the same technique FontPreview.cpp uses for its two
// edge-aligned runs, here applied to one centered run -- places the ink itself in the middle of
// the control whichever way it leans, instead of the advance box. DT_NOCLIP is essential once
// that inset makes the layout box narrower than the control, so the overhang paints into the
// margin instead of being clipped there; the paint DC is already clipped to the client area,
// so nothing spills onto the border. WM_ERASEBKGND is suppressed (see the subclass proc) since
// this fills the whole client area itself.
void OnFontSamplePaint(HWND hwnd) {
    PAINTSTRUCT ps;
    const HDC dc = BeginPaint(hwnd, &ps);
    ASSERT(dc);
    if (!dc) return;

    RECT rc;
    VERIFY(GetClientRect(hwnd, &rc));

    const HBRUSH brush = FORWARD_WM_CTLCOLORSTATIC(GetParent(hwnd), dc, hwnd, SendMessage);
    if (brush) VERIFY(FillRect(dc, &rc, brush));
    SetBkMode(dc, TRANSPARENT);

    const HFONT font = g_sampleFont ? g_sampleFont : GetWindowFont(hwnd);
    SampleRun run = { { 0, 0 }, 0, 0 };
    if (font && MeasureSampleRun(dc, font, kFontSample, run)) {
        RECT box = rc;
        box.left  += run.leftBearing;
        box.right -= run.rightBearing;

        const HGDIOBJ previous = SelectObject(dc, font);
        const bool selected = previous && previous != HGDI_ERROR;
        ASSERT(selected);
        if (selected) {
            VERIFY(DrawText(dc, kFontSample, -1, &box,
                            DT_CENTER | DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER | DT_NOCLIP));
            VERIFY(SelectObject(dc, previous));
        }
    }

    VERIFY(EndPaint(hwnd, &ps));
}

// Keep comdlg32's Sample control text fixed to the tip digits, and take over its painting so
// an italic or script face's overhang is centered rather than clipped (see OnFontSamplePaint).
LRESULT CALLBACK FontSampleSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR /*id*/, DWORD_PTR /*ref*/) {
    switch (msg) {
        HANDLE_MSG(hwnd, WM_FONT_SAMPLE_SETTEXT, OnFontSampleSetText);
        HANDLE_MSG(hwnd, WM_NCDESTROY, OnFontSampleNcDestroy);
        HANDLE_MSG(hwnd, WM_PAINT, OnFontSamplePaint);
        case WM_ERASEBKGND: return TRUE;
        case WM_SETFONT:
            // comdlg32 applies the small dialog font after the hook's WM_INITDIALOG.
            // Keep our fitted sample font until ApplyChooseFontSample replaces it.
            if (g_sampleFont &&
                reinterpret_cast<HFONT>(wParam) != g_sampleFont)
                return 0;
            break;
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// Apply a selected face/style to the ChooseFont Sample control, sized to fit its box.
void ApplyChooseFontSample(HWND sample, const LOGFONT& selected) {
    constexpr int kSampleFillPercent = 90;
    constexpr int kPercentScale = 100;

    LOGFONT lf;
    MoveMemory(&lf, &selected, sizeof(lf));
    if (lf.lfFaceName[0] == 0) return;

    RECT rc;
    const BOOL gotClientRect = GetClientRect(sample, &rc);
    ASSERT(gotClientRect);
    if (!gotClientRect) return;
    const int boxWidth = rc.right - rc.left;
    const int boxHeight = rc.bottom - rc.top;
    const int fitWidth = MulDiv(boxWidth, kSampleFillPercent, kPercentScale);
    const int fitHeight = MulDiv(boxHeight, kSampleFillPercent, kPercentScale);
    if (fitWidth <= 0 || fitHeight <= 0) return;

    lf.lfWidth = 0;
    const HDC dc = GetDC(sample);
    ASSERT(dc);
    if (!dc) return;

    int bestHeight = 0;
    int low = 1;
    int high = fitHeight;
    while (low <= high) {
        const int height = low + (high - low) / 2;
        lf.lfHeight = -height;
        const HFONT candidate = CreateFontIndirect(&lf);
        ASSERT(candidate);
        if (!candidate) {
            high = height - 1;
            continue;
        }

        SampleRun run = { { 0, 0 }, 0, 0 };
        if (!MeasureSampleRun(dc, candidate, kFontSample, run)) {
            // Release the DC first: a failed restore leaves 'candidate' selected into it.
            VERIFY(ReleaseDC(sample, dc) == 1);
            VERIFY(DeleteObject(candidate));
            return;
        }
        VERIFY(DeleteObject(candidate));

        // Bound by the ink width, not the bare advance: an italic or script face's overhang
        // (see OnFontSamplePaint) must fit the box too, or centering it there would still
        // clip it against the control's border.
        if (run.InkWidth() <= fitWidth && run.extent.cy <= fitHeight) {
            bestHeight = height;
            low = height + 1;
        } else {
            high = height - 1;
        }
    }
    VERIFY(ReleaseDC(sample, dc) == 1);

    if (bestHeight == 0) return;
    lf.lfHeight = -bestHeight;
    const HFONT font = CreateFontIndirect(&lf);
    ASSERT(font);
    if (!font) return;
    HFONT previousFont = g_sampleFont;
    g_sampleFont = font;
    SetWindowFont(sample, font, TRUE);
    WinAPI::GdiObject::Delete(previousFont);
    VERIFY(SetWindowText(sample, kFontSample));
}

// Repaint the ChooseFont Sample control in the currently selected font. With a custom
// template comdlg32 does not refresh it, so read and apply its live selection.
void RefreshChooseFontSample(HWND dialog) {
    const HWND sample = GetDlgItem(dialog, stc5);
    if (!sample) return;

    LOGFONT lf;
    WinAPI::ChooseFont::GetLogFont(dialog, lf);
    ApplyChooseFontSample(sample, lf);
}

[[nodiscard]] bool SameFontSelection(const LOGFONT& left, const LOGFONT& right) {
    return lstrcmp(left.lfFaceName, right.lfFaceName) == 0 &&
           left.lfWeight    == right.lfWeight &&
           left.lfItalic    == right.lfItalic &&
           left.lfUnderline == right.lfUnderline &&
           left.lfStrikeOut == right.lfStrikeOut;
}

struct ApplyButtonSearch {
    HWND button;
};

BOOL CALLBACK FindVisibleApplyButton(HWND window, LPARAM param) {
    ApplyButtonSearch* const search = reinterpret_cast<ApplyButtonSearch*>(param);
    if (!IsWindowVisible(window)) return TRUE;

    const HWND apply = GetDlgItem(window, psh3);
    if (!apply) return TRUE;

    search->button = apply;
    return FALSE;
}

[[nodiscard]] HWND ChooseFontApplyButton(HWND dialog) {
    const HWND direct = GetDlgItem(dialog, psh3);
    if (IsWindowVisible(dialog) && direct) return direct;

    ApplyButtonSearch search = { nullptr };
    EnumThreadWindows(GetCurrentThreadId(), FindVisibleApplyButton,
                      reinterpret_cast<LPARAM>(&search));
    return search.button ? search.button : direct;
}

LRESULT CALLBACK ApplyButtonSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                         UINT_PTR id, DWORD_PTR ref);

void PreserveAppliedSelection(HWND apply) {
    if (!g_applySucceeded) return;

    g_applySucceeded = false;
    WinAPI::Window::Enable(apply, false);
}

void OnApplyButtonNcDestroy(HWND apply) {
    VERIFY(RemoveWindowSubclass(apply, ApplyButtonSubclassProc, kApplyButtonSubclassId));
    FORWARD_WM_NCDESTROY(apply, DefSubclassProc);
}

LRESULT CALLBACK ApplyButtonSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                         UINT_PTR /*id*/, DWORD_PTR /*ref*/) {
    switch (msg) {
        HANDLE_MSG(hwnd, WM_NCDESTROY, OnApplyButtonNcDestroy);
    }

    const LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
    PreserveAppliedSelection(hwnd);
    return result;
}

void SetChooseFontApplyEnabled(HWND dialog, bool enabled) {
    const HWND apply = ChooseFontApplyButton(dialog);
    if (!apply) return;
    VERIFY(SetWindowSubclass(
        apply, ApplyButtonSubclassProc, kApplyButtonSubclassId, 0));
    ShowWindow(apply, SW_SHOW);
    WinAPI::Window::Enable(apply, enabled);
}

// Adopt the owner's persisted font, as last published to the exchange (see
// FontPickerHelper::SetApplied), as the Apply baseline.
void SyncAppliedFont() {
    if (!g_exchange) return;
    MemoryBarrier();
    MoveMemory(&g_lastAppliedFont, &g_exchange->applied, sizeof(g_lastAppliedFont));
    g_lastAppliedDefault = g_exchange->appliedDefault != 0;
}

void UpdateChooseFontApplyState(HWND dialog) {
    LOGFONT selected;
    WinAPI::ChooseFont::GetLogFont(dialog, selected);

    bool changed = g_resetToFallback != g_lastAppliedDefault;
    if (!changed && !g_resetToFallback)
        changed = !SameFontSelection(selected, g_lastAppliedFont);
    SetChooseFontApplyEnabled(dialog, changed);
}

void ApplyChooseFontSelection(HWND dialog) {
    LOGFONT selected;
    WinAPI::ChooseFont::GetLogFont(dialog, selected);

    if (!g_exchange) {
        ASSERT(g_exchange);
        return;
    }

    const FontPickerHelper::Result result = g_resetToFallback
        ? FontPickerHelper::Result::Default
        : FontPickerHelper::Result::Chosen;
    if (result == FontPickerHelper::Result::Chosen && selected.lfFaceName[0] == 0)
        return;
    if (result == FontPickerHelper::Result::Chosen) {
        MoveMemory(&g_exchange->font, &selected, sizeof(g_exchange->font));
        MoveMemory(&g_lastAppliedFont, &selected, sizeof(g_lastAppliedFont));
    }

    if (IsWindow(g_exchange->owner)) {
        SendMessage(g_exchange->owner, FontPickerHelper::kApplySelectionMessage,
                    static_cast<WPARAM>(result), 0);
        g_lastAppliedDefault = result == FontPickerHelper::Result::Default;
        g_applySucceeded = true;
    }
}

// Drive the visible controls so comdlg32 rebuilds its internal LOGFONT for the fallback.
[[nodiscard]] bool ResetChooseFontToFallback(HWND dialog) {
    const HWND nameCombo = GetDlgItem(dialog, cmb1);
    const HWND styleCombo = GetDlgItem(dialog, cmb2);
    if (!nameCombo) return false;

    VERIFY(CheckDlgButton(dialog, chx1, BST_UNCHECKED));
    VERIFY(CheckDlgButton(dialog, chx2, BST_UNCHECKED));

    const int face = ComboBox_FindStringExact(nameCombo, -1, g_fallbackFont.lfFaceName);
    if (face < 0) return false;
    ComboBox_SetCurSel(nameCombo, face);
    FORWARD_WM_COMMAND(dialog, cmb1, nameCombo, CBN_SELCHANGE, SendMessage);

    if (styleCombo) {
        const int style = ComboBox_FindStringExact(styleCombo, -1, TEXT("Regular"));
        if (style >= 0) {
            ComboBox_SetCurSel(styleCombo, style);
            FORWARD_WM_COMMAND(dialog, cmb2, styleCombo, CBN_SELCHANGE, SendMessage);
        }
    }

    return true;
}

void OnChooseFontCommand(HWND dialog, int id, HWND control, UINT notification) {
    if (id == IDCANCEL) {
        g_pendingDialogResult = FontPickerHelper::Result::Cancelled;
        FORWARD_WM_COMMAND(dialog, id, control, notification, DefSubclassProc);
        if (IsWindowVisible(dialog))
            g_pendingDialogResult = FontPickerHelper::Result::Pending;
        return;
    }

    if (id == IDOK) {
        WinAPI::ChooseFont::GetLogFont(dialog, g_dialogResult);
        g_pendingDialogResult = g_resetToFallback
            ? FontPickerHelper::Result::Default
            : FontPickerHelper::Result::Chosen;
        FORWARD_WM_COMMAND(dialog, id, control, notification, DefSubclassProc);
        // CF_FORCEFONTEXIST can reject typed text and keep the dialog open.
        if (IsWindowVisible(dialog)) {
            g_pendingDialogResult = FontPickerHelper::Result::Pending;
        } else if (g_pendingDialogResult == FontPickerHelper::Result::Chosen &&
                   g_chooseFontResult && g_chooseFontResult->lfFaceName[0] != 0) {
            MoveMemory(&g_dialogResult, g_chooseFontResult, sizeof(g_dialogResult));
        }
        return;
    }

    FORWARD_WM_COMMAND(dialog, id, control, notification, DefSubclassProc);
    const bool comboChanged =
        (id == cmb1 || id == cmb2) &&
        (notification == CBN_SELCHANGE || notification == CBN_EDITCHANGE);
    const bool effectToggled =
        (id == chx1 || id == chx2) && notification == BN_CLICKED;
    if (comboChanged || effectToggled) {
        RefreshChooseFontSample(dialog);
        g_resetToFallback = false;
        UpdateChooseFontApplyState(dialog);
    }

    // Double-clicking a name in the Font list or a style in the Font style list is the
    // shortcut for applying it -- the lists are CBS_SIMPLE, so their always-visible list box
    // reports the second click as CBN_DBLCLK, by which point the first has already moved the
    // selection and settled the Apply state. The press is routed through the Apply button so
    // it follows the identical path as a real click, and so it stays a no-op while that
    // button is disabled because the selection is already the applied one. Focus goes back
    // where it was: a real click leaves it on the button, but a double-click in a list should
    // leave the user free to keep browsing that list with the arrow keys.
    if (notification == CBN_DBLCLK && (id == cmb1 || id == cmb2)) {
        const HWND apply = ChooseFontApplyButton(dialog);
        if (apply && IsWindowEnabled(apply)) {
            const HWND focus = GetFocus();
            WinAPI::Button::Click(apply);
            if (focus) SetFocus(focus);
        }
    }
}

LRESULT OnChooseFontNotify(HWND dialog, int idFrom, NMHDR* notification) {
    if (notification->idFrom == IDC_FONT_RESET_LINK &&
        (notification->code == NM_CLICK || notification->code == NM_RETURN)) {
        if (ResetChooseFontToFallback(dialog)) {
            g_resetToFallback = true;
            UpdateChooseFontApplyState(dialog);
        }
        return 0;
    }

    return FORWARD_WM_NOTIFY(dialog, idFrom, notification, DefSubclassProc);
}

void OnChooseFontClose(HWND dialog) {
    g_pendingDialogResult = FontPickerHelper::Result::Cancelled;
    FORWARD_WM_CLOSE(dialog, DefSubclassProc);
    if (IsWindowVisible(dialog))
        g_pendingDialogResult = FontPickerHelper::Result::Pending;
}

// The dialog is going away: publish its result and end the helper process (see Finish).
void OnChooseFontNcDestroy(HWND /*dialog*/) {
    const FontPickerHelper::Result result =
        g_pendingDialogResult == FontPickerHelper::Result::Pending
            ? FontPickerHelper::Result::Cancelled
            : g_pendingDialogResult;
    Finish(result, result == FontPickerHelper::Result::Chosen ? &g_dialogResult : nullptr);
}

void OnChooseFontShowWindow(HWND dialog, BOOL show, UINT status) {
    FORWARD_WM_SHOWWINDOW(dialog, show, status, DefSubclassProc);
    if (show) {
        RefreshChooseFontSample(dialog);
        UpdateChooseFontApplyState(dialog);
        if (g_exchange && IsWindow(g_exchange->owner))
            (void)PostMessage(g_exchange->owner, FontPickerHelper::kShownMessage,
                              static_cast<WPARAM>(g_exchange->serial), 0);
    }
}

// comdlg32 paints its own Sample text straight onto the dialog surface, over the Sample
// control's rectangle, in the small dialog font. The control repaints in the fitted font
// only when its own WM_PAINT is dispatched, which can be hundreds of milliseconds later
// while comdlg32 is still enumerating fonts -- long enough to be seen as a wrong-font
// flash. Repaint the control synchronously after every dialog paint so the fitted font is
// restored within the same paint cycle.
void OnChooseFontPaint(HWND dialog) {
    FORWARD_WM_PAINT(dialog, DefSubclassProc);
    if (const HWND sample = GetDlgItem(dialog, stc5))
        VERIFY(RedrawWindow(sample, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW));
}

// WM_FONT_PICKER_ACTIVATE (posted by FontPickerHelper::ActivateDialog in the main process,
// right after its own SetForegroundWindow call): restore/raise/activate the font dialog on its
// own UI thread. PreferencesDialog::ActivateDialog foregrounds Preferences first, so it ends
// up directly behind the font dialog.
void OnActivateChooseFont(HWND dialog) {
    if (IsIconic(dialog)) ShowWindow(dialog, SW_RESTORE);
    SetActiveWindow(dialog);
    BringWindowToTop(dialog);
    SetForegroundWindow(dialog);
}

// WM_FONT_PICKER_SYNC_APPLIED (posted by FontPickerHelper::SetApplied): Preferences saved a
// font, so re-measure the Apply button against it.
void OnSyncAppliedFont(HWND dialog) {
    SyncAppliedFont();
    UpdateChooseFontApplyState(dialog);
}

UINT_PTR OnChooseFontHookCommand(HWND dialog, int id, HWND /*control*/,
                                 UINT notification) {
    if (id == psh3 && notification == BN_CLICKED) {
        ApplyChooseFontSelection(dialog);
        return TRUE;
    }
    return FALSE;
}

// WM_SYSCOMMAND: the title-bar "?" (context-help) button that FONTSELECTORDLG's DS_CONTEXTHELP
// style adds opens the README's Tip font section (see WinAPI::Url::OpenOnContextHelp).
void OnChooseFontSysCommand(HWND dialog, UINT cmd, int x, int y) {
    WinAPI::Url::OpenOnContextHelp(dialog, cmd, x, y,
                                   TEXT("https://github.com/i2van/WinNumTip/blob/main/README.md#tip-font"),
                                   DefSubclassProc);
}

LRESULT CALLBACK ChooseFontDlgSubclass(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                       UINT_PTR /*id*/, DWORD_PTR /*ref*/) {
    switch (msg) {
        HANDLE_MSG(hwnd, WM_COMMAND, OnChooseFontCommand);
        HANDLE_MSG(hwnd, WM_NOTIFY, OnChooseFontNotify);
        HANDLE_MSG(hwnd, WM_CLOSE, OnChooseFontClose);
        HANDLE_MSG(hwnd, WM_NCDESTROY, OnChooseFontNcDestroy);
        HANDLE_MSG(hwnd, WM_SHOWWINDOW, OnChooseFontShowWindow);
        HANDLE_MSG(hwnd, WM_PAINT, OnChooseFontPaint);
        HANDLE_MSG(hwnd, WM_FONT_PICKER_ACTIVATE, OnActivateChooseFont);
        HANDLE_MSG(hwnd, WM_FONT_PICKER_SYNC_APPLIED, OnSyncAppliedFont);
        HANDLE_MSG(hwnd, WM_SYSCOMMAND, OnChooseFontSysCommand);
        default: return DefSubclassProc(hwnd, msg, wParam, lParam);
    }
}

// The widths of 'window's invisible DWM borders: GetWindowRect includes them but nothing is
// drawn there, so windows abutted by their window rects alone would show a gap between them.
[[nodiscard]] RECT InvisibleBorders(HWND window) {
    RECT borders = { 0, 0, 0, 0 };
    RECT bounds;
    RECT frame;
    if (GetWindowRect(window, &bounds) &&
        SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame,
                                        sizeof(frame)))) {
        borders.left   = frame.left - bounds.left;
        borders.top    = frame.top - bounds.top;
        borders.right  = bounds.right - frame.right;
        borders.bottom = bounds.bottom - frame.bottom;
    }
    return borders;
}

// Place 'dialog' beside 'owner' -- on its right, else on its left -- top-aligned with it but
// kept within the owner's monitor work area. Returns false, leaving 'dialog' where it is,
// when neither side has room for it.
[[nodiscard]] bool PlaceBesideOwner(HWND dialog, HWND owner) {
    RECT ownerRect;
    RECT dialogRect;
    if (!GetWindowRect(owner, &ownerRect) || !GetWindowRect(dialog, &dialogRect)) return false;

    MONITORINFO monitor;
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfo(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor))
        return false;
    const RECT& work = monitor.rcWork;

    // Work with the visible frames. Both dialogs have the same frame style, so the owner's
    // invisible borders stand in for the dialog's, which DWM does not report before it shows.
    const RECT borders = InvisibleBorders(owner);
    const int ownerLeft  = ownerRect.left + borders.left;
    const int ownerRight = ownerRect.right - borders.right;
    const int ownerTop   = ownerRect.top + borders.top;
    const int width  = dialogRect.right - dialogRect.left - borders.left - borders.right;
    const int height = dialogRect.bottom - dialogRect.top - borders.top - borders.bottom;
    if (height > work.bottom - work.top) return false;

    int x;
    if (ownerRight >= work.left && ownerRight + width <= work.right)
        x = ownerRight;
    else if (ownerLeft <= work.right && ownerLeft - width >= work.left)
        x = ownerLeft - width;
    else
        return false;
    const int y = max(static_cast<int>(work.top), min(ownerTop, static_cast<int>(work.bottom) - height));

    return !!SetWindowPos(dialog, nullptr, x - borders.left, y - borders.top, 0, 0,
                          SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// Center 'dialog' over 'owner', kept within the owner's monitor work area.
void CenterOverOwner(HWND dialog, HWND owner) {
    RECT ownerRect;
    RECT dialogRect;
    if (!GetWindowRect(owner, &ownerRect) || !GetWindowRect(dialog, &dialogRect)) return;

    const int width  = dialogRect.right - dialogRect.left;
    const int height = dialogRect.bottom - dialogRect.top;
    int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;

    MONITORINFO monitor;
    monitor.cbSize = sizeof(monitor);
    if (GetMonitorInfo(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor)) {
        const RECT& work = monitor.rcWork;
        x = min(max(x, static_cast<int>(work.left)), static_cast<int>(work.right) - width);
        y = min(max(y, static_cast<int>(work.top)), static_cast<int>(work.bottom) - height);
    }
    VERIFY(SetWindowPos(dialog, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE));
}

BOOL OnChooseFontInitDialog(HWND dialog, HWND /*focus*/, LPARAM chooseFontParam) {
    SetDialog(dialog);
    // A font saved after launch but before the dialog was published above found no dialog
    // to notify (see FontPickerHelper::SetApplied), so pick it up here.
    SyncAppliedFont();

    // Give the dialog the app icon (shared with About and Preferences) rather than showing
    // the generic one.
    DialogIcon::Set(dialog, g_inst);

    // The dialog is modeless, so open it beside Preferences, leaving that in view and
    // usable; when neither side has room, center it over Preferences instead.
    if (IsWindow(g_parentDialog) && !PlaceBesideOwner(dialog, g_parentDialog))
        CenterOverOwner(dialog, g_parentDialog);
    SetForegroundWindow(dialog);

    if (const HWND sample = GetDlgItem(dialog, stc5)) {
        VERIFY(SetWindowSubclass(sample, FontSampleSubclassProc, kSampleSubclassId, 0));
        VERIFY(SetWindowText(sample, kFontSample));
        const CHOOSEFONT* const chooseFont =
            reinterpret_cast<const CHOOSEFONT*>(chooseFontParam);
        if (chooseFont && chooseFont->lpLogFont)
            ApplyChooseFontSample(sample, *chooseFont->lpLogFont);
    }
    VERIFY(SetWindowSubclass(dialog, ChooseFontDlgSubclass, kDlgSubclassId, 0));
    UpdateChooseFontApplyState(dialog);

    if (const HWND nameCombo = GetDlgItem(dialog, cmb1))
        SetFocus(nameCombo);

    return FALSE;
}

UINT_PTR CALLBACK ChooseFontHook(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        HANDLE_MSG(hwnd, WM_INITDIALOG, OnChooseFontInitDialog);
        HANDLE_MSG(hwnd, WM_CHOOSEFONT_COMMAND, OnChooseFontHookCommand);
        default: return 0;
    }
}

[[nodiscard]] FontPickerHelper::Result ShowDialog(HINSTANCE inst, HWND owner,
                                                 const LOGFONT& initial, bool initialDefault,
                                                 const LOGFONT& applied, bool appliedDefault,
                                                 const LOGFONT& fallback,
                                                 LOGFONT& selected) {
    g_inst = inst;
    g_parentDialog = owner;
    g_resetToFallback = initialDefault;
    MoveMemory(&g_lastAppliedFont, &applied, sizeof(g_lastAppliedFont));
    g_lastAppliedDefault = appliedDefault;
    g_applySucceeded = false;
    g_pendingDialogResult = FontPickerHelper::Result::Pending;
    ZeroMemory(&g_dialogResult, sizeof(g_dialogResult));
    MoveMemory(&g_fallbackFont, &fallback, sizeof(g_fallbackFont));

    // 'owner' is the main process's Preferences window. The dialog is modeless with respect
    // to it, and the main process does not wait for it: it learns of the helper's exit
    // through a thread-pool wait (see Open).
    if (!IsWindow(owner)) return FontPickerHelper::Result::Failed;

    // Own the dialog by a hidden window of this process rather than by Preferences: as with
    // Preferences (owned by the hidden message window), that gives it an Alt+Tab entry of
    // its own but no taskbar button, and keeps it out of Preferences' last-active-popup
    // chain, so switching to Preferences activates Preferences rather than the font dialog.
    // ChooseFont's DialogBox disables this window instead of Preferences, which therefore
    // stays usable. It covers Preferences' rect so the dialog, which opens on its owner's
    // monitor, starts at Preferences' DPI -- PlaceBesideOwner then measures it at its final
    // size, rather than before a cross-monitor DPI change resizes it.
    RECT ownerRect;
    if (!GetWindowRect(owner, &ownerRect)) return FontPickerHelper::Result::Failed;
    const HWND hiddenOwner = CreateWindowEx(WS_EX_TOOLWINDOW, WC_STATIC, TEXT(""), WS_POPUP,
                                            ownerRect.left, ownerRect.top,
                                            ownerRect.right - ownerRect.left,
                                            ownerRect.bottom - ownerRect.top,
                                            nullptr, nullptr, g_inst, nullptr);
    if (!hiddenOwner) return FontPickerHelper::Result::Failed;

    LOGFONT font;
    MoveMemory(&font, &initial, sizeof(font));

    CHOOSEFONT chooseFont;
    ZeroMemory(&chooseFont, sizeof(chooseFont));
    chooseFont.lStructSize = sizeof(chooseFont);
    chooseFont.hwndOwner = hiddenOwner;
    chooseFont.hInstance = g_inst;
    chooseFont.lpTemplateName = kFontTemplate;
    chooseFont.lpLogFont = &font;
    chooseFont.lpfnHook = ChooseFontHook;
    chooseFont.Flags = CF_INITTOLOGFONTSTRUCT | CF_EFFECTS | CF_SCREENFONTS | CF_APPLY |
                       CF_FORCEFONTEXIST | CF_ENABLEHOOK | CF_ENABLETEMPLATE;

    g_chooseFontResult = &font;
    const BOOL chosen = ChooseFont(&chooseFont);
    g_chooseFontResult = nullptr;
    const DWORD error = chosen ? 0 : CommDlgExtendedError();
    VERIFY(DestroyWindow(hiddenOwner));

    SetDialog(nullptr);
    g_parentDialog = nullptr;
    WinAPI::GdiObject::Delete(g_sampleFont);

    if (!chosen)
        return error == 0
            ? FontPickerHelper::Result::Cancelled
            : FontPickerHelper::Result::Failed;

    MoveMemory(&selected, &font, sizeof(selected));
    return g_resetToFallback
        ? FontPickerHelper::Result::Default
        : FontPickerHelper::Result::Chosen;
}

// Thread-pool callback: the helper process exited, so tell its owner to collect the result.
VOID CALLBACK OnHelperExited(PVOID context, BOOLEAN /*timedOut*/) {
    const Session* const session = static_cast<const Session*>(context);
    (void)PostMessage(session->owner, FontPickerHelper::kClosedMessage,
                      static_cast<WPARAM>(session->serial), 0);
}

// Release the main process's handles to the helper session. Unregistering blocks until a
// running OnHelperExited callback has returned, so g_session is free to change afterwards.
void ReleaseSession() {
    if (g_processWait) {
        VERIFY(UnregisterWaitEx(g_processWait, INVALID_HANDLE_VALUE));
        g_processWait = nullptr;
    }
    if (g_process) {
        VERIFY(CloseHandle(g_process));
        g_process = nullptr;
    }
    if (g_exchange) {
        VERIFY(UnmapViewOfFile(g_exchange));
        g_exchange = nullptr;
    }
    if (g_mapping) {
        VERIFY(CloseHandle(g_mapping));
        g_mapping = nullptr;
    }
    g_session.owner = nullptr;
}

} // namespace

namespace FontPickerHelper {

bool RunIfRequested(HINSTANCE inst) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;

    const bool helper = argc == 3 && lstrcmpW(argv[1], kHelperSwitch) == 0;
    if (!helper) {
        LocalFree(argv);
        return false;
    }

    const HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, argv[2]);
    LocalFree(argv);
    if (!mapping) return true;

    Exchange* const exchange = static_cast<Exchange*>(
        MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Exchange)));
    if (!exchange) {
        CloseHandle(mapping);
        return true;
    }

    if (exchange->cbSize == sizeof(Exchange) && exchange->version == kExchangeVersion) {
        g_exchange = exchange;

        Result result = Result::Failed;
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(com)) {
            if (WinAPI::CommonControls::Init(ICC_LINK_CLASS)) {
                LOGFONT selected;
                ZeroMemory(&selected, sizeof(selected));
                result = ShowDialog(
                    inst, exchange->owner, exchange->font,
                    exchange->initialDefault != 0, exchange->applied,
                    exchange->appliedDefault != 0, exchange->fallback, selected);
                if (result == Result::Chosen)
                    MoveMemory(&exchange->font, &selected, sizeof(exchange->font));
            }
            CoUninitialize();
        }
        exchange->status = static_cast<LONG>(result);

        g_exchange = nullptr;
    }

    VERIFY(UnmapViewOfFile(exchange));
    VERIFY(CloseHandle(mapping));
    return true;
}

void ShowError(HINSTANCE inst, HWND owner) {
    TCHAR text[96];
    TCHAR caption[32];
    MessageBox(owner, WinAPI::String::Load(inst, IDS_FONT_PICKER_ERROR, text),
               WinAPI::String::Load(inst, IDS_APP_NAME, caption), MB_OK | MB_ICONERROR);
}

OpenResult Open(HINSTANCE inst, HWND owner, const LOGFONT& initial, bool initialDefault,
                const LOGFONT& applied, bool appliedDefault, const LOGFONT& fallback) {
    // Only one font dialog at a time: bring the open one forward instead.
    if (g_exchange) {
        (void)ActivateDialog();
        return OpenResult::AlreadyOpen;
    }

    TCHAR mappingName[128];
    const LONG serial = ++g_exchangeSerial;
    WinAPI::String::Format(mappingName, TEXT(R"(Local\WinNumTipFontPicker)") APP_GUID TEXT("-%lu-%ld"),
                           GetCurrentProcessId(), serial);

    const HANDLE mapping = CreateFileMapping(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                             0, sizeof(Exchange), mappingName);
    if (!mapping || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mapping) CloseHandle(mapping);
        return OpenResult::Failed;
    }

    Exchange* const exchange = static_cast<Exchange*>(
        MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Exchange)));
    if (!exchange) {
        CloseHandle(mapping);
        return OpenResult::Failed;
    }

    ZeroMemory(exchange, sizeof(*exchange));
    exchange->cbSize = sizeof(*exchange);
    exchange->version = kExchangeVersion;
    exchange->owner = owner;
    exchange->serial = serial;
    MoveMemory(&exchange->font, &initial, sizeof(exchange->font));
    MoveMemory(&exchange->applied, &applied, sizeof(exchange->applied));
    MoveMemory(&exchange->fallback, &fallback, sizeof(exchange->fallback));
    exchange->initialDefault = initialDefault ? TRUE : FALSE;
    exchange->appliedDefault = appliedDefault ? TRUE : FALSE;

    TCHAR exe[MAX_PATH];
    const DWORD exeLength = GetModuleFileName(nullptr, exe, ARRAYSIZE(exe));
    TCHAR commandLine[MAX_PATH + ARRAYSIZE(mappingName) + 32];
    if (exeLength == 0 || exeLength >= ARRAYSIZE(exe)) {
        VERIFY(UnmapViewOfFile(exchange));
        VERIFY(CloseHandle(mapping));
        return OpenResult::Failed;
    }
    WinAPI::String::Format(commandLine, TEXT("\"%s\" %s \"%s\""), exe, kHelperSwitch, mappingName);

    STARTUPINFO si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    const BOOL started = CreateProcess(exe, commandLine, nullptr, nullptr, FALSE, 0,
                                       nullptr, nullptr, &si, &pi);
    if (!started) {
        VERIFY(UnmapViewOfFile(exchange));
        VERIFY(CloseHandle(mapping));
        return OpenResult::Failed;
    }

    VERIFY(CloseHandle(pi.hThread));
    AllowSetForegroundWindow(pi.dwProcessId);

    g_exchange = exchange;
    g_mapping = mapping;
    g_process = pi.hProcess;
    g_sessionInst = inst;
    g_session.owner = owner;
    g_session.serial = serial;
    if (!RegisterWaitForSingleObject(&g_processWait, g_process, OnHelperExited, &g_session,
                                     INFINITE, WT_EXECUTEONLYONCE)) {
        g_processWait = nullptr;
        VERIFY(TerminateProcess(g_process, ERROR_GEN_FAILURE));
        ReleaseSession();
        return OpenResult::Failed;
    }
    return OpenResult::Opened;
}

bool IsSession(WPARAM session) {
    return g_exchange && static_cast<LONG>(session) == g_session.serial;
}

Result Complete(WPARAM session, LOGFONT& selected) {
    if (!g_exchange || static_cast<LONG>(session) != g_session.serial)
        return Result::Pending;

    const Result result = static_cast<Result>(g_exchange->status);
    if (result == Result::Chosen)
        MoveMemory(&selected, &g_exchange->font, sizeof(selected));
    const HWND owner = g_session.owner;
    const HINSTANCE inst = g_sessionInst;
    ReleaseSession();

    if (result == Result::Failed || result == Result::Pending) {
        ShowError(inst, owner);
        return Result::Failed;
    }
    return result;
}

void Close() {
    if (!g_exchange) return;

    // Unregister first, so the exit this causes posts no kClosedMessage; any already posted
    // is recognized as stale by Complete. TerminateProcess fails harmlessly when the helper
    // has already exited on its own.
    if (g_processWait) {
        VERIFY(UnregisterWaitEx(g_processWait, INVALID_HANDLE_VALUE));
        g_processWait = nullptr;
    }
    (void)TerminateProcess(g_process, ERROR_CANCELLED);
    (void)WaitForSingleObject(g_process, kCloseWaitMs);
    ReleaseSession();
}

void SetApplied(const LOGFONT& applied, bool appliedDefault) {
    if (!g_exchange) return;

    MoveMemory(&g_exchange->applied, &applied, sizeof(g_exchange->applied));
    g_exchange->appliedDefault = appliedDefault ? TRUE : FALSE;
    // Pairs with the helper's publish-then-read in OnChooseFontInitDialog: either it reads
    // this font there, or the dialog is visible here and is told to re-read it.
    MemoryBarrier();
    if (const HWND dialog = g_exchange->dialog)
        (void)PostMessage(dialog, WM_FONT_PICKER_SYNC_APPLIED, 0, 0);
}

bool ReadAppliedFont(LOGFONT& selected) {
    if (!g_exchange) return false;
    MoveMemory(&selected, &g_exchange->font, sizeof(selected));
    return selected.lfFaceName[0] != 0;
}

// Foreground the still-open font dialog (from the main process) and ask it to fully
// reactivate itself on its own UI thread (OnActivateChooseFont, via the posted message
// below). PreferencesDialog::ActivateDialog raises Preferences first.
bool ActivateDialog() {
    const HWND active = ActiveDialog();
    if (!active) return false;

    // Skip if already foreground: this is called from several overlapping paths
    // (PreferencesDialog::ActivateDialog, a repeat click on Select..., ...), and a tray-icon
    // click/double-click can drive several of them within milliseconds of each other.
    if (GetForegroundWindow() == active) return true;

    WinAPI::Window::AllowSetForeground(active);
    SetForegroundWindow(active);
    return !!PostMessage(active, WM_FONT_PICKER_ACTIVATE, 0, 0);
}

// A hidden dialog is treated as gone: EndDialog hides it before it is destroyed (Finish then
// activates Preferences), and redirecting that activation back to it would bounce focus.
HWND ActiveDialog() {
    const HWND active = g_exchange ? g_exchange->dialog : nullptr;
    return active && IsWindow(active) && IsWindowVisible(active) ? active : nullptr;
}

} // namespace FontPickerHelper
