#include "last_tab_close.h"

namespace pulse::app {

bool LastTabClosesWindow(size_t tab_count, bool tab_pinned, bool enabled) noexcept {
    return enabled && tab_count == 1 && !tab_pinned;
}

} // namespace pulse::app
