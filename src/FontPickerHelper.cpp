#include "stdafx.h"
#include "FontPickerHelper.h"
#include "resource.h"

// windowsx-style crackers for helper-only messages. The WM_SETTEXT alias preserves
// DefSubclassProc's result, which the standard HANDLE_WM_SETTEXT cracker discards.
#define WM_FONT_SAMPLE_SETTEXT WM_SETTEXT
#define HANDLE_WM_FONT_SAMPLE_SETTEXT(hwnd, wParam, lParam, fn) ((fn)((hwnd), reinterpret_cast<LPCTSTR>(lParam)))
#define WM_FONT_PICKER_ACTIVATE (WM_APP + 0x100)
#define HANDLE_WM_FONT_PICKER_ACTIVATE(hwnd, wParam, lParam, fn) ((fn)(hwnd), 0L)

namespace {

constexpr LPCTSTR kHelperSwitch = TEXT("--font-picker");
constexpr DWORD kExchangeVersion = 1;

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
    volatile LONG status;
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
HFONT g_sampleFont = nullptr;
FontPickerHelper::Result g_pendingDialogResult = FontPickerHelper::Result::Pending;
LOGFONT g_dialogResult = { 0 };
LOGFONT* g_chooseFontResult = nullptr;

constexpr LPCTSTR kFontTemplate = TEXT("FONTSELECTORDLG");
constexpr LPCTSTR kFontSample = TEXT("0123456789");
constexpr UINT_PTR kSampleSubclassId = 1;
constexpr UINT_PTR kDlgSubclassId = 2;

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
            DWORD ownerProcess = 0;
            GetWindowThreadProcessId(g_exchange->owner, &ownerProcess);
            if (ownerProcess) AllowSetForegroundWindow(ownerProcess);
            (void)EnableWindow(g_exchange->owner, TRUE);
            SetForegroundWindow(g_exchange->owner);
        }
    }
    TerminateProcess(GetCurrentProcess(), 0);
    ExitProcess(0);
}

LRESULT CALLBACK FontSampleSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR id, DWORD_PTR ref);

LRESULT OnFontSampleSetText(HWND hwnd, LPCTSTR /*text*/) {
    return DefSubclassProc(hwnd, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(kFontSample));
}

void OnFontSampleNcDestroy(HWND hwnd) {
    VERIFY(RemoveWindowSubclass(hwnd, FontSampleSubclassProc, kSampleSubclassId));
    FORWARD_WM_NCDESTROY(hwnd, DefSubclassProc);
}

