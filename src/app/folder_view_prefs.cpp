#include "folder_view_prefs.h"
#include "../common/json_utils.h"
#include "../common/path_utils.h"
#include <algorithm>
#include <string>

namespace pulse::app {

std::wstring FolderPrefKey(const std::wstring& path) {
    if (path.empty() || path.starts_with(L"pulse:")) return {};
    std::wstring key = path::StripExtendedPathPrefix(path);
    std::replace(key.begin(), key.end(), L'/', L'\\');
    // Accept only absolute filesystem paths; virtual views have separate rules.
    if (!(key.size() >= 3 && key[1] == L':' && key[2] == L'\\') &&
        !key.starts_with(L"\\\\")) return {};
    while (key.size() > 3 && key.back() == L'\\') key.pop_back();
    return key;
}

namespace {

std::wstring JsonKey(ui::ViewMode mode) {
    return std::wstring(L"folder_view_") + ui::ViewModeName(mode);
}

} // namespace

bool FolderViewPrefs::PathLess::operator()(const std::wstring& a, const std::wstring& b) const {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
                                b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
}

std::optional<ui::ViewMode> FolderViewPrefs::Find(const std::wstring& path) const {
    if (path.empty()) return this_pc_;
    const auto key = FolderPrefKey(path);
    if (key.empty()) return std::nullopt;
    const auto it = views_.find(key);
    return it == views_.end() ? std::nullopt : std::optional(it->second);
}

bool FolderViewPrefs::Set(const std::wstring& path, ui::ViewMode mode) {
    if (static_cast<unsigned>(mode) >= 8) return false;
    if (path.empty()) {
        if (this_pc_ == mode) return false;
        this_pc_ = mode;
        return true;
    }
    const auto key = FolderPrefKey(path);
    if (key.empty()) return false;
    const auto [it, inserted] = views_.try_emplace(key, mode);
    if (!inserted && it->second == mode) return false;
    it->second = mode;
    return true;
}

void FolderViewPrefs::ApplyToAll(ui::ViewMode mode) {
    views_.clear();
    default_ = ui::ViewModeFromIndex(ui::ViewModeIndex(mode));
}

void FolderViewPrefs::AppendJson(std::wstring& out) const {
    out += L",\n  \"folder_view_default\":" + std::to_wstring(ui::ViewModeIndex(default_));
    out += L",\n  \"folder_view_this_pc\":" +
           std::to_wstring(this_pc_ ? ui::ViewModeIndex(*this_pc_) : -1);
    // Group paths by stable view names, using the existing string-array codec.
    for (int i = 0; i < 8; ++i) {
        const auto mode = ui::ViewModeFromIndex(i);
        out += L",\n  \"" + JsonKey(mode) + L"\":[";
        bool first = true;
        for (const auto& [path, saved_mode] : views_) {
            if (saved_mode != mode) continue;
            if (!first) out += L",";
            first = false;
            out += L"\"";
            json::Escape(path, out);
            out += L"\"";
        }
        out += L"]";
    }
}

void FolderViewPrefs::ReadJson(const std::wstring& input) {
    Clear();
    default_ = ui::ViewModeFromIndex(json::ExtractInt(
        input, L"folder_view_default", ui::ViewModeIndex(ui::ViewMode::Details)));
    const int this_pc = json::ExtractInt(input, L"folder_view_this_pc", -1);
    if (this_pc >= 0 && this_pc < 8) this_pc_ = ui::ViewModeFromIndex(this_pc);
    for (int i = 0; i < 8; ++i) {
        const auto mode = ui::ViewModeFromIndex(i);
        for (const auto& path : json::ExtractStringArray(input, JsonKey(mode)))
            Set(path, mode);
    }
}

} // namespace pulse::app
