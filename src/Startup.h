#pragma once

// User-level "start with Windows" registration: a value named after the app under the
// current user's startup registry key (HKCU\Software\Microsoft\Windows\CurrentVersion\Run)
// holding the executable's quoted full path, so Windows launches the app at sign-in. No
// elevation is needed; the persisted on/off preference itself lives in Preferences.
namespace Startup {

// Reconcile the HKCU Run entry with the "start with Windows" preference. When 'enabled',
// ensure the entry exists and points at this executable, creating or correcting it when it
// is missing or holds a different path (e.g. the portable app was moved); an entry that
// already matches is left untouched. When disabled, remove the entry (a no-op if it does
// not exist). Called at startup while the preference is set (self-heal) and whenever the
// Preferences dialog applies the setting.
void Sync(bool enabled);

} // namespace Startup