// Keep comdlg32's Sample control text fixed to the badge digits.
LRESULT CALLBACK FontSampleSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR /*id*/, DWORD_PTR /*ref*/) {
    switch (msg) {
        HANDLE_MSG(hwnd, WM_FONT_SAMPLE_SETTEXT, OnFontSampleSetText);
        HANDLE_MSG(hwnd, WM_NCDESTROY, OnFontSampleNcDestroy);
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
    LOGFONT lf;
    MoveMemory(&lf, &selected, sizeof(lf));
    if (lf.lfFaceName[0] == 0) return;

    RECT rc;
    GetClientRect(sample, &rc);
    const int boxWidth = rc.right - rc.left;
    const int boxHeight = rc.bottom - rc.top;
    constexpr int kReferenceHeight = 100;
    constexpr int kFillPercent = 90;
    lf.lfWidth = 0;
    lf.lfHeight = -kReferenceHeight;
    if (const HFONT measure = CreateFontIndirect(&lf)) {
        const HDC dc = GetDC(sample);
        const HFONT previous = static_cast<HFONT>(SelectObject(dc, measure));
        SIZE extent = { 0, 0 };
        GetTextExtentPoint32(dc, kFontSample, lstrlen(kFontSample), &extent);
        SelectObject(dc, previous);
        ReleaseDC(sample, dc);
        DeleteObject(measure);

        int height = kReferenceHeight;
        if (extent.cx > 0 && boxWidth > 0) {
            const int widthHeight =
                MulDiv(kReferenceHeight, boxWidth * kFillPercent / 100, extent.cx);
            if (widthHeight < height) height = widthHeight;
        }
        if (extent.cy > 0 && boxHeight > 0) {
            const int boxHeightLimit =
                MulDiv(kReferenceHeight, boxHeight * kFillPercent / 100, extent.cy);
            if (boxHeightLimit < height) height = boxHeightLimit;
        }
        lf.lfHeight = -(height > 0 ? height : 1);
    }

    const HFONT font = CreateFontIndirect(&lf);
    if (!font) return;
    if (g_sampleFont) DeleteObject(g_sampleFont);
    g_sampleFont = font;
    SendMessage(sample, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    VERIFY(SetWindowText(sample, kFontSample));
}

// Repaint the ChooseFont Sample control in the currently selected font. With a custom
// template comdlg32 does not refresh it, so read and apply its live selection.
void RefreshChooseFontSample(HWND dialog) {
    const HWND sample = GetDlgItem(dialog, stc5);
    if (!sample) return;

    LOGFONT lf;
    ZeroMemory(&lf, sizeof(lf));
    SendMessage(dialog, WM_CHOOSEFONT_GETLOGFONT, 0, reinterpret_cast<LPARAM>(&lf));
    ApplyChooseFontSample(sample, lf);
}

// Drive the visible controls so comdlg32 rebuilds its internal LOGFONT for the fallback.
[[nodiscard]] bool ResetChooseFontToFallback(HWND dialog) {
    const HWND nameCombo = GetDlgItem(dialog, cmb1);
    const HWND styleCombo = GetDlgItem(dialog, cmb2);
    if (!nameCombo) return false;

    VERIFY(CheckDlgButton(dialog, chx1, BST_UNCHECKED));
    VERIFY(CheckDlgButton(dialog, chx2, BST_UNCHECKED));

    const int face = static_cast<int>(
        SendMessage(nameCombo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                    reinterpret_cast<LPARAM>(g_fallbackFont.lfFaceName)));
    if (face < 0) return false;
    SendMessage(nameCombo, CB_SETCURSEL, face, 0);
    FORWARD_WM_COMMAND(dialog, cmb1, nameCombo, CBN_SELCHANGE, SendMessage);

    if (styleCombo) {
        const int style = static_cast<int>(
            SendMessage(styleCombo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                        reinterpret_cast<LPARAM>(TEXT("Regular"))));
        if (style >= 0) {
            SendMessage(styleCombo, CB_SETCURSEL, style, 0);
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
        SendMessage(dialog, WM_CHOOSEFONT_GETLOGFONT, 0,
                    reinterpret_cast<LPARAM>(&g_dialogResult));
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
        (id == cmb1 || id == cmb2) && notification == CBN_SELCHANGE;
    const bool effectToggled =
        (id == chx1 || id == chx2) && notification == BN_CLICKED;
    if (comboChanged || effectToggled) {
        RefreshChooseFontSample(dialog);
        g_resetToFallback = false;
    }
}

LRESULT OnChooseFontNotify(HWND dialog, int idFrom, NMHDR* notification) {
    if (notification->idFrom == IDC_FONT_RESET_LINK &&
        (notification->code == NM_CLICK || notification->code == NM_RETURN)) {
        if (ResetChooseFontToFallback(dialog))
            g_resetToFallback = true;
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
    if (show) RefreshChooseFontSample(dialog);
    FORWARD_WM_SHOWWINDOW(dialog, show, status, DefSubclassProc);
}

void OnActivateChooseFont(HWND dialog) {
    if (IsIconic(dialog)) ShowWindow(dialog, SW_RESTORE);
    SetActiveWindow(dialog);
    BringWindowToTop(dialog);
    SetForegroundWindow(dialog);
}

LRESULT CALLBACK ChooseFontDlgSubclass(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                       UINT_PTR /*id*/, DWORD_PTR /*ref*/) {
    switch (msg) {
        HANDLE_MSG(hwnd, WM_COMMAND, OnChooseFontCommand);
        HANDLE_MSG(hwnd, WM_NOTIFY, OnChooseFontNotify);
        HANDLE_MSG(hwnd, WM_CLOSE, OnChooseFontClose);
        HANDLE_MSG(hwnd, WM_NCDESTROY, OnChooseFontNcDestroy);
        HANDLE_MSG(hwnd, WM_SHOWWINDOW, OnChooseFontShowWindow);
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

    if (const HWND nameCombo = GetDlgItem(dialog, cmb1))
        SetFocus(nameCombo);

    return FALSE;
}

UINT_PTR CALLBACK ChooseFontHook(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        HANDLE_MSG(hwnd, WM_INITDIALOG, OnChooseFontInitDialog);
        default: return 0;
    }
}

FontPickerHelper::Result ShowDialog(HINSTANCE inst, HWND owner,
                                    const LOGFONT& initial, const LOGFONT& fallback,
                                    LOGFONT& selected) {
    g_inst = inst;
    g_parentDialog = owner;
    g_resetToFallback = false;
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
    chooseFont.Flags = CF_INITTOLOGFONTSTRUCT | CF_EFFECTS | CF_SCREENFONTS |
                       CF_FORCEFONTEXIST | CF_ENABLEHOOK | CF_ENABLETEMPLATE;

    g_chooseFontResult = &font;
    const BOOL chosen = ChooseFont(&chooseFont);
    g_chooseFontResult = nullptr;
    const DWORD error = chosen ? 0 : CommDlgExtendedError();

    SetDialog(nullptr);
    g_parentDialog = nullptr;
    if (g_sampleFont) { DeleteObject(g_sampleFont); g_sampleFont = nullptr; }
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
            INITCOMMONCONTROLSEX icc;
            icc.dwSize = sizeof(icc);
            icc.dwICC = ICC_LINK_CLASS;
            if (InitCommonControlsEx(&icc)) {
                LOGFONT selected;
                ZeroMemory(&selected, sizeof(selected));
                result = ShowDialog(inst, exchange->owner, exchange->font,
                                    exchange->fallback, selected);
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
              const LOGFONT& fallback, LOGFONT& selected) {
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

bool ActivateDialog() {
    const HWND active = ActiveDialog();
    if (!active) return false;

    DWORD process = 0;
    GetWindowThreadProcessId(active, &process);
    if (process) AllowSetForegroundWindow(process);

    SetForegroundWindow(active);
    return PostMessage(active, WM_FONT_PICKER_ACTIVATE, 0, 0) != FALSE;
}

HWND ActiveDialog() {
    const HWND active = g_exchange ? g_exchange->dialog : g_dialog;
    return active && IsWindow(active) ? active : nullptr;
}

} // namespace FontPickerHelper
