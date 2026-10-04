#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pulse::index {
enum class HierarchyError : uint8_t { None, Count, ParentRange, SelfParent, FileRoot, ParentType, DeletedParent, Cycle };
struct HierarchyIssue {
    HierarchyError error = HierarchyError::None;
    int32_t node = -1, parent = -1;
};
// Validate relationships, not only byte ranges: a structurally readable cache
// can still contain stale global node IDs from an older delta replay.
template<class Read>
bool ValidateIndexHierarchy(int32_t count, Read read, HierarchyIssue* issue = nullptr) {
    if (issue) *issue = {};
    auto fail = [&](HierarchyError error, int32_t node, int32_t parent) {
        if (issue) *issue = {error, node, parent};
        return false;
    };
    if (count < 0) return fail(HierarchyError::Count, -1, -1);
    constexpr uint8_t directory = 1, deleted = 4;
    for (int32_t i = 0; i < count; ++i) {
        const auto n = read(i);
        if (n.flags & deleted) continue;
        if (n.parent < -1 || n.parent >= count) return fail(HierarchyError::ParentRange, i, n.parent);
        if (n.parent == i) return fail(HierarchyError::SelfParent, i, n.parent);
        if (n.parent < 0) {
            if (!(n.flags & directory)) return fail(HierarchyError::FileRoot, i, n.parent);
        } else {
            const auto parent = read(n.parent);
            if (!(parent.flags & directory)) return fail(HierarchyError::ParentType, i, n.parent);
            if (parent.flags & deleted) return fail(HierarchyError::DeletedParent, i, n.parent);
        }
    }
    std::vector<uint8_t> state(static_cast<std::size_t>(count), 0);
    for (int32_t i = 0; i < count; ++i) {
        if (state[i] || (read(i).flags & deleted)) continue;
        int32_t cursor = i;
        while (cursor >= 0 && state[cursor] == 0) {
            state[cursor] = 1;
            cursor = read(cursor).parent;
        }
        if (cursor >= 0 && state[cursor] == 1) return fail(HierarchyError::Cycle, cursor, read(cursor).parent);
        cursor = i;
        while (cursor >= 0 && state[cursor] == 1) {
            state[cursor] = 2;
            cursor = read(cursor).parent;
        }
    }
    return true;
}
}
