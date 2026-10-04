// default_file_manager.h — "设为默认文件管理器" (B站 #1): one switch over the
// folder/drive, Win+E and This PC takeovers, plus the This PC open verb.
#pragma once

#include <string>
#include <string_view>

namespace pulse::app {

struct AppPrefs;

// Shell parsing name of This PC; also the argument the This PC verb passes.
inline constexpr wchar_t kThisPcParsingName[] = L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}";

enum class DefaultManagerState { Off, Partial, Full };

// From the three takeover flags in prefs (registry is their source of truth).
DefaultManagerState DefaultFileManagerState(const AppPrefs& prefs);

// Settings text: the full description, or which parts File Explorer still opens.
std::wstring DefaultFileManagerSummary(const AppPrefs& prefs);

// Turns every takeover on (filling in the missing ones) or off (restoring
// File Explorer; other programs' registrations stay untouched).
bool ApplyDefaultFileManager(AppPrefs& prefs, bool on);

// HKCU CLSID\{20D04FE0-...}\shell\open: double-clicking This PC opens Pulse.
bool ReadThisPcOpen(const std::wstring& exe);
bool ApplyThisPcOpen(AppPrefs& prefs, bool on);

// Launch arguments that mean This PC: its parsing name (with or without the
// "shell:" prefix) and shell:MyComputerFolder. Quotes and case are ignored.
bool IsThisPcArgument(std::wstring_view raw);

} // namespace pulse::app
