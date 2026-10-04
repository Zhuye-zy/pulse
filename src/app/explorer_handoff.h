#pragma once

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app {

enum class HandoffState { Pending, Ready, Cancelled, Expired, Closed };

// Shared across the UI and the Explorer STA; it never owns COM interfaces.
struct ExplorerHandoff {
    explicit ExplorerHandoff(uint64_t deadline) : deadline_tick(deadline) {}
    const uint64_t deadline_tick;
    std::atomic<HandoffState> state{HandoffState::Pending};
    std::atomic<bool> received{false};

    bool Receive(uint64_t now) {
        if (now >= deadline_tick) { Cancel(HandoffState::Expired); return false; }
        return state == HandoffState::Pending && !received.exchange(true);
    }

    bool Ready(uint64_t now) {
        if (now >= deadline_tick) { Cancel(HandoffState::Expired); return false; }
        auto expected = HandoffState::Pending;
        return state.compare_exchange_strong(expected, HandoffState::Ready);
    }
    void Cancel(HandoffState reason = HandoffState::Cancelled) {
        auto current = state.load();
        while ((current == HandoffState::Pending || current == HandoffState::Ready) &&
               !state.compare_exchange_weak(current, reason)) {}
    }
    bool ClaimClose(uint64_t now) {
        if (now >= deadline_tick) { Cancel(HandoffState::Expired); return false; }
        auto expected = HandoffState::Ready;
        return state.compare_exchange_strong(expected, HandoffState::Closed);
    }
};

// A tab owns the lease: closing it or replacing its navigation cancels a
// pending handoff, including a Ready acknowledgement not consumed by the STA.
struct ExplorerNavigationLease {
    std::shared_ptr<ExplorerHandoff> handoff;
    std::wstring path;
    uint64_t generation = 0;
    std::vector<std::wstring> names;
    bool Complete(std::wstring_view current_path, uint64_t current_generation, bool success,
                  std::vector<std::wstring> selected, uint64_t now) {
        if (!handoff || generation != current_generation) return false;
        auto expected = names;
        std::sort(expected.begin(), expected.end());
        std::sort(selected.begin(), selected.end());
        if (!success || path != current_path || expected != selected) {
            handoff->Cancel();
            return false;
        }
        return handoff->Ready(now);
    }
    ~ExplorerNavigationLease() { if (handoff) handoff->Cancel(); }
};

} // namespace pulse::app
