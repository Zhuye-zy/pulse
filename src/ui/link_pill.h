#pragma once
#include "ui_motion.h"
#include <string>

namespace pulse::ui {
inline bool IsShortcutName(const std::wstring& name) {
    return name.size() >= 4 &&
        (_wcsicmp(name.c_str() + name.size() - 4, L".lnk") == 0 ||
         _wcsicmp(name.c_str() + name.size() - 4, L".url") == 0);
}

// Only transitioning/expanded visible links retain state. Navigation and a
// discontinuous frame discard old targets, so idle never keeps rendering.
class LinkPillMotion {
public:
    float Update(uint64_t context, uint64_t key, bool expanded, uint64_t frame,
                 uint64_t now, bool animate) {
        if (context != context_) { states_.clear(); context_ = context; }
        if (frame != frame_) {
            std::erase_if(states_, [frame](const auto& item) { return item.second.frame + 1 < frame; });
            frame_ = frame;
        }
        auto found = states_.find(key);
        if (found == states_.end()) {
            if (!expanded) return 0.0f;
            if (states_.size() >= 2048) return 1.0f;
            found = states_.emplace(key, State{animate && motion::SystemAnimationsEnabled() ? 0.0f : 1.0f, 1, now, frame}).first;
        }
        auto& state = found->second;
        const float current = Sample(state, now);
        const float target = expanded ? 1.0f : 0.0f;
        if (state.target != target) {
            state.from = animate && motion::SystemAnimationsEnabled() ? current : target;
            state.target = target; state.start = now;
        }
        if (!animate) state.from = state.target;
        state.frame = frame;
        const float result = Sample(state, now);
        if (!expanded && result <= 0.0f) states_.erase(found);
        return result;
    }
    bool Active(uint64_t now) const {
        for (const auto& [key, state] : states_)
            if (state.from != state.target && now - state.start < 220 + motion::kSettleGraceMs) return true;
        return false;
    }
private:
    struct State { float from, target; uint64_t start, frame; };
    static float Sample(const State& state, uint64_t now) {
        return state.from + (state.target - state.from) *
            motion::EaseOutCubic(static_cast<float>(now - state.start) / 220.0f);
    }
    uint64_t context_ = 0, frame_ = 0;
    std::unordered_map<uint64_t, State> states_;
};

inline D2D1_RECT_F LinkPillRect(const D2D1_RECT_F& artwork, float diameter,
                               float right_limit, float text_width, float expansion) {
    const float width = diameter + std::max(0.0f, std::min(text_width,
        right_limit - artwork.left - diameter)) * std::clamp(expansion, 0.0f, 1.0f);
    return {artwork.left, std::max(artwork.top, artwork.bottom - diameter),
            artwork.left + width, artwork.bottom};
}
} // namespace pulse::ui
