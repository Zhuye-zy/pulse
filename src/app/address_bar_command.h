// address_bar_command.h — Typing cmd / powershell / pwsh / wt in the address
// bar opens that program in the current folder, like Explorer (#44-⑤).
#pragma once
#include <string>
#include <string_view>

namespace pulse::app {

enum class AddressBarProgram { None, Cmd, PowerShell, Pwsh, WindowsTerminal };

struct AddressBarCommand {
    AddressBarProgram program = AddressBarProgram::None;
    std::wstring args;   // Everything after the program name, trimmed.
};

// Recognizes "<program>[.exe] [args]" (case-insensitive). Anything else,
// including paths, returns AddressBarProgram::None and is navigated as before.
AddressBarCommand ParseAddressBarCommand(std::wstring_view text);

// Executable to launch for a recognized program (nullptr for None).
const wchar_t* AddressBarProgramExe(AddressBarProgram program);

// Folder shortcuts typed in the address bar (#54): %VAR% environment variables
// (%temp%, %appdata%\Microsoft) and shell: folders (shell:startup,
// shell:sendto\sub, shell:::{CLSID}). shell:MyComputerFolder resolves to This PC
// (an empty path) and shell:RecycleBinFolder to the pulse:recycle view.
struct AddressShortcut {
    bool resolved = false;   // false: not a shortcut, or one that names nothing
    std::wstring path;       // filesystem path, L"" for This PC, or a pulse: view
};

// Surrounding spaces and quotes are ignored. Plain paths, unknown variables
// and shell: names without a filesystem folder come back unresolved, so they
// keep the address bar's ordinary handling. shell: lookups need COM.
AddressShortcut ResolveAddressShortcut(std::wstring_view text);

} // namespace pulse::app
