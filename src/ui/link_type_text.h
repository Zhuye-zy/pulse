#pragma once
#include "../fs/fs_enum.h"
#include "../common/localization.h"

namespace pulse::ui {

inline std::wstring LinkTypeText(fs::LinkKind kind) {
    if (kind == fs::LinkKind::SymbolicLink) return l10n::Pick(L"符号链接", L"Symbolic link");
    if (kind == fs::LinkKind::Junction) return l10n::Pick(L"目录联接", L"Junction");
    return {};
}

} // namespace pulse::ui
