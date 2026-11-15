#pragma once

namespace Registry {

// Whether Windows uses the dark system (taskbar) mode. False if the setting is absent
// or cannot be read. Used on first run without an INI and by Reset to defaults.
[[nodiscard]] bool SystemUsesDarkTheme();

// Store the quoted executable command in the current user's Run entry, leaving a
// matching entry untouched. A null command removes the entry (best effort).
void SetStartupCommand(LPCTSTR command);

} // namespace Registry
