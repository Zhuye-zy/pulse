#pragma once
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <string_view>

namespace pulse::ops {
struct ShellCommandParts { std::wstring executable, arguments; };
bool SplitShellCommand(std::wstring_view command, ShellCommandParts& parts);
struct ShellCommandResult {
    DWORD error = ERROR_SUCCESS;
    DWORD create_error = ERROR_SUCCESS;
    bool elevation_requested = false;
};
struct ShellCommandApi {
    decltype(&CreateProcessW) create_process = ::CreateProcessW;
    decltype(&ShellExecuteExW) shell_execute = ::ShellExecuteExW;
};
ShellCommandResult LaunchShellCommand(const std::wstring& command, const std::wstring& directory,
    HWND owner, const ShellCommandApi& api = {});
}
