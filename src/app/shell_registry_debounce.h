#pragma once

// Coalesces bursts of shell-registry change notifications (HKCR / FileExts).
// Registry cleaners and installers touch thousands of keys in a row; each
// flush drops the context-menu caches and re-reads hundreds of extensions,
// so one flush per burst is enough. Pure logic, clocked by the caller.

#include <algorithm>
#include <cstdint>

namespace pulse::app {

class ShellRegistryDebounce {
public:
    static constexpr uint64_t kQuietMs = 1500;     // flush after this much silence
    static constexpr uint64_t kMaxDelayMs = 10000; // ...or at the latest this long after the first change
    static constexpr uint32_t kIdle = 0xFFFFFFFFu; // nothing pending (== INFINITE)

    void Note(uint64_t now) {
        if (!pending_) {
            pending_ = true;
            first_ = now;
        }
        last_ = now;
    }

    bool Pending() const { return pending_; }

    // Milliseconds until the pending flush is due; 0 when due, kIdle when idle.
    uint32_t WaitMs(uint64_t now) const {
        if (!pending_) return kIdle;
        const uint64_t due = (std::min)(last_ + kQuietMs, first_ + kMaxDelayMs);
        if (now >= due) return 0;
        return static_cast<uint32_t>((std::min)(due - now, static_cast<uint64_t>(kIdle - 1)));
    }

    // True once per burst, when its flush is due; the burst is then consumed.
    bool TakeDue(uint64_t now) {
        if (!pending_ || WaitMs(now) != 0) return false;
        pending_ = false;
        return true;
    }

private:
    bool pending_ = false;
    uint64_t first_ = 0;
    uint64_t last_ = 0;
};

} // namespace pulse::app
