#include "drop_staging.h"

#include <windows.h>
#include <atomic>
#include <cwctype>

namespace pulse::app {
namespace {

// Plain Win32 form without the \\?\ prefix or trailing separators.
std::wstring Plain(std::wstring path) {
    for (auto& c : path) if (c == L'/') c = L'\\';
    if (path.starts_with(L"\\\\?\\UNC\\")) path = L"\\\\" + path.substr(8);
    else if (path.starts_with(L"\\\\?\\")) path = path.substr(4);
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    return path;
}

std::wstring Long(const std::wstring& path) {
    const std::wstring plain = Plain(path);
    if (plain.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + plain.substr(2);
    return L"\\\\?\\" + plain;
}

// Expands 8.3 components (C:\Users\ADMINI~1) so prefixes compare reliably.
std::wstring Expanded(const std::wstring& path) {
    std::wstring plain = Plain(path);
    if (plain.find(L'~') == std::wstring::npos) return plain;
    wchar_t buffer[32768];
    const DWORD n = GetLongPathNameW(Long(plain).c_str(), buffer, ARRAYSIZE(buffer));
    return n > 0 && n < ARRAYSIZE(buffer) ? Plain(buffer) : plain;
}

bool Within(const std::wstring& path, const std::wstring& dir) {
    const std::wstring a = Expanded(path);
    const std::wstring b = Expanded(dir);
    if (b.empty() || a.size() <= b.size()) return false;
    if (CompareStringOrdinal(a.c_str(), static_cast<int>(b.size()), b.c_str(),
                             static_cast<int>(b.size()), TRUE) != CSTR_EQUAL)
        return false;
    return b.back() == L'\\' || a[b.size()] == L'\\';
}

std::wstring Leaf(const std::wstring& path) {
    const std::wstring plain = Plain(path);
    const size_t slash = plain.find_last_of(L'\\');
    return slash == std::wstring::npos ? plain : plain.substr(slash + 1);
}

std::wstring Parent(const std::wstring& path) {
    const std::wstring plain = Plain(path);
    const size_t slash = plain.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : plain.substr(0, slash);
}

template <typename Fn>
void ForEachChild(const std::wstring& dir, Fn&& fn) {
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW((Long(dir) + L"\\*").c_str(), FindExInfoBasic, &data,
                                   FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
        if (!fn(data)) break;
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

// Hard links share the source's data, which survives the archive manager
// deleting its own name. Read-only files are copied instead, so removing a
// stage never has to change attributes that a link would share.
bool LinkOrCopyTree(const std::wstring& from, const std::wstring& to) {
    const DWORD attrs = GetFileAttributesW(Long(from).c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) return false;  // never follow junctions
        if (!CreateDirectoryW(Long(to).c_str(), nullptr)) return false;
        bool ok = true;
        ForEachChild(from, [&](const WIN32_FIND_DATAW& child) {
            ok = LinkOrCopyTree(Plain(from) + L"\\" + child.cFileName, Plain(to) + L"\\" + child.cFileName);
            return ok;
        });
        return ok;
    }
    if (!(attrs & FILE_ATTRIBUTE_READONLY) &&
        CreateHardLinkW(Long(to).c_str(), Long(from).c_str(), nullptr))
        return true;
    return CopyFileW(Long(from).c_str(), Long(to).c_str(), TRUE) != FALSE;
}

void RemoveTree(const std::wstring& path) {
    const std::wstring target = Long(path);
    const DWORD attrs = GetFileAttributesW(target.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        if (!(attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
            ForEachChild(path, [&](const WIN32_FIND_DATAW& child) {
                RemoveTree(Plain(path) + L"\\" + child.cFileName);
                return true;
            });
        }
        RemoveDirectoryW(target.c_str());
        return;
    }
    // Read-only entries in a stage are copies (LinkOrCopyTree), not links.
    if (attrs & FILE_ATTRIBUTE_READONLY)
        SetFileAttributesW(target.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
    DeleteFileW(target.c_str());
}

bool ProcessAlive(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return GetLastError() == ERROR_ACCESS_DENIED;  // exists, not ours to open
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
}

} // namespace

std::wstring TempDirectory() {
    wchar_t buffer[MAX_PATH + 1]{};
    const DWORD n = GetTempPathW(ARRAYSIZE(buffer), buffer);
    if (n == 0 || n >= ARRAYSIZE(buffer)) return {};
    return Expanded(buffer);
}

std::wstring DropStageRoot() {
    const std::wstring temp = TempDirectory();
    return temp.empty() ? std::wstring() : temp + L"\\PulseDrop";
}

bool IsTemporaryDropSource(const std::wstring& path, const std::wstring& temp_dir,
                           const std::wstring& stage_root) {
    if (path.empty() || temp_dir.empty() || !Within(path, temp_dir)) return false;
    if (stage_root.empty()) return true;
    return !Within(path, stage_root) &&
           CompareStringOrdinal(Expanded(path).c_str(), -1, Expanded(stage_root).c_str(), -1, TRUE) != CSTR_EQUAL;
}

bool StageDropSources(const std::vector<std::wstring>& sources, const std::wstring& temp_dir,
                      const std::wstring& stage_root, std::vector<std::wstring>& staged) {
    static std::atomic<unsigned> counter{0};
    staged = sources;
    if (stage_root.empty()) return false;
    std::wstring stage;
    bool any = false;
    for (size_t i = 0; i < sources.size(); ++i) {
        const std::wstring& source = sources[i];
        if (!IsTemporaryDropSource(source, temp_dir, stage_root)) continue;
        if (stage.empty()) {
            CreateDirectoryW(Long(stage_root).c_str(), nullptr);
            stage = Plain(stage_root) + L"\\" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                    std::to_wstring(++counter) + L"-" + std::to_wstring(GetTickCount64());
            if (!CreateDirectoryW(Long(stage).c_str(), nullptr)) return false;
        }
        // Named like the original parent (7zE44D7D628), which dialogs show.
        std::wstring parent = Leaf(Parent(source));
        if (parent.empty()) parent = L"Temp";
        const std::wstring folder = stage + L"\\" + parent;
        CreateDirectoryW(Long(folder).c_str(), nullptr);
        const std::wstring target = folder + L"\\" + Leaf(source);
        if (GetFileAttributesW(Long(target).c_str()) != INVALID_FILE_ATTRIBUTES) continue;  // name taken
        if (!LinkOrCopyTree(source, target)) {
            RemoveTree(target);
            continue;
        }
        staged[i] = target;
        any = true;
    }
    if (!any && !stage.empty()) RemoveTree(stage);
    return any;
}

void SweepDropStages(const std::wstring& stage_root, bool include_own) {
    if (stage_root.empty()) return;
    const DWORD self = GetCurrentProcessId();
    std::vector<std::wstring> doomed;
    ForEachChild(stage_root, [&](const WIN32_FIND_DATAW& child) {
        if (!(child.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return true;
        wchar_t* end = nullptr;
        const unsigned long pid = wcstoul(child.cFileName, &end, 10);
        if (end == child.cFileName || *end != L'-') return true;  // not a stage folder
        if (pid == self ? include_own : !ProcessAlive(static_cast<DWORD>(pid)))
            doomed.push_back(Plain(stage_root) + L"\\" + child.cFileName);
        return true;
    });
    for (const auto& dir : doomed) RemoveTree(dir);
}

} // namespace pulse::app
