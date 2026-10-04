#pragma once

#include <cstdint>

namespace pulse::app {

struct BlankPaneClickRelease {
    bool pending = false;
    bool marquee_active = false;
    bool owns_capture = false;
    bool same_context = false;
    bool modified = false;
    bool blank_list_hit = false;
    int delta_x = 0;
    int delta_y = 0;
    int drag_width = 0;
    int drag_height = 0;
};

inline bool IsBlankPaneBackClick(const BlankPaneClickRelease& click) {
    const int64_t dx = click.delta_x;
    const int64_t dy = click.delta_y;
    return click.pending && !click.marquee_active && click.owns_capture &&
        click.same_context && !click.modified && click.blank_list_hit &&
        dx > -static_cast<int64_t>(click.drag_width) && dx < click.drag_width &&
        dy > -static_cast<int64_t>(click.drag_height) && dy < click.drag_height;
}

// AppPrefs::blank_click_action: what a double click on empty list space does.
inline constexpr int kBlankClickOff = 0;
inline constexpr int kBlankClickBack = 1;   // history first, the parent folder without it
inline constexpr int kBlankClickUp = 2;     // always the parent folder (B站 #11)

inline int NormalizeBlankClickAction(int action) {
    return action == kBlankClickBack || action == kBlankClickUp ? action : kBlankClickOff;
}

// False means: go to the parent folder, if there is one.
inline bool BlankClickGoesBack(int action, bool can_go_back) {
    return action == kBlankClickBack && can_go_back;
}

} // namespace pulse::app
