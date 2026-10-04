#include "../ops/shell_command.h"
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace pulse::ops;
namespace {
DWORD create_error = ERROR_ELEVATION_REQUIRED, shell_error = ERROR_SUCCESS;
unsigned shell_calls = 0;
std::wstring actual_file, actual_args, actual_dir, actual_verb;
HWND actual_owner = nullptr;
BOOL WINAPI CreateStub(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
    BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION) {
    SetLastError(create_error); return create_error == ERROR_SUCCESS;
}
BOOL WINAPI ShellStub(SHELLEXECUTEINFOW* info) {
    ++shell_calls;
    actual_file = info->lpFile ? info->lpFile : L"";
    actual_args = info->lpParameters ? info->lpParameters : L"";
    actual_dir = info->lpDirectory ? info->lpDirectory : L"";
    actual_verb = info->lpVerb ? info->lpVerb : L"";
    actual_owner = info->hwnd;
    SetLastError(shell_error); return shell_error == ERROR_SUCCESS;
}
BOOL WINAPI CreateHidden(LPCWSTR image, LPWSTR command, LPSECURITY_ATTRIBUTES a, LPSECURITY_ATTRIBUTES b,
    BOOL inherit, DWORD flags, LPVOID environment, LPCWSTR directory, LPSTARTUPINFOW startup, LPPROCESS_INFORMATION process) {
    startup->dwFlags |= STARTF_USESHOWWINDOW; startup->wShowWindow = SW_HIDE;
    return CreateProcessW(image, command, a, b, inherit, flags, environment, directory, startup, process);
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && std::wstring_view(argv[1]) == L"--child") {
        wchar_t directory[32768]{}; GetCurrentDirectoryW(ARRAYSIZE(directory), directory);
        std::wofstream output(argv[2]); output << argv[3] << L'\n' << directory;
        return output ? 0 : 1;
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--probe-elevation") {
        std::wstring command = L"\"" + std::wstring(argv[2]) + L"\"";
        STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
        const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
            CREATE_SUSPENDED, nullptr, nullptr, &startup, &process);
        const DWORD error = created ? ERROR_SUCCESS : GetLastError();
        // Never execute the downloaded application, even if it did not request elevation.
        if (created) { TerminateProcess(process.hProcess, 0); WaitForSingleObject(process.hProcess, 5000);
            CloseHandle(process.hThread); CloseHandle(process.hProcess); }
        std::cout << "created=" << created << " win32_error=" << error << '\n';
        return !created && error == ERROR_ELEVATION_REQUIRED ? 0 : 1;
    }
    bool ok = true;
    auto check = [&](bool condition, const char* label) { ok &= condition;
        std::cout << (condition ? "[PASS] " : "[FAIL] ") << label << '\n'; };
    wchar_t self[32768]{}; GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"shell-command-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(root / L"工具 folder");
    const auto image = root / L"工具 folder" / L"menu helper.exe";
    std::filesystem::copy_file(self, image);
    const std::wstring arguments = L"/Monitor \"C:\\测试 文件\\setup.exe\" /mode=\"two words\"";
    const std::wstring command = L"\"" + image.wstring() + L"\" " + arguments;
    ShellCommandParts parts;
    check(SplitShellCommand(command, parts) && parts.executable == image.wstring() && parts.arguments == arguments,
        "quoted Unicode image and original argument quoting preserved");
    check(SplitShellCommand(image.wstring() + L" " + arguments, parts) && parts.executable == image.wstring() && parts.arguments == arguments,
        "unquoted executable path containing spaces resolved without truncation");
    check(!SplitShellCommand(L"\"unfinished", parts) && !SplitShellCommand(L"", parts), "malformed command rejected");
    const HWND owner = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1234));
    const ShellCommandApi stub{CreateStub, ShellStub};
    auto result = LaunchShellCommand(command, root.wstring(), owner, stub);
    check(!result.error && result.create_error == ERROR_ELEVATION_REQUIRED && result.elevation_requested && shell_calls == 1,
        "error 740 delegates exactly once to Windows elevation");
    check(actual_file == image.wstring() && actual_args == arguments && actual_dir == root.wstring() &&
        actual_verb == L"runas" && actual_owner == owner, "elevation preserves image, arguments, working directory and owner");
    shell_error = ERROR_CANCELLED;
    result = LaunchShellCommand(command, root.wstring(), owner, stub);
    check(result.error == ERROR_CANCELLED && shell_calls == 2, "UAC cancellation is preserved with no retry");
    shell_error = ERROR_ACCESS_DENIED;
    result = LaunchShellCommand(command, root.wstring(), owner, stub);
    check(result.error == ERROR_ACCESS_DENIED && result.create_error == ERROR_ELEVATION_REQUIRED,
        "elevation launch failure preserves both diagnostic error codes");
    const auto calls = shell_calls;
    create_error = ERROR_SUCCESS;
    result = LaunchShellCommand(command, root.wstring(), owner, stub);
    check(!result.error && !result.elevation_requested && shell_calls == calls, "ordinary executable does not request elevation");
    create_error = ERROR_FILE_NOT_FOUND;
    result = LaunchShellCommand(command, root.wstring(), owner, stub);
    check(result.error == ERROR_FILE_NOT_FOUND && shell_calls == calls, "unrelated failures never trigger elevation");
    const auto marker = root / L"child.txt";
    const auto live = LaunchShellCommand(L"\"" + image.wstring() + L"\" --child \"" + marker.wstring() +
        L"\" \"two words\"", root.wstring(), nullptr, {CreateHidden, ShellStub});
    const auto deadline = GetTickCount64() + 5000;
    while (!std::filesystem::exists(marker) && GetTickCount64() < deadline) Sleep(10);
    // Wait for the child to finish writing before comparing its captured values.
    std::wstring argument, directory;
    while (GetTickCount64() < deadline) {
        std::wifstream input(marker); std::getline(input, argument); std::getline(input, directory);
        if (directory == root.wstring()) break;
        Sleep(10);
    }
    check(!live.error && argument == L"two words" && directory == root.wstring(), "real non-elevated child receives argument and selected working directory");
    std::wcout << L"[INFO] isolated fixture=" << root.wstring() << L'\n';
    return ok ? 0 : 1;
}
