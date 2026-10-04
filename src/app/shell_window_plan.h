// shell_window_plan.h — Which Pulse panes the shell sees as folder windows
// (B站 #1 phase 2a), and what has to change when panes navigate or close.
//
// With Pulse as the default file manager, "open file location" in other
// programs (SHOpenFolderAndSelectItems) looks up IShellWindows for a window
// showing the folder and asks it to select the item. ShellWindowRegistry
// registers each pane there; this header holds the pure planning part.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::app {

struct ShellWindowEntry {
    uint64_t key = 0;      // pane identity
    std::wstring path;     // folder shown; empty = This PC
    bool operator==(const ShellWindowEntry&) const = default;
};

enum class ShellWindowActionKind { Register, Navigate, Revoke };

struct ShellWindowAction {
    ShellWindowActionKind kind = ShellWindowActionKind::Register;
    uint64_t key = 0;
    std::wstring path;
    bool operator==(const ShellWindowAction&) const = default;
};

// A drive or UNC folder, or This PC (empty). Pulse's own views (pulse:…
// search, settings, recycle bin) have no shell folder to report. Expects the
// plain form: strip \\?\ first (path::StripExtendedPathPrefix).
bool IsShellWindowPath(const std::wstring& path);

// Revokes first (keys that are gone), then registrations and navigations in
// `wanted` order. Paths compare case-insensitively.
std::vector<ShellWindowAction> PlanShellWindowChanges(
    const std::vector<ShellWindowEntry>& current, const std::vector<ShellWindowEntry>& wanted);

// Posted to the UI thread when the shell asks a pane to select an item.
struct ShellSelectRequest {
    uint64_t key = 0;
    std::wstring path;     // absolute parsing name of the item
    unsigned flags = 0;    // SVSIF
};

// Splits an item path into its folder and leaf ("C:\a\b.txt" -> "C:\a",
// "b.txt"; "C:\a" -> "C:\", "a"). False for a drive root or anything without
// a parent folder.
bool SplitShellItemPath(const std::wstring& path, std::wstring& folder, std::wstring& leaf);

// B站 #1 phase 2b: a File Explorer window opened behind Pulse's back
// (explorer.exe /select,… run directly). What one poll of it found:
struct ExplorerWindowProbe {
    unsigned age_ms = 0;          // since the window registered
    bool view_ready = false;      // its folder view exists and reports a folder
    bool supported = false;       // a file system folder or This PC
    size_t selected = 0;          // items selected in it
};

enum class ExplorerTakeoverStep { Wait, Take, Leave };

constexpr unsigned kExplorerViewTimeoutMs = 4000;     // no view by then: leave it
constexpr unsigned kExplorerSelectionGraceMs = 400;   // /select applies after the view

// Wait for the view, leave virtual locations (Control Panel, Home, network…)
// alone, and give /select a moment to arrive before taking the window.
ExplorerTakeoverStep DecideExplorerTakeover(const ExplorerWindowProbe& probe);

} // namespace pulse::app
