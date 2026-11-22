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

// Posted to the Preferences owner once the helper process has exited. wParam identifies the
// helper session; pass it to Complete.
constexpr UINT kClosedMessage = WM_APP + 0x102;

// If this process was launched as the font-dialog helper, run it and report that normal
// application startup must stop. Call before enforcing the main application's mutex.
[[nodiscard]] bool RunIfRequested(HINSTANCE inst);

// Launch the helper and return at once: the font dialog is modeless, so 'owner' stays
// enabled and usable while it is open. When the helper exits, kClosedMessage is posted to
// 'owner'. Returns false when the helper could not be started (the error has already been
// shown) or a font dialog is already open (it is brought forward instead).
// 'initialDefault' preserves the semantic distinction between the fallback face and an
// explicitly selected matching font. 'applied'/'appliedDefault' describe the font the owner
// has actually persisted, which the dialog's Apply button compares its live selection
// against. That is not necessarily the initial selection: closing the dialog with OK hands
// the choice back to the owner without persisting it, so reopening starts on a selection
// that Apply still has work to do for.
[[nodiscard]] bool Open(HINSTANCE inst, HWND owner, const LOGFONT& initial,
                        bool initialDefault, const LOGFONT& applied,
                        bool appliedDefault, const LOGFONT& fallback);

// Collect the dialog result of the helper session 'session' (kClosedMessage's wParam) and
// release it. Returns Pending for a stale notification (the session was already closed);
// 'selected' is populated only for Chosen.
[[nodiscard]] Result Complete(WPARAM session, LOGFONT& selected);

// Close the font dialog, if open, discarding any selection not yet applied.
void Close();

// Hand the owner's newly persisted font to the open font dialog, if any, as the baseline
// its Apply button measures the live selection against (see Open's 'applied').
void SetApplied(const LOGFONT& applied, bool appliedDefault);

// Read the font published immediately before kApplySelectionMessage. Valid only while
// the helper session is open; Default applications do not need a LOGFONT.
[[nodiscard]] bool ReadAppliedFont(LOGFONT& selected);

// Resident-process activation support while the helper dialog is open.
[[nodiscard]] bool ActivateDialog();
[[nodiscard]] HWND ActiveDialog();

} // namespace FontPickerHelper
