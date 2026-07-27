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

// If this process was launched as the font-dialog helper, run it and report that normal
// application startup must stop. Call before enforcing the main application's mutex.
[[nodiscard]] bool RunIfRequested(HINSTANCE inst);

// Launch the helper and wait while keeping the owner responsive. Returns the dialog result;
// 'selected' is populated only for Chosen.
[[nodiscard]] Result Open(HINSTANCE inst, HWND owner, const LOGFONT& initial,
                            const LOGFONT& fallback, LOGFONT& selected);

// Resident-process activation support while the helper dialog is open.
[[nodiscard]] bool ActivateDialog();
[[nodiscard]] HWND ActiveDialog();

} // namespace FontPickerHelper
