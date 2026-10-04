#pragma once
#include <cwctype>
#include <string>
#include <string_view>

namespace pulse {

inline bool IsRenameFilename(const std::wstring& name) {
    if (name.empty() || name == L"." || name == L".." ||
        name.back() == L'.' || name.back() == L' ') return false;
    for (const auto c : name)
        if (c < 32 || std::wstring_view(L"\\/:*?\"<>|").find(c) != std::wstring_view::npos) return false;
    std::wstring stem = name.substr(0, name.find(L'.'));
    while (!stem.empty() && stem.back() == L' ') stem.pop_back();
    for (auto& c : stem) c = static_cast<wchar_t>(towupper(c));
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
        stem == L"CONIN$" || stem == L"CONOUT$") return false;
    if (stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) &&
        ((stem[3] >= L'1' && stem[3] <= L'9') || stem[3] == L'\u00b9' || stem[3] == L'\u00b2' || stem[3] == L'\u00b3')) return false;
    return true;
}

} // namespace pulse
