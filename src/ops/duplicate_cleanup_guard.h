#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pulse::ops {

struct DuplicateCleanupFile {
    std::wstring path;
    uint64_t size = 0;
    uint64_t modified = 0;
};

struct DuplicateCleanupGroup {
    DuplicateCleanupFile keeper;
    std::vector<DuplicateCleanupFile> extras;
};

// Construct and retain on the operation worker through Shell completion.
// Keepers cannot be written or removed while the cleanup is in progress.
class DuplicateCleanupGuard {
public:
    DuplicateCleanupGuard() = default;
    ~DuplicateCleanupGuard();
    DuplicateCleanupGuard(const DuplicateCleanupGuard&) = delete;
    DuplicateCleanupGuard& operator=(const DuplicateCleanupGuard&) = delete;
    bool Validate(const std::vector<DuplicateCleanupGroup>& groups,
                  const std::vector<std::wstring>& sources,
                  const std::function<bool()>& cancelled);

private:
    HANDLE Open(const DuplicateCleanupFile& file, bool keeper);
    std::vector<HANDLE> handles_;
};

} // namespace pulse::ops
