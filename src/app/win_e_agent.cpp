#include "win_e_agent.h"
#include "single_instance_coordinator.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace pulse::app::win_e_agent {
namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"PulseWinE";
constexpr wchar_t kStartupShortcut[] = L"Pulse WinE.lnk";
constexpr wchar_t kWindowClass[] = L"PulseWinEAgentWindow";
constexpr wchar_t kMutexName[] = L"Local\\Pulse.WinE.Agent";
constexpr UINT kLaunch = WM_APP + 1;
constexpr UINT kStop = WM_APP + 2;
constexpr UINT kStatus = WM_APP + 3;
constexpr UINT_PTR kHealthTimer = 1;
constexpr UINT_PTR kActivateTimer = 2;
HWND window = nullptr;
HHOOK hook = nullptr;
bool e_suppressed = false;
int activation_attempts = 0;
bool activation_attached = false;
std::wstring executable;

std::wstring Command(const std::wstring& exe) {
    return L"\"" + exe + L"\" --win-e-agent";
}

bool SetStartupShortcut(const std::wstring& exe, bool enabled) {
    PWSTR startup = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Startup, KF_FLAG_CREATE, nullptr, &startup)))
        return false;
    const std::wstring path = std::wstring(startup) + L"\\" + kStartupShortcut;
    CoTaskMemFree(startup);
    if (!enabled) {
        const BOOL deleted = DeleteFileW(path.c_str());
        return deleted || GetLastError() == ERROR_FILE_NOT_FOUND;
    }

    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = SUCCEEDED(initialized);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return false;
    IShellLinkW* link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&link));
    if (SUCCEEDED(hr)) {
        const size_t slash = exe.find_last_of(L"\\/");
        const std::wstring working_dir = slash == std::wstring::npos ? L"" : exe.substr(0, slash);
        hr = link->SetPath(exe.c_str());
        if (SUCCEEDED(hr)) hr = link->SetArguments(L"--win-e-agent");
        if (SUCCEEDED(hr) && !working_dir.empty()) hr = link->SetWorkingDirectory(working_dir.c_str());
        IPersistFile* persist = nullptr;
        if (SUCCEEDED(hr)) hr = link->QueryInterface(IID_PPV_ARGS(&persist));
        if (SUCCEEDED(hr)) {
            hr = persist->Save(path.c_str(), TRUE);
            persist->Release();
        }
        link->Release();
    }
    if (uninitialize) CoUninitialize();
    return SUCCEEDED(hr);
}

std::wstring CurrentExe() {
    std::wstring path(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size >= static_cast<DWORD>(path.size())) return {};
    path.resize(size);
    return path;
}

std::wstring ReadRunValue() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return {};
    wchar_t value[32768]{};
    DWORD bytes = sizeof(value) - sizeof(wchar_t);
    DWORD type = 0;
    const LONG result = RegQueryValueExW(key, kRunValue, nullptr, &type,
                                          reinterpret_cast<BYTE*>(value), &bytes);
    RegCloseKey(key);
    return result == ERROR_SUCCESS && type == REG_SZ ? value : L"";
}

HWND AgentWindow() {
    return FindWindowExW(HWND_MESSAGE, nullptr, kWindowClass, nullptr);
}

bool LaunchPulse() {
    if (GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    std::wstring command = L"\"" + executable + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                        0, nullptr, nullptr, &startup, &process))
        return false;
    AllowSetForegroundWindow(process.dwProcessId);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

