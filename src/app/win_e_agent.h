#pragma once

#include <string>

namespace pulse::app::win_e_agent {

// HKCU Run value is the source of truth. The agent is a separate, windowless
// pulse.exe instance so closing the file manager does not disable Win+E.
bool Enabled(const std::wstring& exe);
bool SetEnabled(const std::wstring& exe, bool enabled);
bool EnsureRunning(const std::wstring& exe);
int Run();

} // namespace pulse::app::win_e_agent
