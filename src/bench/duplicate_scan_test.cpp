#include "../app/duplicate_scan.h"
#include "../ops/ops_manager.h"
#include <algorithm>
#include <filesystem>

#include <cstdio>

using namespace pulse;

namespace {

int passed = 0;
int failed = 0;

void Check(bool condition, const wchar_t* name) {
    ++(condition ? passed : failed);
    wprintf(L"[%s] %s\n", condition ? L"PASS" : L"FAIL", name);
}

void TestDeletedKeepers() {
    app::DuplicateScanSession session;
    session.generation = 44;
    index::ContentSearchProgress progress;
    progress.generation = 44;
    progress.done = true;
    index::ContentHit keeper, extra, third;
    keeper.path = L"C:\\scan\\kept\\a.bin";
    keeper.group = 1; keeper.modified = 30; keeper.size = 100;
    extra = keeper; extra.path = L"C:\\scan\\other\\b.bin"; extra.modified = 20;
    third = extra; third.path = L"C:\\scan\\other\\c.bin"; third.modified = 10;
    session.ApplyUpdate(progress, {keeper, extra, third});
    ops::OpRequest request;
    Check(session.BuildCleanupRequest(request, 0) && request.duplicate_cleanup &&
              request.sources.size() == 2 && request.duplicate_groups.size() == 1 &&
              request.duplicate_groups[0].keeper.path == keeper.path,
          L"group cleanup carries keeper and candidate snapshots");
    Check(session.BuildCleanupRequest(request) && request.sources.size() == 2,
          L"all cleanup uses the same guarded request");
    session.RemoveDeleted({L"c:\\SCAN\\kep"});
    Check(session.groups.size() == 1, L"deletion respects path component boundaries");
    session.RemoveDeleted({L"c:\\SCAN\\KEPT\\"});
    Check(session.groups.empty() && !session.BuildCleanupRequest(request, 0) &&
              !session.BuildCleanupRequest(request),
          L"deleted keeper parent invalidates group and all cleanup");
    session.ApplyUpdate(progress, {extra, third});
    Check(session.groups.empty(), L"late results cannot revive an invalidated keeper group");
    session.ResetResults();
    session.ApplyUpdate(progress, {keeper, extra, third});
    Check(session.groups.size() == 1, L"new scan resets invalidated group identities");
    session.RemoveDeleted({keeper.path});
    Check(!session.BuildCleanupRequest(request), L"direct keeper deletion requires a new scan");
    session.ResetResults();
    session.ApplyUpdate(progress, {keeper, extra, third});
    session.RemoveDeleted({extra.path});
    Check(session.BuildCleanupRequest(request) && request.sources == std::vector<std::wstring>{third.path},
          L"deleting an extra preserves the keeper and remaining candidate");
    session.scanning = true;
    Check(!session.BuildCleanupRequest(request, 0) && !session.BuildCleanupRequest(request),
          L"cleanup is rejected while scanning");
    session.ResetResults();
    keeper.path = L"\\\\?\\UNC\\server\\share\\kept\\a.bin";
    session.ApplyUpdate(progress, {keeper, extra});
    session.RemoveDeleted({L"\\\\server\\share\\kept"});
    Check(session.groups.empty(), L"UNC extended scan paths match ordinary deleted parent paths");
    session.ResetResults();
    keeper.path = L"\\\\?\\C:\\scan\\kept\\a.bin";
    session.ApplyUpdate(progress, {keeper, extra});
    session.RemoveDeleted({L"C:/scan/kept/"});
    Check(session.groups.empty(), L"long path prefixes and separator styles do not preserve stale keepers");
}

void TestCleanupGuard() {
    namespace fs = std::filesystem;
    const fs::path root = fs::absolute(fs::path(__FILE__).parent_path().parent_path().parent_path() /
        L"bench_data" / (L"duplicate-guard-" + std::to_wstring(GetCurrentProcessId()) +
                         L"-" + std::to_wstring(GetTickCount64())));
    std::error_code error;
    fs::create_directories(root.parent_path(), error);
    const bool created = fs::create_directory(root, error);
    Check(created, L"guard fixture is an isolated new directory");
    if (!created) return;
    const auto kept = (root / L"keeper.bin").wstring();
    const auto extra = (root / L"extra.bin").wstring();
    const std::string bytes(131073, 'x');
    auto write = [&](const std::wstring& file, const std::string& data, const FILETIME* modified = nullptr) {
        HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        bool ok = WriteFile(handle, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                  written == data.size();
        if (modified) ok = SetFileTime(handle, nullptr, nullptr, modified) && ok;
        CloseHandle(handle);
        return ok;
    };
    auto snapshot = [&](const std::wstring& file) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        Check(GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data) != FALSE,
              L"guard fixture metadata is readable");
        return ops::DuplicateCleanupFile{file,
            (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow,
            (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime};
    };
    Check(write(kept, bytes) && write(extra, bytes), L"guard fixture files created");
    ops::DuplicateCleanupGroup group{snapshot(kept), {snapshot(extra)}};
    const std::vector<std::wstring> sources{extra};
    const auto running = [] { return false; };
    {
        ops::DuplicateCleanupGuard guard;
        Check(guard.Validate({group}, sources, running), L"identical unchanged files pass cleanup verification");
        HANDLE writer = CreateFileW(kept.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
        Check(writer == INVALID_HANDLE_VALUE, L"keeper remains protected against concurrent writes");
        if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
        Check(!DeleteFileW(kept.c_str()) && GetLastError() == ERROR_SHARING_VIOLATION,
              L"keeper remains protected against deletion through cleanup lifetime");
    }
    {
        ops::DuplicateCleanupGuard guard;
        Check(!guard.Validate({group}, {kept}, running), L"unverified source substitution is rejected");
    }
    {
        ops::DuplicateCleanupGuard guard;
        int polls = 0;
        Check(!guard.Validate({group}, sources, [&] { return ++polls >= 4; }),
              L"verification stops on cancellation during content reads");
    }
    const FILETIME timestamp{static_cast<DWORD>(group.extras[0].modified),
                             static_cast<DWORD>(group.extras[0].modified >> 32)};
    std::string changed = bytes; changed.back() = 'y';
    Check(write(extra, changed, &timestamp), L"candidate fixture changed with original size and timestamp");
    {
        ops::DuplicateCleanupGuard guard;
        Check(!guard.Validate({group}, sources, running), L"changed candidate content is rejected despite matching metadata");
    }
    Check(write(extra, bytes, &timestamp), L"candidate fixture restored");
    {
        auto stale = group; ++stale.keeper.modified;
        ops::DuplicateCleanupGuard guard;
        Check(!guard.Validate({stale}, sources, running), L"stale keeper metadata requires a new scan");
    }
    const auto moved = (root / L"moved.bin").wstring();
    Check(MoveFileW(kept.c_str(), moved.c_str()) != FALSE, L"keeper fixture moved after scan");
    {
        ops::DuplicateCleanupGuard guard;
        Check(!guard.Validate({group}, sources, running), L"missing keeper blocks deletion of surviving candidate");
    }
    Check(DeleteFileW(extra.c_str()) && DeleteFileW(moved.c_str()) && RemoveDirectoryW(root.c_str()),
          L"guard fixture handles released and isolated files cleaned up");
}

} // namespace

int wmain() {
    TestDeletedKeepers();
    TestCleanupGuard();
    app::DuplicateScanSession session;
    Check(app::DuplicateScanSession::DefaultMinimumBytes(app::DuplicateScanScope::Folder) == 1024,
          L"folder default minimum is 1 KB");
    Check(app::DuplicateScanSession::DefaultMinimumBytes(app::DuplicateScanScope::Drive) ==
              1024ull * 1024ull,
          L"drive default minimum is 1 MB");
    Check(app::DuplicateScanSession::NormalizeDriveRoot(L"c:") == L"C:\\" &&
              app::DuplicateScanSession::NormalizeDriveRoot(L"D:\\") == L"D:\\",
          L"drive roots normalize to X:\\");

    index::VolumeInfo fixed;
    fixed.mount_point = L"C:\\";
    fixed.kind = index::VolumeKind::Fixed;
    index::VolumeInfo usb;
    usb.mount_point = L"E:\\";
    usb.kind = index::VolumeKind::Removable;
    auto all = app::DuplicateScanSession::ResolveRoots(
        app::DuplicateScanScope::AllFixed, L"", L"", {fixed, usb});
    Check(all.size() == 1 && all[0] == L"C:\\", L"all-local-disks uses fixed volumes only");
    auto drive = app::DuplicateScanSession::ResolveRoots(
        app::DuplicateScanScope::Drive, L"", L"E:\\", {fixed, usb});
    Check(drive.size() == 1 && drive[0] == L"E:\\", L"drive scope uses the selected root");
    auto folder = app::DuplicateScanSession::ResolveRoots(
        app::DuplicateScanScope::Folder, L"C:\\Windows", L"", {fixed, usb});
    Check(folder.size() == 1 && folder[0] == L"C:\\Windows", L"folder scope uses the folder path");

    session.generation = 1;
    session.scanning = true;
    index::ContentHit older;
    older.path = L"C:\\old.bin";
    older.name = L"old.bin";
    older.size = 100;
    older.modified = 10;
    older.group = 3;
    index::ContentHit newer;
    newer.path = L"D:\\new.bin";
    newer.name = L"new.bin";
    newer.size = 100;
    newer.modified = 20;
    newer.group = 3;
    index::ContentSearchProgress progress;
    progress.generation = 1;
    progress.done = true;
    session.ApplyUpdate(progress, {older, newer});
    Check(session.groups.size() == 1 && session.groups[0].files.size() == 2,
          L"hits with the same group become one card");
    Check(session.groups[0].keep_index == 0 &&
              session.groups[0].files[0].path == L"D:\\new.bin",
          L"default keep is the newest modified file");
    auto extra = session.FilesToDelete(0);
    Check(extra.size() == 1 && extra[0] == L"C:\\old.bin", L"FilesToDelete omits the kept file");
    session.SetKeep(0, 1);
    extra = session.FilesToDelete(0);
    Check(extra.size() == 1 && extra[0] == L"D:\\new.bin", L"SetKeep changes the file to delete");
    Check(session.AllFilesToDelete().size() == 1 && session.ExtraCount() == 1,
          L"all-extras matches the remaining copy");
    session.RemoveDeleted({L"D:\\new.bin"});
    Check(session.groups.empty(), L"groups drop when fewer than two files remain");

    app::DuplicateScanSession batched;
    batched.generation = 9;
    index::ContentSearchProgress batch_progress;
    batch_progress.generation = 9;
    batch_progress.done = false;
    index::ContentHit third = older;
    third.path = L"E:\\third.bin";
    third.name = L"third.bin";
    third.modified = 30;
    batched.ApplyUpdate(batch_progress, {older});
    Check(batched.groups.empty(), L"a partial duplicate group stays hidden until its second file");
    batched.ApplyUpdate(batch_progress, {newer});
    Check(batched.groups.size() == 1 && batched.groups[0].files.size() == 2,
          L"separate batches merge into one duplicate group");
    batched.SetKeep(0, 1);
    batched.ApplyUpdate(batch_progress, {third});
    Check(batched.groups[0].files.size() == 3 &&
              batched.groups[0].files[batched.groups[0].keep_index].path == L"C:\\old.bin",
          L"later batches preserve the chosen file to keep");

    app::DuplicateScanSession deletion;
    deletion.generation = 10;
    index::ContentSearchProgress deletion_progress;
    deletion_progress.generation = 10;
    index::ContentHit a = older;
    a.path = L"A:\\first.bin";
    a.name = L"first.bin";
    a.modified = 40;
    index::ContentHit b = older;
    b.path = L"B:\\second.bin";
    b.name = L"second.bin";
    b.modified = 30;
    index::ContentHit c = older;
    c.path = L"C:\\keeper.bin";
    c.name = L"keeper.bin";
    c.modified = 20;
    index::ContentHit d = older;
    d.path = L"D:\\last.bin";
    d.name = L"last.bin";
    d.modified = 10;
    deletion.ApplyUpdate(deletion_progress, {a, b, c, d});
    for (size_t i = 0; i < deletion.groups[0].files.size(); ++i) {
        if (deletion.groups[0].files[i].path == c.path) deletion.SetKeep(0, i);
    }
    deletion.RemoveDeleted({a.path});
    const auto kept_after_delete = deletion.FilesToDelete(0);
    Check(deletion.groups[0].files[deletion.groups[0].keep_index].path == c.path &&
              std::find(kept_after_delete.begin(), kept_after_delete.end(), c.path) == kept_after_delete.end(),
          L"deleting before the keeper preserves keeper identity");

    app::DuplicateScanSession reforming;
    reforming.generation = 11;
    index::ContentSearchProgress reform_progress;
    reform_progress.generation = 11;
    reforming.ApplyUpdate(reform_progress, {older, newer});
    reforming.RemoveDeleted({older.path});
    reforming.ApplyUpdate(reform_progress, {third});
    Check(reforming.groups.size() == 1 && reforming.groups[0].files.size() == 2 &&
              std::any_of(reforming.groups[0].files.begin(), reforming.groups[0].files.end(),
                          [&](const app::DuplicateFile& file) { return file.path == newer.path; }),
          L"a surviving file reforms its group after a later scan hit");

    app::DuplicateScanSession epoch;
    epoch.generation = 1;
    epoch.ApplyUpdate(progress, {older, newer});
    Check(epoch.result_epoch > 0, L"results bump the view epoch");
    const uint64_t after_hits = epoch.result_epoch;
    epoch.SetKeep(0, 1);
    Check(epoch.result_epoch > after_hits, L"keep changes bump the view epoch");
    const uint64_t after_keep = epoch.result_epoch;
    epoch.SetKeep(0, 1);
    Check(epoch.result_epoch == after_keep, L"unchanged keep leaves the view epoch");
    epoch.RemoveDeleted({L"D:\\new.bin"});
    Check(epoch.result_epoch > after_keep, L"deletes bump the view epoch");
    const uint64_t after_delete = epoch.result_epoch;
    epoch.ResetResults();
    Check(epoch.result_epoch > after_delete && epoch.groups.empty(),
          L"reset bumps the view epoch");

    wprintf(L"%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
