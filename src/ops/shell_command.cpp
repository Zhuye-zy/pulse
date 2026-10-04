#include "shell_command.h"
#include "../common/path_utils.h"
#include <vector>

namespace pulse::ops {
namespace {
bool Space(wchar_t c) { return c == L' ' || c == L'\t'; }
bool ResolveExecutable(std::wstring candidate, std::wstring& resolved) {
    if (candidate.empty() || candidate.find(L'"') != std::wstring::npos) return false;
    std::vector<wchar_t> buffer(32768);
    // Resolve against the caller's search rules, before ShellExecute applies
    // the selected item's working directory to relative executable names.
    DWORD length = SearchPathW(nullptr, candidate.c_str(), L".exe", static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (!length || length >= buffer.size()) return false;
    resolved.assign(buffer.data(), length);
    const DWORD attributes = GetFileAttributesW(resolved.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}
}
bool SplitShellCommand(std::wstring_view command, ShellCommandParts& parts) {
    parts = {};
    size_t start = 0;
    while (start < command.size() && Space(command[start])) ++start;
    if (start == command.size() || command.find_first_of(L"\r\n\0", 0, 3) != std::wstring_view::npos) return false;
    size_t end = start;
    if (command[start] == L'"') {
        end = command.find(L'"', start + 1);
        if (end == std::wstring_view::npos || end == start + 1 ||
            (end + 1 < command.size() && !Space(command[end + 1]))) return false;
        if (!ResolveExecutable(std::wstring(command.substr(start + 1, end - start - 1)), parts.executable)) return false;
        ++end;
    } else {
        // CreateProcess tries each whitespace-delimited prefix for unquoted
        // paths containing spaces. Use the same first-existing-image order.
        for (; end <= command.size(); ++end) {
            if (end < command.size() && !Space(command[end])) continue;
            if (ResolveExecutable(std::wstring(command.substr(start, end - start)), parts.executable)) break;
        }
        if (parts.executable.empty()) return false;
    }
    while (end < command.size() && Space(command[end])) ++end;
    parts.arguments.assign(command.substr(end));
    return true;
}
ShellCommandResult LaunchShellCommand(const std::wstring& command, const std::wstring& directory,
    HWND owner, const ShellCommandApi& api) {
    if (command.empty()) return {ERROR_INVALID_PARAMETER, ERROR_INVALID_PARAMETER, false};
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(0);
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION process{};
    const auto working = pulse::path::StripExtendedPathPrefix(directory);
    const wchar_t* working_ptr = working.empty() ? nullptr : working.c_str();
    if (api.create_process(nullptr, buffer.data(), nullptr, nullptr, FALSE, 0, nullptr, working_ptr, &startup, &process)) {
        if (process.hThread) CloseHandle(process.hThread);
        if (process.hProcess) CloseHandle(process.hProcess);
        return {};
    }
    const DWORD error = GetLastError();
    if (error != ERROR_ELEVATION_REQUIRED) return {error, error, false};
    ShellCommandParts parts;
    if (!SplitShellCommand(command, parts)) return {ERROR_BAD_PATHNAME, error, false};
    SHELLEXECUTEINFOW execute{sizeof(execute)};
    execute.fMask = SEE_MASK_NOASYNC | SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    execute.hwnd = owner;
    execute.lpVerb = L"runas";
    execute.lpFile = parts.executable.c_str();
    execute.lpParameters = parts.arguments.empty() ? nullptr : parts.arguments.c_str();
    execute.lpDirectory = working_ptr;
    execute.nShow = SW_SHOWNORMAL;
    if (!api.shell_execute(&execute)) return {GetLastError(), error, true};
    if (execute.hProcess) CloseHandle(execute.hProcess);
    return {ERROR_SUCCESS, error, true};
}
}
