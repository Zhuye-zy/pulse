#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <string>
#include "index_memory_probe.h"
#include "index_maintenance_probe.h"
#include "../common/runtime_log.h"

namespace pulse::index {
enum class FilenameStage : size_t { Wait, Topology, Journal, Notify, DeltaFlush, Merge, Rebuild, Recovery, NamePoolCompact, Count };

// Opt-in worker-owned counters; when enabled, an idle worker writes once per minute. The fixed file
// name lives beside the index and is excluded by the index-artifact filter.
class FilenameTiming {
public:
    struct Token { uint64_t wall = 0, cpu = 0; };
    static Token Begin() noexcept;
    void End(FilenameStage stage, Token token, uint64_t changes = 0,
             DWORD error = ERROR_SUCCESS, const char* reason = "none", wchar_t volume = 0) noexcept;
    void Flush(bool force = false) noexcept;
    bool Due() const noexcept { return (IndexDiagnosticsEnabled() || diagnostics::runtime::Enabled()) &&
        (!last_flush_ || GetTickCount64() - last_flush_ >= 60000); }
    IndexMemoryProbe& Memory() noexcept { return memory_; }
    IndexMaintenanceProbe& Maintenance() noexcept { return maintenance_; }
private:
    struct Counter {
        uint64_t calls = 0, wall_us = 0, cpu_us = 0, changes = 0, errors = 0;
        DWORD last_error = 0;
        wchar_t volume = 0;
        const char* reason = "none";
    };
    std::array<Counter, static_cast<size_t>(FilenameStage::Count)> counters_{};
    uint64_t last_flush_ = 0;
    IndexMemoryProbe memory_;
    IndexMaintenanceProbe maintenance_;
};
}
