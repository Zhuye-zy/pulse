// startup_launch.cpp — see startup_launch.h.
#include "startup_launch.h"
#include <windows.h>
#include <cwchar>

namespace pulse::app {

std::wstring StartupCommandLine(const std::wstring& exe) {
    if (exe.empty()) return {};
    return L"\"" + exe + L"\" " + kStartupArgument;
}

bool StartupCommandNeedsRepair(const std::wstring& value, const std::wstring& exe) {
    if (value.empty() || exe.empty()) return false;
    const std::wstring legacy = L"\"" + exe + L"\"";
    return CompareStringOrdinal(value.c_str(), static_cast<int>(value.size()),
                                legacy.c_str(), static_cast<int>(legacy.size()), TRUE) == CSTR_EQUAL;
}

bool HasStartupArgument(int argc, const wchar_t* const* argv) {
    for (int i = 1; i < argc && argv; ++i)
        if (argv[i] && wcscmp(argv[i], kStartupArgument) == 0) return true;
    return false;
}

bool StartsHiddenInTray(bool startup_launch, bool start_in_tray) noexcept {
    return startup_launch && start_in_tray;
}

} // namespace pulse::app
