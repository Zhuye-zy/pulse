// startup_launch.h — Launch at sign-in, optionally straight into the tray (B站 #10).
//
// The HKCU Run value starts Pulse with --startup, so a sign-in launch can be
// told apart from the user opening Pulse; "start in the tray" only applies to
// the former. Older Run values without the flag are rewritten on load.
#pragma once
#include <string>

namespace pulse::app {

inline constexpr wchar_t kStartupArgument[] = L"--startup";

// "\"<exe>\" --startup"; empty when exe is empty.
std::wstring StartupCommandLine(const std::wstring& exe);
// True only for the pre-flag value written by older builds for this same exe
// ("\"<exe>\""). Values for another copy of Pulse are left alone.
bool StartupCommandNeedsRepair(const std::wstring& value, const std::wstring& exe);
bool HasStartupArgument(int argc, const wchar_t* const* argv);
// The window stays hidden behind the tray icon at a sign-in launch when the
// user asked for it.
bool StartsHiddenInTray(bool startup_launch, bool start_in_tray) noexcept;

} // namespace pulse::app
