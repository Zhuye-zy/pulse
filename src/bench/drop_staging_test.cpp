#include "../app/drop_staging.h"

#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace pulse::app;
namespace fsys = std::filesystem;

namespace {
int passed = 0;
int failed = 0;
void Check(bool condition, const wchar_t* name) {
    ++(condition ? passed : failed);
    wprintf(L"[%s] %s\n", condition ? L"PASS" : L"FAIL", name);
}
void Write(const fsys::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}
std::string Read(const fsys::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}
void ForceRemove(const fsys::path& root) {
    std::error_code ec;
    for (auto it = fsys::recursive_directory_iterator(root, ec); !ec && it != fsys::recursive_directory_iterator(); ++it)
        SetFileAttributesW(it->path().c_str(), FILE_ATTRIBUTE_NORMAL);
    fsys::remove_all(root, ec);
}
}  // namespace

int wmain() {
    const fsys::path root = fsys::temp_directory_path() /
        (L"pulse-drop-staging-test-" + std::to_wstring(GetCurrentProcessId()));
    ForceRemove(root);
    const fsys::path temp = root / L"temp";
    const fsys::path stage_root = temp / L"PulseDrop";
    const fsys::path extract = temp / L"7zE44D7D628";
    const fsys::path other = root / L"other";
    fsys::create_directories(extract / L"sub");
    fsys::create_directories(other);
    Write(extract / L"plugin.dll", "plugin bytes");
    Write(extract / L"sub" / L"inner.txt", "inner");
    Write(extract / L"locked.txt", "read only");
    SetFileAttributesW((extract / L"locked.txt").c_str(), FILE_ATTRIBUTE_READONLY);
    Write(other / L"keep.txt", "elsewhere");

    Check(IsTemporaryDropSource((extract / L"plugin.dll").wstring(), temp.wstring(), stage_root.wstring()),
          L"an archive extraction under the temp folder is temporary");
    Check(IsTemporaryDropSource(L"\\\\?\\" + (extract / L"plugin.dll").wstring(), temp.wstring() + L"\\", stage_root.wstring()),
          L"long-path prefixes and trailing separators do not matter");
    Check(!IsTemporaryDropSource((other / L"keep.txt").wstring(), temp.wstring(), stage_root.wstring()),
          L"a source outside the temp folder is not temporary");
    Check(!IsTemporaryDropSource((root / L"temp2" / L"x.txt").wstring(), temp.wstring(), stage_root.wstring()),
          L"a sibling sharing the temp folder's name prefix is not temporary");
    Check(!IsTemporaryDropSource((stage_root / L"1-1-1" / L"x.txt").wstring(), temp.wstring(), stage_root.wstring()),
          L"Pulse's own stage is never staged again");
    Check(!TempDirectory().empty() && TempDirectory().find(L'~') == std::wstring::npos,
          L"the temp folder is reported in long form");

    const std::vector<std::wstring> sources{(extract / L"plugin.dll").wstring(), (extract / L"sub").wstring(),
                                            (other / L"keep.txt").wstring(), (extract / L"locked.txt").wstring()};
    std::vector<std::wstring> staged;
    const bool any = StageDropSources(sources, temp.wstring(), stage_root.wstring(), staged);
    Check(any && staged.size() == 4, L"temporary sources are staged");
    Check(staged[2] == sources[2], L"other sources pass through unchanged");
    const fsys::path staged_dll = staged[0];
    Check(staged[0] != sources[0] && staged_dll.filename() == L"plugin.dll" &&
          staged_dll.parent_path().filename() == L"7zE44D7D628" &&
          staged_dll.parent_path().parent_path().parent_path() == stage_root,
          L"a staged file keeps its name under a folder named like its original parent");

    // The archive manager deletes its extraction once the drop returns.
    ForceRemove(extract);
    Check(!fsys::exists(extract), L"fixture: the archive manager removed its temporary folder");
    Check(Read(staged[0]) == "plugin bytes", L"the staged file survives the archive manager's cleanup");
    Check(Read(fsys::path(staged[1]) / L"inner.txt") == "inner", L"a staged folder keeps its contents");
    Check(Read(staged[3]) == "read only" &&
          (GetFileAttributesW(staged[3].c_str()) & FILE_ATTRIBUTE_READONLY) != 0,
          L"a read-only file is staged as a copy that stays read-only");

    std::vector<std::wstring> untouched;
    Check(!StageDropSources({(other / L"keep.txt").wstring()}, temp.wstring(), stage_root.wstring(), untouched) &&
          untouched.size() == 1 && untouched[0] == (other / L"keep.txt").wstring(),
          L"nothing is staged when no source is temporary");

    // 999999 is not a multiple of four, so no process can have it.
    const fsys::path dead = stage_root / L"999999-1-1";
    fsys::create_directories(dead / L"x");
    Write(dead / L"x" / L"left.txt", "left over");
    const fsys::path unrelated = stage_root / L"notes";
    fsys::create_directories(unrelated);
    SweepDropStages(stage_root.wstring(), false);
    Check(!fsys::exists(dead), L"a stage left by an exited process is swept");
    Check(fsys::exists(staged_dll), L"this process's stage survives the startup sweep");
    Check(fsys::exists(unrelated), L"folders that are not stages are left alone");
    SweepDropStages(stage_root.wstring(), true);
    Check(!fsys::exists(staged_dll.parent_path().parent_path()),
          L"this process's stage, read-only copy included, is removed at exit");

    ForceRemove(root);
    wprintf(L"%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
