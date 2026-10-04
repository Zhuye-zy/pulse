#pragma once

#include "../ui/view_layout.h"
#include <map>
#include <optional>
#include <string>

namespace pulse::app {

// Normalized key for per-folder preferences; empty for virtual views and
// nonabsolute paths, which are never persisted.
std::wstring FolderPrefKey(const std::wstring& path);

// This PC (the empty path) has no folder key but keeps its own choice, like
// File Explorer; other virtual views are still never remembered.
class FolderViewPrefs {
public:
    std::optional<ui::ViewMode> Find(const std::wstring& path) const;
    bool Set(const std::wstring& path, ui::ViewMode mode);
    // Mode for folders without a saved choice.
    ui::ViewMode Default() const { return default_; }
    // "Apply to all folders": `mode` becomes the default, per-folder choices go.
    // This PC is not a folder, so its own choice stays.
    void ApplyToAll(ui::ViewMode mode);
    void Clear() {
        views_.clear();
        this_pc_.reset();
        default_ = ui::ViewMode::Details;
    }
    void AppendJson(std::wstring& out) const;
    void ReadJson(const std::wstring& json);

private:
    struct PathLess {
        bool operator()(const std::wstring& a, const std::wstring& b) const;
    };
    std::map<std::wstring, ui::ViewMode, PathLess> views_;
    std::optional<ui::ViewMode> this_pc_;
    ui::ViewMode default_ = ui::ViewMode::Details;
};

} // namespace pulse::app
