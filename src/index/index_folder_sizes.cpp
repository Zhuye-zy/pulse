#include "index_engine.h"
#include <algorithm>

namespace pulse::index {
FolderSizeIndex::Item Engine::FolderSizeItem(int32_t id) const {
    if (id < 0 || id >= LiveCount() || IsTomb(id)) return {};
    const auto n = NodeAt(id);
    return {n.parent, AttrAt(id).size, (n.flags & kFlagDir) != 0, true};
}
std::vector<IndexedFolderSize> Engine::FolderSizes(const std::vector<std::wstring>& paths) {
    if (paths.size() > kFolderSizeBatch) return {};
    std::vector<IndexedFolderSize> result(paths.size());
    std::unique_lock lock(mutex_);
    if (!ready_ || building_ || folder_size_gap_) return result;
    std::vector<int32_t> ids(paths.size(), -1);
    bool covered = false;
    for (size_t i = 0; i < paths.size(); ++i) {
        const auto path = NormalizeChangePath(paths[i]);
        const auto id = ResolvePathLocked(path);
        if (id < 0 || IsTomb(id) || !(NodeAt(id).flags & kFlagDir) ||
            (NodeAt(id).flags & kFlagHidden) || IsExcludedPath(path)) continue;
        int32_t root = id;
        int32_t remaining = LiveCount();
        while (root >= 0 && root < LiveCount() && remaining-- > 0 && NodeAt(root).parent >= 0) root = NodeAt(root).parent;
        if (root < 0 || root >= LiveCount() || remaining <= 0 || std::find(inactive_volume_roots_.begin(), inactive_volume_roots_.end(), root) != inactive_volume_roots_.end()) continue;
        for (const auto& volume : vols_) if (volume.root_idx == root && volume.journal_id && volume.folder_size_current) {
            ids[i] = id; covered = true; break;
        }
    }
    if (!covered) return result;
    if (!folder_sizes_.Valid()) {
        if (GetTickCount64() < folder_size_retry_after_) return result;
        const auto started = GetTickCount64();
        if (!folder_sizes_.Build(LiveCount(), [this](int32_t id) { return FolderSizeItem(id); })) {
            diagnostics::runtime::Event("index_folder_totals_failed", {{"nodes", static_cast<uint64_t>(LiveCount())},
                {"elapsed_ms", GetTickCount64() - started}, {"retry_ms", 30000}});
            // Incomplete/cyclic metadata must stay unknown, without rescanning
            // millions of nodes on every client poll. Snapshot replacement
            // resets this delay; ordinary journal edits retry within 30 seconds.
            folder_size_retry_after_ = GetTickCount64() + 30000;
            return result;
        }
        diagnostics::runtime::Event("index_folder_totals_ready", {{"nodes", static_cast<uint64_t>(LiveCount())},
            {"elapsed_ms", GetTickCount64() - started}});
        folder_size_retry_after_ = 0;
    }
    for (size_t i = 0; i < ids.size(); ++i) if (const auto bytes = folder_sizes_.Get(ids[i])) result[i] = {true, *bytes};
    return result;
}
}
