#include "address_bar_command.h"
#include <windows.h>
#include <shlobj.h>
#include <cwctype>

namespace pulse::app {
namespace {

bool IsSpace(wchar_t c) {
    return c == L' ' || c == L'\t' || c == 0x3000;
}

std::wstring_view Trim(std::wstring_view text) {
    while (!text.empty() && IsSpace(text.front())) text.remove_prefix(1);
    while (!text.empty() && IsSpace(text.back())) text.remove_suffix(1);
    return text;
}

bool EqualsNoCase(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    }
    return true;
}

struct ProgramName {
    const wchar_t* name;
    AddressBarProgram program;
};

constexpr ProgramName kPrograms[] = {
    { L"cmd",        AddressBarProgram::Cmd },
    { L"powershell", AddressBarProgram::PowerShell },
    { L"pwsh",       AddressBarProgram::Pwsh },
    { L"wt",         AddressBarProgram::WindowsTerminal },
};

} // namespace

AddressBarCommand ParseAddressBarCommand(std::wstring_view text) {
    AddressBarCommand result;
    text = Trim(text);
    size_t name_end = 0;
    while (name_end < text.size() && !IsSpace(text[name_end])) ++name_end;
    std::wstring_view name = text.substr(0, name_end);
    if (name.size() > 4 && EqualsNoCase(name.substr(name.size() - 4), L".exe"))
        name.remove_suffix(4);
    for (const auto& candidate : kPrograms) {
        if (EqualsNoCase(name, candidate.name)) {
            result.program = candidate.program;
            result.args = std::wstring(Trim(text.substr(name_end)));
            break;
        }
    }
    return result;
}

namespace {

bool StartsWithNoCase(std::wstring_view text, std::wstring_view prefix) {
    return text.size() >= prefix.size() && EqualsNoCase(text.substr(0, prefix.size()), prefix);
}

std::wstring ExpandVariables(const std::wstring& text) {
    const DWORD needed = ExpandEnvironmentStringsW(text.c_str(), nullptr, 0);
    if (needed == 0) return text;
    std::wstring out(needed, L'\0');
    const DWORD written = ExpandEnvironmentStringsW(text.c_str(), out.data(), needed);
    if (written == 0 || written > needed) return text;
    out.resize(written - 1);
    return out;
}

// Filesystem folder behind a "shell:<name>" moniker, or empty when the name is
// unknown or the folder is virtual.
std::wstring ShellFolderPath(const std::wstring& moniker) {
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(moniker.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl)
        return {};
    std::wstring path(32768, L'\0');
    const bool ok = SHGetPathFromIDListEx(pidl, path.data(), static_cast<DWORD>(path.size()),
                                          GPFIDL_DEFAULT) != FALSE;
    CoTaskMemFree(pidl);
    if (!ok) return {};
    path.resize(wcslen(path.c_str()));
    return path;
}

} // namespace

AddressShortcut ResolveAddressShortcut(std::wstring_view text) {
    AddressShortcut result;
    text = Trim(text);
    while (!text.empty() && text.front() == L'"') text.remove_prefix(1);
    while (!text.empty() && text.back() == L'"') text.remove_suffix(1);
    text = Trim(text);
    std::wstring work(text);
    result.path = work;
    if (work.find(L'%') != std::wstring::npos) {
        std::wstring expanded = ExpandVariables(work);
        if (expanded != work) {
            work = std::move(expanded);
            result.resolved = true;
            result.path = work;
        }
    }
    constexpr std::wstring_view kShell = L"shell:";
    if (!StartsWithNoCase(work, kShell)) return result;

    // "shell:<name>[\rest]": only the first segment names the folder.
    const size_t name_end = work.find_first_of(L"\\/", kShell.size());
    const std::wstring head = work.substr(0, name_end);
    std::wstring rest = name_end == std::wstring::npos ? std::wstring() : work.substr(name_end);
    for (auto& c : rest) if (c == L'/') c = L'\\';
    while (!rest.empty() && rest.back() == L'\\') rest.pop_back();
    const std::wstring_view name = std::wstring_view(head).substr(kShell.size());

    if (EqualsNoCase(name, L"MyComputerFolder") ||
        EqualsNoCase(name, L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}")) {
        result.resolved = rest.empty();
        result.path = rest.empty() ? std::wstring() : work;
        return result;
    }
    if (EqualsNoCase(name, L"RecycleBinFolder") ||
        EqualsNoCase(name, L"::{645FF040-5081-101B-9F08-00AA002F954E}")) {
        result.resolved = rest.empty();
        result.path = rest.empty() ? std::wstring(L"pulse:recycle") : work;
        return result;
    }
    const std::wstring folder = name.empty() ? std::wstring() : ShellFolderPath(head);
    if (folder.empty()) {
        result.resolved = false;
        result.path = work;
        return result;
    }
    result.resolved = true;
    result.path = folder;
    if (!rest.empty()) {
        if (result.path.back() == L'\\') result.path.pop_back();
        result.path += rest;
    }
    return result;
}

const wchar_t* AddressBarProgramExe(AddressBarProgram program) {
    switch (program) {
    case AddressBarProgram::Cmd:             return L"cmd.exe";
    case AddressBarProgram::PowerShell:      return L"powershell.exe";
    case AddressBarProgram::Pwsh:            return L"pwsh.exe";
    case AddressBarProgram::WindowsTerminal: return L"wt.exe";
    case AddressBarProgram::None:            break;
    }
    return nullptr;
}

} // namespace pulse::app
