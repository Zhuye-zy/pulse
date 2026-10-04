// file_lock_owner.cpp — Restart Manager lookup of processes holding a file.
#include "file_lock_owner.h"

#include <restartmanager.h>
#include <winerror.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <iterator>
#include <system_error>

#pragma comment(lib, "rstrtmgr.lib")

namespace pulse::ops {
namespace {

// sherrors.h values; spelled out so this file does not pull in the Shell headers.
constexpr HRESULT kCopyEngineAccessDeniedSrc = static_cast<HRESULT>(0x80270021L);
constexpr HRESULT kCopyEngineAccessDeniedDest = static_cast<HRESULT>(0x80270022L);
constexpr HRESULT kCopyEngineSharingViolationSrc = static_cast<HRESULT>(0x80270027L);
constexpr HRESULT kCopyEngineSharingViolationDest = static_cast<HRESULT>(0x80270028L);

constexpr HRESULT Win32Hr(DWORD code) noexcept {
    return static_cast<HRESULT>((code & 0x0000FFFFu) | (static_cast<DWORD>(FACILITY_WIN32) << 16)
                                | 0x80000000u);
}

bool LooksLikePath(const std::wstring& text) {
    if (text.size() >= 3 && iswalpha(text[0]) && text[1] == L':' &&
        (text[2] == L'\\' || text[2] == L'/'))
        return true;
    return text.size() >= 3 && text[0] == L'\\' && text[1] == L'\\';
}

std::wstring Trim(std::wstring text) {
    const auto not_space = [](wchar_t c) { return !iswspace(c); };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
    return text;
}

std::wstring FileNamePart(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring ParentPart(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

bool SameFolder(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return false;
    std::wstring left = a, right = b;
    while (!left.empty() && (left.back() == L'\\' || left.back() == L'/')) left.pop_back();
    while (!right.empty() && (right.back() == L'\\' || right.back() == L'/')) right.pop_back();
    return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()),
                                right.c_str(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

std::wstring ProcessImagePath(HANDLE process) {
    std::wstring buffer(32768, L'\0');
    DWORD size = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameW(process, 0, buffer.data(), &size)) return {};
    buffer.resize(size);
    return buffer;
}

} // namespace

bool IsLockLikeError(HRESULT hr) noexcept {
    switch (hr) {
    case Win32Hr(ERROR_SHARING_VIOLATION):
    case Win32Hr(ERROR_LOCK_VIOLATION):
    case Win32Hr(ERROR_ACCESS_DENIED):
    case Win32Hr(ERROR_USER_MAPPED_FILE):
    case Win32Hr(ERROR_DIR_NOT_EMPTY):
    case kCopyEngineSharingViolationSrc:
    case kCopyEngineSharingViolationDest:
    case kCopyEngineAccessDeniedSrc:
    case kCopyEngineAccessDeniedDest:
        return true;
    default:
        return false;
    }
}

std::wstring FailedPathFromError(const std::wstring& error) {
    const size_t marker = error.rfind(L" | ");
    std::wstring tail = Trim(marker == std::wstring::npos ? error : error.substr(marker + 3));
    return LooksLikePath(tail) ? tail : std::wstring();
}

std::vector<std::wstring> LockProbeFiles(const std::wstring& path, size_t limit) {
    std::vector<std::wstring> files;
    if (path.empty() || limit == 0) return files;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return files;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        files.push_back(path);
        return files;
    }
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return files;
    namespace fsys = std::filesystem;
    std::error_code ec;
    fsys::recursive_directory_iterator it(fsys::path(path),
        fsys::directory_options::skip_permission_denied, ec);
    const fsys::recursive_directory_iterator end;
    while (!ec && it != end && files.size() < limit) {
        const DWORD child = GetFileAttributesW(it->path().c_str());
        if (child != INVALID_FILE_ATTRIBUTES) {
            if (child & FILE_ATTRIBUTE_DIRECTORY) {
                if (child & FILE_ATTRIBUTE_REPARSE_POINT) it.disable_recursion_pending();
            } else {
                files.push_back(it->path().wstring());
            }
        }
        it.increment(ec);
    }
    return files;
}

std::vector<LockOwner> FindLockOwners(const std::vector<std::wstring>& files,
                                      const std::wstring& protected_dir) {
    std::vector<LockOwner> owners;
    if (files.empty()) return owners;
    DWORD session = 0;
    WCHAR key[CCH_RM_SESSION_KEY + 1]{};
    if (RmStartSession(&session, 0, key) != ERROR_SUCCESS) return owners;

    std::vector<LPCWSTR> names;
    names.reserve(files.size());
    for (const auto& file : files) names.push_back(file.c_str());
    std::vector<RM_PROCESS_INFO> infos;
    if (RmRegisterResources(session, static_cast<UINT>(names.size()), names.data(),
                            0, nullptr, 0, nullptr) == ERROR_SUCCESS) {
        UINT needed = 0;
        UINT count = 0;
        DWORD reasons = 0;
        DWORD result = ERROR_MORE_DATA;
        for (int attempt = 0; attempt < 4 && result == ERROR_MORE_DATA; ++attempt) {
            count = static_cast<UINT>(infos.size());
            result = RmGetList(session, &needed, &count, infos.empty() ? nullptr : infos.data(),
                               &reasons);
            if (result == ERROR_MORE_DATA) infos.resize(static_cast<size_t>(needed) + 2);
        }
        if (result == ERROR_SUCCESS) infos.resize(count);
        else infos.clear();
    }
    RmEndSession(session);

    const DWORD self = GetCurrentProcessId();
    for (const auto& info : infos) {
        const DWORD pid = info.Process.dwProcessId;
        if (pid == 0) continue;
        if (std::any_of(owners.begin(), owners.end(),
                        [pid](const LockOwner& known) { return known.pid == pid; }))
            continue;
        LockOwner owner;
        owner.pid = pid;
        owner.start_time = info.Process.ProcessStartTime;
        owner.app_name = info.strAppName;
        std::wstring image_path;
        if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            image_path = ProcessImagePath(process);
            CloseHandle(process);
        }
        owner.image_name = FileNamePart(image_path);
        owner.closable = pid != self && pid > 4
            && info.ApplicationType != RmCritical
            && info.ApplicationType != RmService
            && info.ApplicationType != RmExplorer
            && !(image_path.empty() ? false : SameFolder(ParentPart(image_path), protected_dir));
        owners.push_back(std::move(owner));
    }
    return owners;
}

LockReport ProbeLockFailure(HRESULT hr, const std::wstring& error,
                            const std::vector<std::wstring>& sources,
                            const std::wstring& protected_dir, size_t limit) {
    LockReport report;
    if (!IsLockLikeError(hr)) return report;
    std::vector<std::wstring> files;
    const std::wstring full = FailedPathFromError(error);
    if (!full.empty() && GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES) {
        report.path = full;
        files = LockProbeFiles(full, limit);
    } else {
        const size_t marker = error.rfind(L" | ");
        const std::wstring name = Trim(marker == std::wstring::npos ? error : error.substr(marker + 3));
        for (const auto& source : sources) {
            if (name.empty() || name.find_first_of(L"\\/") != std::wstring::npos) break;
            const std::wstring source_name = FileNamePart(source);
            if (CompareStringOrdinal(source_name.c_str(), -1, name.c_str(), -1, TRUE) == CSTR_EQUAL) {
                report.path = source;
                break;
            }
        }
        if (!report.path.empty()) {
            files = LockProbeFiles(report.path, limit);
        } else {
            for (const auto& source : sources) {
                if (files.size() >= limit) break;
                auto more = LockProbeFiles(source, limit - files.size());
                files.insert(files.end(), std::make_move_iterator(more.begin()),
                             std::make_move_iterator(more.end()));
            }
            // The reported name may be a file inside a source folder; the error
            // may also be a bare message. Only a probed file can name the item.
            for (const auto& file : files) {
                if (name.empty()) break;
                const std::wstring file_name = FileNamePart(file);
                if (CompareStringOrdinal(file_name.c_str(), -1, name.c_str(), -1, TRUE) == CSTR_EQUAL) {
                    report.path = file;
                    break;
                }
            }
            if (report.path.empty() && !sources.empty()) report.path = sources.front();
        }
    }
    if (files.empty()) {
        report.path.clear();
        return report;
    }
    report.owners = FindLockOwners(files, protected_dir);
    if (report.owners.empty()) report.path.clear();
    return report;
}

CloseOwnerResult CloseLockOwner(const LockOwner& owner, DWORD wait_ms, DWORD* error) {
    if (error) *error = ERROR_SUCCESS;
    if (!owner.closable || owner.pid == 0 || owner.pid == GetCurrentProcessId())
        return CloseOwnerResult::Refused;
    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                 FALSE, owner.pid);
    if (!process) {
        const DWORD code = GetLastError();
        if (error) *error = code;
        return code == ERROR_INVALID_PARAMETER ? CloseOwnerResult::AlreadyGone
                                               : CloseOwnerResult::Denied;
    }
    CloseOwnerResult result = CloseOwnerResult::Closed;
    FILETIME created{}, exited{}, kernel{}, user{};
    if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
        result = CloseOwnerResult::AlreadyGone;
    } else if (!GetProcessTimes(process, &created, &exited, &kernel, &user) ||
               CompareFileTime(&created, &owner.start_time) != 0) {
        // The pid was reused by another process since Restart Manager saw it.
        result = CloseOwnerResult::AlreadyGone;
    } else if (!TerminateProcess(process, 1)) {
        if (error) *error = GetLastError();
        result = CloseOwnerResult::Denied;
    } else if (WaitForSingleObject(process, wait_ms) != WAIT_OBJECT_0) {
        result = CloseOwnerResult::TimedOut;
    }
    CloseHandle(process);
    return result;
}

std::wstring DescribeLockOwner(const LockOwner& owner) {
    std::wstring text = owner.app_name.empty() ? owner.image_name : owner.app_name;
    std::wstring detail;
    if (!owner.image_name.empty() &&
        CompareStringOrdinal(owner.image_name.c_str(), -1, text.c_str(), -1, TRUE) != CSTR_EQUAL)
        detail = owner.image_name + L", ";
    detail += L"PID " + std::to_wstring(owner.pid);
    if (text.empty()) return detail;
    return text + L" (" + detail + L")";
}

std::wstring FormatLockOwnerLines(const std::vector<LockOwner>& owners, size_t max_lines) {
    std::wstring text;
    const size_t shown = (std::min)(owners.size(), max_lines);
    for (size_t i = 0; i < shown; ++i) {
        if (!text.empty()) text += L"\n";
        text += L"\x2022 " + DescribeLockOwner(owners[i]);
    }
    if (owners.size() > shown) text += L"\n\x2026";
    return text;
}

std::wstring CurrentModuleDirectory() {
    std::wstring buffer(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size == 0 || size >= buffer.size()) return {};
    buffer.resize(size);
    return ParentPart(buffer);
}

} // namespace pulse::ops
