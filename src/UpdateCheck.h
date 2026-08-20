#pragma once

// Release update check: asks github.com whether a build newer than this one has been
// published, off the UI thread, so the About dialog can offer an "Update" link next to
// the version it shows (see IDC_ABOUT_UPDATE).
namespace UpdateCheck {

// Start(notify): (re)run the check in the background and post WM_UPDATECHECK to 'notify'
// when it finishes; the result is then read with IsAvailable(). Returns immediately -- the
// network round trip never runs on the UI thread. At most one request is ever in flight: a
// call made while one is running just re-targets the notification at 'notify', so
// re-opening About while the previous check is still running does not start a second one.
// And once a check has found an update, no further request is made at all -- the answer
// cannot change back, so IsAvailable() keeps reporting it for the rest of the session.
void Start(HWND notify);

// Stop(notify): drop 'notify' as the notification target (call when it is being destroyed),
// so a check still in flight cannot post to a dead window. A no-op when the current target
// is a different window -- i.e. when a newer About dialog has already taken over.
void Stop(HWND notify);

// IsAvailable(): true when the last completed check found a release newer than this build.
// False until the first check finishes, and whenever it failed (offline, blocked, ...) --
// a failure is never reported to the user, it just leaves the link hidden.
[[nodiscard]] bool IsAvailable();

} // namespace UpdateCheck