LRESULT CALLBACK KeyboardHook(int code, WPARAM message, LPARAM data) {
    if (code == HC_ACTION) {
        const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
        if (key->vkCode == 'E') {
            const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
            const bool up = message == WM_KEYUP || message == WM_SYSKEYUP;
            const bool win = (GetAsyncKeyState(VK_LWIN) & 0x8000) ||
                             (GetAsyncKeyState(VK_RWIN) & 0x8000);
            const bool other_modifier = (GetAsyncKeyState(VK_CONTROL) & 0x8000) ||
                                        (GetAsyncKeyState(VK_MENU) & 0x8000) ||
                                        (GetAsyncKeyState(VK_SHIFT) & 0x8000);
            if (down && win && !other_modifier && window) {
                if (!e_suppressed) {
                    // The shell must see another key with Win, otherwise the
                    // released Win key opens Start after E is suppressed.
                    INPUT cancel[2]{};
                    cancel[0].type = INPUT_KEYBOARD;
                    cancel[0].ki.wVk = VK_F24;
                    cancel[1] = cancel[0];
                    cancel[1].ki.dwFlags = KEYEVENTF_KEYUP;
                    if (SendInput(2, cancel, sizeof(INPUT)) != 2 ||
                        !PostMessageW(window, kLaunch, 0, 0))
                        return CallNextHookEx(hook, code, message, data);
                }
                e_suppressed = true;
                return 1;
            }
            if (up && e_suppressed) {
                e_suppressed = false;
                return 1;
            }
        }
    }
    return CallNextHookEx(hook, code, message, data);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == kLaunch) {
        if (!LaunchPulse()) {
            // If Pulse was removed while the agent is running, keep Explorer usable.
            ShellExecuteW(nullptr, L"open", L"explorer.exe", nullptr, nullptr, SW_SHOWNORMAL);
        } else {
            activation_attempts = 0;
            activation_attached = false;
            SetTimer(hwnd, kActivateTimer, 100, nullptr);
        }
        return 0;
    }
    if (message == kStop) { DestroyWindow(hwnd); return 0; }
    if (message == kStatus) return hook != nullptr ? 1 : 0;
    if (message == WM_TIMER) {
        if (wparam == kHealthTimer) {
            if (!Enabled(executable) ||
                GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES)
                DestroyWindow(hwnd);
        } else if (wparam == kActivateTimer) {
            ++activation_attempts;
            HWND pulse_window = FindWindowW(SingleInstanceCoordinator::WindowClassName(), nullptr);
            if (pulse_window) {
                ShowWindow(pulse_window, IsIconic(pulse_window) ? SW_RESTORE : SW_SHOW);
                if (!SetForegroundWindow(pulse_window) && !activation_attached &&
                    activation_attempts >= 2) {
                    activation_attached = true;
                    const HWND foreground = GetForegroundWindow();
                    const DWORD foreground_thread = GetWindowThreadProcessId(foreground, nullptr);
                    const DWORD agent_thread = GetCurrentThreadId();
                    if (foreground_thread && foreground_thread != agent_thread &&
                        AttachThreadInput(agent_thread, foreground_thread, TRUE)) {
                        BringWindowToTop(pulse_window);
                        SetForegroundWindow(pulse_window);
                        AttachThreadInput(agent_thread, foreground_thread, FALSE);
                    }
                }
            }
            if (activation_attempts >= 30 ||
                (activation_attempts >= 10 && pulse_window && GetForegroundWindow() == pulse_window))
                KillTimer(hwnd, kActivateTimer);
        }
        return 0;
    }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

} // namespace

bool Enabled(const std::wstring& exe) {
    const std::wstring actual = ReadRunValue();
    const std::wstring expected = Command(exe);
    return !exe.empty() && CompareStringOrdinal(actual.c_str(), -1,
        expected.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool EnsureRunning(const std::wstring& exe) {
    if (Enabled(exe)) SetStartupShortcut(exe, true);
    auto ready = [] {
        if (HWND agent = AgentWindow()) {
            DWORD_PTR result = 0;
            return SendMessageTimeoutW(agent, kStatus, 0, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &result) && result == 1;
        }
        return false;
    };
    if (ready()) return true;
    std::wstring command = Command(exe);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        return false;
    CloseHandle(process.hThread);
    for (int i = 0; i < 40; ++i) {
        if (ready()) { CloseHandle(process.hProcess); return true; }
        if (WaitForSingleObject(process.hProcess, 50) == WAIT_OBJECT_0) break;
    }
    CloseHandle(process.hProcess);
    return ready();
}

bool SetEnabled(const std::wstring& exe, bool enabled) {
    if (exe.empty()) return false;
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                        KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    bool ok = true;
    if (enabled) {
        const std::wstring command = Command(exe);
        ok = RegSetValueExW(key, kRunValue, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
        if (ok) SetStartupShortcut(exe, true);
        if (ok && !EnsureRunning(exe)) {
            RegDeleteValueW(key, kRunValue);
            SetStartupShortcut(exe, false);
            ok = false;
        }
    } else {
        if (Enabled(exe)) {
            const LONG result = RegDeleteValueW(key, kRunValue);
            ok = result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
        }
        if (ok) {
            SetStartupShortcut(exe, false);
            if (HWND agent = AgentWindow()) PostMessageW(agent, kStop, 0, 0);
        }
    }
    RegCloseKey(key);
    return ok;
}

int Run() {
    executable = CurrentExe();
    if (!Enabled(executable)) return 1;
    SetStartupShortcut(executable, true);
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return 1;
    TOKEN_ELEVATION elevation{};
    DWORD bytes = 0;
    const bool token_ok = GetTokenInformation(token, TokenElevation, &elevation,
                                               sizeof(elevation), &bytes) != FALSE;
    CloseHandle(token);
    if (!token_ok || elevation.TokenIsElevated) return 1;
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(mutex); return 0; }
    WNDCLASSW cls{};
    cls.lpfnWndProc = WindowProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = kWindowClass;
    const ATOM atom = RegisterClassW(&cls);
    if (!atom) { ReleaseMutex(mutex); CloseHandle(mutex); return 1; }
    window = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0,
                             HWND_MESSAGE, nullptr, cls.hInstance, nullptr);
    if (!window) { UnregisterClassW(kWindowClass, cls.hInstance); ReleaseMutex(mutex); CloseHandle(mutex); return 1; }
    hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHook, cls.hInstance, 0);
    if (!hook) {
        DestroyWindow(window);
        window = nullptr;
        UnregisterClassW(kWindowClass, cls.hInstance);
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return 1;
    }
    SetTimer(window, kHealthTimer, 2000, nullptr);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    UnhookWindowsHookEx(hook);
    hook = nullptr;
    window = nullptr;
    UnregisterClassW(kWindowClass, cls.hInstance);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}

} // namespace pulse::app::win_e_agent
