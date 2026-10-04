#pragma once

#include <string>

namespace pulse::app {

// A failed save must remain dirty so a later close can retry it.
template<typename Writer>
bool SaveChangedSession(const std::wstring& json, std::wstring& saved,
                        bool force, Writer&& write) {
    if (!force && json == saved) return true;
    if (!write(json)) return false;
    saved = json;
    return true;
}

} // namespace pulse::app
