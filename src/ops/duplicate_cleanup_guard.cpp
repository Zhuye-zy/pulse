#include "duplicate_cleanup_guard.h"
#include <algorithm>
#include <array>

namespace pulse::ops {
namespace {
std::wstring Win32Path(const std::wstring& value) {
    if (value.starts_with(L"\\\\?\\")) return value;
    if (value.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + value.substr(2);
    if (value.size() >= 3 && value[1] == L':' && value[2] == L'\\') return L"\\\\?\\" + value;
    return value;
}
}

DuplicateCleanupGuard::~DuplicateCleanupGuard() {
    for (HANDLE handle : handles_) CloseHandle(handle);
}

HANDLE DuplicateCleanupGuard::Open(const DuplicateCleanupFile& file, bool keeper) {
    const DWORD share = FILE_SHARE_READ | (keeper ? 0 : FILE_SHARE_DELETE);
    HANDLE handle = CreateFileW(Win32Path(file.path).c_str(), GENERIC_READ,
        share, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return handle;
    BY_HANDLE_FILE_INFORMATION info{};
    const bool valid = GetFileInformationByHandle(handle, &info) &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
        ((static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow) == file.size &&
        ((static_cast<uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
            info.ftLastWriteTime.dwLowDateTime) == file.modified;
    if (!valid) {
        CloseHandle(handle);
        return INVALID_HANDLE_VALUE;
    }
    handles_.push_back(handle);
    return handle;
}

bool DuplicateCleanupGuard::Validate(const std::vector<DuplicateCleanupGroup>& groups,
                                     const std::vector<std::wstring>& sources,
                                     const std::function<bool()>& cancelled) {
    if (groups.empty() || sources.empty() || !handles_.empty()) return false;
    size_t source = 0;
    std::array<unsigned char, 64 * 1024> kept_bytes{}, extra_bytes{};
    for (const auto& group : groups) {
        if (cancelled() || group.extras.empty()) return false;
        HANDLE keeper = Open(group.keeper, true);
        if (keeper == INVALID_HANDLE_VALUE) return false;
        for (const auto& extra : group.extras) {
            if (cancelled() || source >= sources.size() || sources[source++] != extra.path ||
                extra.size != group.keeper.size) return false;
            HANDLE candidate = Open(extra, false);
            if (candidate == INVALID_HANDLE_VALUE) return false;
            LARGE_INTEGER beginning{};
            if (!SetFilePointerEx(keeper, beginning, nullptr, FILE_BEGIN)) return false;
            for (;;) {
                if (cancelled()) return false;
                DWORD kept_read = 0, extra_read = 0;
                if (!ReadFile(keeper, kept_bytes.data(), static_cast<DWORD>(kept_bytes.size()), &kept_read, nullptr) ||
                    !ReadFile(candidate, extra_bytes.data(), static_cast<DWORD>(extra_bytes.size()), &extra_read, nullptr) ||
                    kept_read != extra_read ||
                    !std::equal(kept_bytes.begin(), kept_bytes.begin() + kept_read, extra_bytes.begin())) return false;
                if (!kept_read) break;
            }
        }
    }
    return source == sources.size() && !cancelled();
}

} // namespace pulse::ops
