#include "stdafx.h"
#include "FontPickerHelper.h"
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

namespace {

constexpr LPCTSTR kHelperSwitch = TEXT("--font-picker");
constexpr DWORD kExchangeVersion = 3;

enum class WaitResult {
    Completed,
    Quitting,
    Failed,
};

// Page-file-backed shared state exchanged with the short-lived helper process.
struct Exchange {
    DWORD cbSize;
    DWORD version;
    HWND owner;
    volatile HWND dialog;
    LOGFONT font;
    LOGFONT fallback;
    LONG initialDefault;
    volatile LONG status;
    // Set (by either side) while it is itself driving the *other* window's activation, so the
    // WM_ACTIVATE that results doesn't bounce back as a redundant kActivateOwnerMessage --
    // see ActivateDialog, OnActivateChooseFont and OnChooseFontActivate.
    volatile LONG suppressOwnerActivate;
};

Exchange* g_exchange = nullptr;
HWND g_dialog = nullptr;
LONG g_exchangeSerial = 0;
bool g_isHelperProcess = false;

// Helper-side state for the live ChooseFont dialog.
HINSTANCE g_inst = nullptr;
HWND g_parentDialog = nullptr;
LOGFONT g_fallbackFont = { 0 };
bool g_resetToFallback = false;
LOGFONT g_lastAppliedFont = { 0 };
bool g_lastAppliedDefault = false;
bool g_applySucceeded = false;
HFONT g_sampleFont = nullptr;
FontPickerHelper::Result g_pendingDialogResult = FontPickerHelper::Result::Pending;
LOGFONT g_dialogResult = { 0 };
LOGFONT* g_chooseFontResult = nullptr;

constexpr LPCTSTR kFontTemplate = TEXT("FONTSELECTORDLG");
constexpr LPCTSTR kFontSample = TEXT("0123456789");
constexpr UINT_PTR kSampleSubclassId = 1;
constexpr UINT_PTR kDlgSubclassId = 2;
constexpr UINT_PTR kApplyButtonSubclassId = 3;

void SetDialog(HWND dialog) {
    g_dialog = dialog;
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
// advance width -- the trailing '9' in the tip digits typically leans furthest -- so sizing
// and centering by the advance alone either clips that overhang against the Sample box, or
// (once it fits) leaves the ink looking off-centre because DrawText centers the advance box
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
// glyphs can push ink outside the run: an interior overhang is absorbed by the neighbouring
// character's advance. GetCharABCWidths reports the bearings as a negative A (ink left of the
// origin) or C (ink past the advance), and serves TrueType faces only -- a bitmap face
// synthesises its slant and already carries it in the advance GetTextExtentPoint32 returns, so
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
// so a caller always has both figures from the same font selection.
[[nodiscard]] bool MeasureSampleRun(HDC dc, HFONT font, LPCTSTR text, SampleRun& run) {
    const HGDIOBJ previous = SelectObject(dc, font);
    const bool selected = previous && previous != HGDI_ERROR;
    ASSERT(selected);
    if (!selected) return false;

    const int length = lstrlen(text);
    const bool measured = GetTextExtentPoint32(dc, text, length, &run.extent) != FALSE;
    ASSERT(measured);
    if (measured) MeasureSampleBearings(dc, text, length, run);
    VERIFY(SelectObject(dc, previous));
    return measured;
}

LRESULT OnFontSampleSetText(HWND hwnd, LPCTSTR /*text*/) {
    return DefSubclassProc(hwnd, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(kFontSample));
}

void OnFontSampleNcDestroy(HWND hwnd) {
    VERIFY(RemoveWindowSubclass(hwnd, FontSampleSubclassProc, kSampleSubclassId));
    FORWARD_WM_NCDESTROY(hwnd, DefSubclassProc);
}

// Paint the Sample control ourselves rather than let comdlg32's stock static class do it: that
// class centres (and clips) strictly by the text's advance width, with no notion of how far an
// italic or script face's outer glyphs paint past it. Insetting the layout box by each side's
// own overhang before centering -- the same technique FontPreview.cpp uses for its two
// edge-aligned runs, here applied to one centred run -- places the ink itself in the middle of
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

    const HBRUSH brush = reinterpret_cast<HBRUSH>(
        SendMessage(GetParent(hwnd), WM_CTLCOLORSTATIC,
                    reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(hwnd)));
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
// an italic or script face's overhang is centred rather than clipped (see OnFontSamplePaint).
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

        const HGDIOBJ previous = SelectObject(dc, candidate);
        const bool selectSucceeded = previous && previous != HGDI_ERROR;
        ASSERT(selectSucceeded);
        if (!selectSucceeded) {
            VERIFY(DeleteObject(candidate));
            VERIFY(ReleaseDC(sample, dc) == 1);
            return;
        }

        SampleRun run = { { 0, 0 }, 0, 0 };
        const BOOL measured = GetTextExtentPoint32(dc, kFontSample, lstrlen(kFontSample), &run.extent);
        ASSERT(measured);
        if (measured) MeasureSampleBearings(dc, kFontSample, lstrlen(kFontSample), run);
        const HGDIOBJ restored = SelectObject(dc, previous);
        const bool restoreSucceeded = restored && restored != HGDI_ERROR;
        ASSERT(restoreSucceeded);
        if (!restoreSucceeded) {
            VERIFY(ReleaseDC(sample, dc) == 1);
            VERIFY(DeleteObject(candidate));
            return;
        }
        VERIFY(DeleteObject(candidate));
        if (!measured) {
            VERIFY(ReleaseDC(sample, dc) == 1);
            return;
        }

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
    if (g_isHelperProcess && id == IDCANCEL) {
        g_pendingDialogResult = FontPickerHelper::Result::Cancelled;
        FORWARD_WM_COMMAND(dialog, id, control, notification, DefSubclassProc);
        if (IsWindowVisible(dialog))
            g_pendingDialogResult = FontPickerHelper::Result::Pending;
        return;
    }

    if (g_isHelperProcess && id == IDOK) {
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
    if (!g_isHelperProcess) {
        FORWARD_WM_CLOSE(dialog, DefSubclassProc);
        return;
    }

    g_pendingDialogResult = FontPickerHelper::Result::Cancelled;
    FORWARD_WM_CLOSE(dialog, DefSubclassProc);
    if (IsWindowVisible(dialog))
        g_pendingDialogResult = FontPickerHelper::Result::Pending;
}

LRESULT CALLBACK ChooseFontDlgSubclass(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                       UINT_PTR id, DWORD_PTR ref);

void OnChooseFontNcDestroy(HWND dialog) {
    if (g_isHelperProcess) {
        const FontPickerHelper::Result result =
            g_pendingDialogResult == FontPickerHelper::Result::Pending
                ? FontPickerHelper::Result::Cancelled
                : g_pendingDialogResult;
        Finish(result,
               result == FontPickerHelper::Result::Chosen ? &g_dialogResult : nullptr);
    }

    VERIFY(RemoveWindowSubclass(dialog, ChooseFontDlgSubclass, kDlgSubclassId));
    FORWARD_WM_NCDESTROY(dialog, DefSubclassProc);
}

void OnChooseFontShowWindow(HWND dialog, BOOL show, UINT status) {
    FORWARD_WM_SHOWWINDOW(dialog, show, status, DefSubclassProc);
    if (show) {
        RefreshChooseFontSample(dialog);
        UpdateChooseFontApplyState(dialog);
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
// right after suppressOwnerActivate is set and its own SetForegroundWindow call is issued):
// restore/raise/activate the font dialog on its own UI thread, then clear
// suppressOwnerActivate -- this is the single place that clears it (see ActivateDialog),
// deliberately deferred to here rather than done immediately after that SetForegroundWindow
// call returns. That earlier point is too soon: cross-process activation isn't necessarily
// fully delivered/dispatched on this thread by the time it returns, so clearing it there could
// let a WM_ACTIVATE that's really just this same activation reach OnChooseFontActivate
// unsuppressed. This message is posted strictly after that SetForegroundWindow call, so by the
// time it is processed, any WM_ACTIVATE it generated has necessarily already been dispatched.
void OnActivateChooseFont(HWND dialog) {
    if (IsIconic(dialog)) ShowWindow(dialog, SW_RESTORE);
    SetActiveWindow(dialog);
    BringWindowToTop(dialog);
    SetForegroundWindow(dialog);
    // Slot Preferences in immediately behind this dialog now, from this (helper) process's own
    // thread, while it genuinely holds the foreground from the SetForegroundWindow call just
    // above. SetWindowPos's z-order change only takes global effect -- rising above whatever
    // window is truly foreground -- when made by a thread that already is the foreground one;
    // called from the main process instead (before handing activation over), the same call
    // silently fails to rise above other apps despite reporting success.
    if (g_exchange && IsWindow(g_exchange->owner))
        SetWindowPos(g_exchange->owner, dialog, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (g_exchange) g_exchange->suppressOwnerActivate = FALSE;
}

// WM_ACTIVATE: the font dialog is a top-level window with no owner link to Preferences (see
// OnChooseFontInitDialog's comment), so the two only come forward together if told to. When
// the OS itself activates this dialog -- Alt+Tab, its own taskbar entry, a direct click, all
// of which bypass FontPickerHelper::ActivateDialog -- tell the owner (Preferences) to
// foreground itself too, rather than leaving it behind other apps. Skipped while
// suppressOwnerActivate is set, i.e. while this very activation is a side effect of
// ActivateDialog/OnActivateChooseFont, to avoid an endless cross-process activation loop.
void OnChooseFontActivate(HWND /*dialog*/, UINT state, HWND /*other*/, BOOL /*minimized*/) {
    if (state == WA_INACTIVE || !g_exchange || g_exchange->suppressOwnerActivate) return;
    if (!IsWindow(g_exchange->owner)) return;
    WinAPI::Window::AllowSetForeground(g_exchange->owner);
    PostMessage(g_exchange->owner, FontPickerHelper::kActivateOwnerMessage, 0, 0);
}

UINT_PTR OnChooseFontHookCommand(HWND dialog, int id, HWND /*control*/,
                                 UINT notification) {
    if (id == psh3 && notification == BN_CLICKED) {
        ApplyChooseFontSelection(dialog);
        return TRUE;
    }
    return FALSE;
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
        HANDLE_MSG(hwnd, WM_ACTIVATE, OnChooseFontActivate);
        HANDLE_MSG(hwnd, WM_FONT_PICKER_ACTIVATE, OnActivateChooseFont);
        default: return DefSubclassProc(hwnd, msg, wParam, lParam);
    }
}

BOOL OnChooseFontInitDialog(HWND dialog, HWND /*focus*/, LPARAM chooseFontParam) {
    SetDialog(dialog);

    // The dialog is owned by a hidden helper window, so center it over Preferences.
    if (IsWindow(g_parentDialog)) {
        RECT ownerRect;
        RECT dialogRect;
        if (GetWindowRect(g_parentDialog, &ownerRect) &&
            GetWindowRect(dialog, &dialogRect)) {
            const int width = dialogRect.right - dialogRect.left;
            const int height = dialogRect.bottom - dialogRect.top;
            int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
            int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;

            MONITORINFO monitor;
            monitor.cbSize = sizeof(monitor);
            if (GetMonitorInfo(
                    MonitorFromWindow(g_parentDialog, MONITOR_DEFAULTTONEAREST), &monitor)) {
                if (x < monitor.rcWork.left) x = monitor.rcWork.left;
                if (y < monitor.rcWork.top) y = monitor.rcWork.top;
                if (x + width > monitor.rcWork.right) x = monitor.rcWork.right - width;
                if (y + height > monitor.rcWork.bottom) y = monitor.rcWork.bottom - height;
            }
            SetWindowPos(dialog, nullptr, x, y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
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
                                                 const LOGFONT& fallback,
                                                 LOGFONT& selected) {
    g_inst = inst;
    g_parentDialog = owner;
    g_resetToFallback = initialDefault;
    MoveMemory(&g_lastAppliedFont, &initial, sizeof(g_lastAppliedFont));
    g_lastAppliedDefault = initialDefault;
    g_applySucceeded = false;
    g_pendingDialogResult = FontPickerHelper::Result::Pending;
    ZeroMemory(&g_dialogResult, sizeof(g_dialogResult));
    MoveMemory(&g_fallbackFont, &fallback, sizeof(g_fallbackFont));

    const HWND helperOwner = CreateWindowEx(
        WS_EX_TOOLWINDOW, TEXT("STATIC"), TEXT(""), WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, g_inst, nullptr);
    if (!helperOwner) return FontPickerHelper::Result::Failed;

    LOGFONT font;
    MoveMemory(&font, &initial, sizeof(font));

    CHOOSEFONT chooseFont;
    ZeroMemory(&chooseFont, sizeof(chooseFont));
    chooseFont.lStructSize = sizeof(chooseFont);
    chooseFont.hwndOwner = helperOwner;
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

    SetDialog(nullptr);
    g_parentDialog = nullptr;
    WinAPI::GdiObject::Delete(g_sampleFont);
    VERIFY(DestroyWindow(helperOwner));

    if (!chosen)
        return error == 0
            ? FontPickerHelper::Result::Cancelled
            : FontPickerHelper::Result::Failed;

    MoveMemory(&selected, &font, sizeof(selected));
    return g_resetToFallback
        ? FontPickerHelper::Result::Default
        : FontPickerHelper::Result::Chosen;
}

[[nodiscard]] WaitResult WaitForHelper(HANDLE process) {
    bool repostQuit = false;
    int quitCode = 0;

    for (;;) {
        const DWORD wait = MsgWaitForMultipleObjects(1, &process, FALSE, INFINITE, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0) break;
        if (wait != WAIT_OBJECT_0 + 1) {
            VERIFY(TerminateProcess(process, ERROR_GEN_FAILURE));
            VERIFY(WaitForSingleObject(process, INFINITE) == WAIT_OBJECT_0);
            return WaitResult::Failed;
        }

        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                if (!repostQuit) {
                    repostQuit = true;
                    quitCode = static_cast<int>(msg.wParam);
                    VERIFY(TerminateProcess(process, ERROR_CANCELLED));
                }
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    if (repostQuit) PostQuitMessage(quitCode);
    return repostQuit ? WaitResult::Quitting : WaitResult::Completed;
}

void ShowError(HINSTANCE inst, HWND owner) {
    TCHAR text[96], caption[32];
    MessageBox(owner, LoadStr(inst, IDS_FONT_PICKER_ERROR, text),
               LoadStr(inst, IDS_APP_NAME, caption), MB_OK | MB_ICONERROR);
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
        g_isHelperProcess = true;
        g_exchange = exchange;

        Result result = Result::Failed;
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(com)) {
            if (WinAPI::CommonControls::Init(ICC_LINK_CLASS)) {
                LOGFONT selected;
                ZeroMemory(&selected, sizeof(selected));
                result = ShowDialog(
                    inst, exchange->owner, exchange->font,
                    exchange->initialDefault != 0, exchange->fallback, selected);
                if (result == Result::Chosen)
                    MoveMemory(&exchange->font, &selected, sizeof(exchange->font));
            }
            CoUninitialize();
        }
        exchange->status = static_cast<LONG>(result);

        g_exchange = nullptr;
        g_isHelperProcess = false;
    }

    VERIFY(UnmapViewOfFile(exchange));
    VERIFY(CloseHandle(mapping));
    return true;
}

Result Open(HINSTANCE inst, HWND owner, const LOGFONT& initial,
            bool initialDefault, const LOGFONT& fallback, LOGFONT& selected) {
    TCHAR mappingName[128];
    const LONG serial = ++g_exchangeSerial;
    wsprintf(mappingName, TEXT("Local\\WinNumTipFontPicker") APP_GUID TEXT("-%lu-%ld"),
             GetCurrentProcessId(), serial);

    const HANDLE mapping = CreateFileMapping(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                             0, sizeof(Exchange), mappingName);
    if (!mapping || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mapping) CloseHandle(mapping);
        ShowError(inst, owner);
        return Result::Failed;
    }

    Exchange* const exchange = static_cast<Exchange*>(
        MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Exchange)));
    if (!exchange) {
        CloseHandle(mapping);
        ShowError(inst, owner);
        return Result::Failed;
    }

    ZeroMemory(exchange, sizeof(*exchange));
    exchange->cbSize = sizeof(*exchange);
    exchange->version = kExchangeVersion;
    exchange->owner = owner;
    MoveMemory(&exchange->font, &initial, sizeof(exchange->font));
    MoveMemory(&exchange->fallback, &fallback, sizeof(exchange->fallback));
    exchange->initialDefault = initialDefault ? TRUE : FALSE;

    TCHAR exe[MAX_PATH];
    const DWORD exeLength = GetModuleFileName(nullptr, exe, ARRAYSIZE(exe));
    TCHAR commandLine[MAX_PATH + ARRAYSIZE(mappingName) + 32];
    if (exeLength == 0 || exeLength >= ARRAYSIZE(exe)) {
        VERIFY(UnmapViewOfFile(exchange));
        VERIFY(CloseHandle(mapping));
        ShowError(inst, owner);
        return Result::Failed;
    }
    wsprintf(commandLine, TEXT("\"%s\" %s \"%s\""), exe, kHelperSwitch, mappingName);

    STARTUPINFO si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    g_exchange = exchange;
    (void)EnableWindow(owner, FALSE);
    const BOOL started = CreateProcess(exe, commandLine, nullptr, nullptr, FALSE, 0,
                                       nullptr, nullptr, &si, &pi);
    if (!started) {
        (void)EnableWindow(owner, TRUE);
        g_exchange = nullptr;
        VERIFY(UnmapViewOfFile(exchange));
        VERIFY(CloseHandle(mapping));
        ShowError(inst, owner);
        return Result::Failed;
    }

    VERIFY(CloseHandle(pi.hThread));
    AllowSetForegroundWindow(pi.dwProcessId);
    const WaitResult waitResult = WaitForHelper(pi.hProcess);
    if (IsWindow(owner)) {
        (void)EnableWindow(owner, TRUE);
        if (waitResult != WaitResult::Quitting) {
            SetActiveWindow(owner);
            SetForegroundWindow(owner);
        }
    }
    VERIFY(CloseHandle(pi.hProcess));
    g_exchange = nullptr;

    Result result = static_cast<Result>(exchange->status);
    bool showError = waitResult == WaitResult::Failed;
    if (waitResult == WaitResult::Quitting) {
        result = Result::Cancelled;
    } else if (waitResult == WaitResult::Completed) {
        if (result == Result::Chosen)
            MoveMemory(&selected, &exchange->font, sizeof(selected));
        else if (result == Result::Failed || result == Result::Pending)
            showError = true;
    }

    VERIFY(UnmapViewOfFile(exchange));
    VERIFY(CloseHandle(mapping));

    if (showError) {
        ShowError(inst, owner);
        return Result::Failed;
    }
    return result;
}

bool ReadAppliedFont(LOGFONT& selected) {
    if (!g_exchange || g_isHelperProcess) return false;
    MoveMemory(&selected, &g_exchange->font, sizeof(selected));
    return selected.lfFaceName[0] != 0;
}

// Foreground the still-open font dialog (from the main process) and ask it to fully
// reactivate itself on its own UI thread (OnActivateChooseFont, via the posted message
// below). suppressOwnerActivate brackets the direct SetForegroundWindow call too: it is a
// cross-process, same-thread-blocking call into the helper, so the WM_ACTIVATE it raises
// there must not be echoed back as a kActivateOwnerMessage while we are the one driving it.
bool ActivateDialog() {
    const HWND active = ActiveDialog();
    if (!active) return false;

    // Skip if already foreground: this is called from several overlapping paths (Preferences'
    // own WM_ACTIVATE handler, PreferencesDialog::ActivateDialog's explicit follow-up call,
    // ...), and a tray-icon click/double-click can drive several of them within milliseconds
    // of each other.
    if (GetForegroundWindow() == active) return true;

    WinAPI::Window::AllowSetForeground(active);

    // Set here, but deliberately NOT cleared here -- see OnActivateChooseFont, which clears
    // it once the dialog has finished (re)activating on its own UI thread.
    if (g_exchange) g_exchange->suppressOwnerActivate = TRUE;
    SetForegroundWindow(active);

    const bool posted = PostMessage(active, WM_FONT_PICKER_ACTIVATE, 0, 0) != FALSE;
    // If the follow-up message couldn't be queued, OnActivateChooseFont will never run to
    // clear the flag we just set -- clear it here instead so a later genuine activation is
    // never suppressed forever.
    if (!posted && g_exchange) g_exchange->suppressOwnerActivate = FALSE;
    return posted;
}

HWND ActiveDialog() {
    const HWND active = g_exchange ? g_exchange->dialog : g_dialog;
    return active && IsWindow(active) ? active : nullptr;
}

} // namespace FontPickerHelper
