// default_file_manager.cpp — see default_file_manager.h.
#include "default_file_manager.h"

#include "app_prefs.h"
#include "shell_integration_registry.h"
#include "../common/localization.h"

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <cwctype>

namespace pulse::app {
namespace {

std::wstring ModulePath() {
    std::wstring path(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size >= path.size()) return {};
    path.resize(size);
    return path;
}
bool EqualsNoCase(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() &&
        CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                             static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

} // namespace

DefaultManagerState DefaultFileManagerState(const AppPrefs& prefs) {
    const int on = (prefs.open_folders_in_pulse ? 1 : 0) + (prefs.take_over_win_e ? 1 : 0) +
                   (prefs.take_over_this_pc ? 1 : 0);
    if (on == 0) return DefaultManagerState::Off;
    return on == 3 ? DefaultManagerState::Full : DefaultManagerState::Partial;
}

std::wstring DefaultFileManagerSummary(const AppPrefs& prefs) {
    using I = l10n::StringId;
    if (DefaultFileManagerState(prefs) != DefaultManagerState::Partial)
        return l10n::Get(I::SettingsDefaultManagerDesc);
    std::wstring missing;
    auto add = [&](const std::wstring& part) {
        if (!missing.empty()) missing += l10n::Pick(L"、", L", ");
        missing += part;
    };
    if (!prefs.open_folders_in_pulse) add(l10n::Get(I::SettingsTakeoverFolders));
    if (!prefs.take_over_this_pc) add(l10n::Get(I::ThisPc));
    if (!prefs.take_over_win_e) add(L"Win+E");
    const std::wstring& pattern = l10n::Get(I::SettingsDefaultManagerPartial);
    const size_t at = pattern.find(L"%s");
    if (at == std::wstring::npos) return pattern;
    return pattern.substr(0, at) + missing + pattern.substr(at + 2);
}

bool ApplyDefaultFileManager(AppPrefs& prefs, bool on) {
    bool ok = prefs.ApplyFolderOpen(on);
    ok = prefs.ApplyWinE(on) && ok;
    ok = ApplyThisPcOpen(prefs, on) && ok;
    return ok;
}

bool ReadThisPcOpen(const std::wstring& exe) {
    return ReadShellIntegration(ShellIntegrationKind::ThisPc, exe);
}

bool ApplyThisPcOpen(AppPrefs& prefs, bool on) {
    if (!prefs.persist) { prefs.take_over_this_pc = on; return true; }
    const std::wstring exe = ModulePath();
    const bool ok = ApplyShellIntegration(ShellIntegrationKind::ThisPc, exe, on);
    prefs.take_over_this_pc = ReadThisPcOpen(exe);
    prefs.integration_residual = prefs.ReadIntegrationResidual();
    prefs.integration_incomplete = prefs.ReadIntegrationIncomplete();
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok && prefs.take_over_this_pc == on;
}
bool IsThisPcArgument(std::wstring_view raw) {
    while (!raw.empty() && (raw.front() == L'"' || iswspace(raw.front()))) raw.remove_prefix(1);
    while (!raw.empty() && (raw.back() == L'"' || raw.back() == L'\\' || iswspace(raw.back())))
        raw.remove_suffix(1);
    constexpr std::wstring_view kShell = L"shell:";
    if (raw.size() > kShell.size() && EqualsNoCase(raw.substr(0, kShell.size()), kShell))
        raw.remove_prefix(kShell.size());
    return EqualsNoCase(raw, kThisPcParsingName) || EqualsNoCase(raw, L"MyComputerFolder");
}

} // namespace pulse::app
