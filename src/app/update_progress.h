#pragma once
#include <cstdint>

namespace pulse::app {
enum class UpdatePhase { Idle, Connecting, Downloading, Verifying, Ready, WaitingOperations, Launching, Installing };
struct UpdateProgress {
    UpdatePhase phase = UpdatePhase::Idle;
    uint64_t received_bytes = 0;
    uint64_t total_bytes = 0; // Zero means unknown, not an estimated package size.
    bool active() const noexcept { return phase != UpdatePhase::Idle; }
    int percent() const noexcept {
        if (phase != UpdatePhase::Downloading || !total_bytes || received_bytes > total_bytes) return -1;
        // Exact floor(received * 100 / total), without overflow or floating-point boundary errors.
        int low = 0, high = 100;
        while (low < high) {
            const int middle = (low + high + 1) / 2;
            const auto threshold = (total_bytes / 100) * middle + ((total_bytes % 100) * middle + 99) / 100;
            if (received_bytes >= threshold) low = middle;
            else high = middle - 1;
        }
        return low;
    }
};
}
