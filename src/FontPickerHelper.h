#pragma once

// Process/IPC support for running the system font dialog in a short-lived WinNumTip
// helper process, so font-engine caches are released when the dialog closes.
namespace FontPickerHelper {

enum class Result : LONG {
    Pending,
    Cancelled,
    Chosen,
    Default,
    Failed,
};

// Sent synchronously to the Preferences owner when Apply is clicked in the still-open
// font dialog. wParam is Result::Chosen or Result::Default.
constexpr UINT kApplySelectionMessage = WM_APP + 0x101;

// If this process was launched as the font-dialog helper, run it and report that normal
// application startup must stop. Call before enforcing the main application's mutex.
[[nodiscard]] bool RunIfRequested(HINSTANCE inst);

// Launch the helper and wait while keeping the owner responsive. Returns the dialog result;
// 'initialDefault' preserves the semantic distinction between the fallback face and an
// explicitly selected matching font; 'selected' is populated only for Chosen.
[[nodiscard]] Result Open(HINSTANCE inst, HWND owner, const LOGFONT& initial,
                            bool initialDefault, const LOGFONT& fallback,
                            LOGFONT& selected);

// Read the font published immediately before kApplySelectionMessage. Valid only while
// Open is waiting for the helper; Default applications do not need a LOGFONT.
[[nodiscard]] bool ReadAppliedFont(LOGFONT& selected);

// Resident-process activation support while the helper dialog is open.
[[nodiscard]] bool ActivateDialog();
[[nodiscard]] HWND ActiveDialog();

} // namespace FontPickerHelper
