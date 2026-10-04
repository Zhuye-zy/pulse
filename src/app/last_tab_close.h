#pragma once

#include <cstddef>

namespace pulse::app {

// Settings > Startup and close: closing the only tab closes the window, like a
// browser. Off by default; pinned tabs never close this way. The window then
// follows the normal close path (it hides to the tray when Pulse keeps running).
bool LastTabClosesWindow(size_t tab_count, bool tab_pinned, bool enabled) noexcept;

} // namespace pulse::app
