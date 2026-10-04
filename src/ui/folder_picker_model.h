#pragma once

// Pure state of the Pulse folder / picture picker: paths, filtering, sorting
// and what the primary button would pick. No windows, no file system calls.

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::ui {

enum class PickerMode { Folder, Image };
enum class PickerEntryKind { Folder, Image, Drive };

struct PickerEntry {
    std::wstring name;   // display name ("Local Disk (C:)" for drives)
    std::wstring path;   // full path
    PickerEntryKind kind = PickerEntryKind::Folder;
    FILETIME modified{};
    uint64_t size = 0;   // file size; total bytes for drives
    uint64_t free = 0;   // drives only
};

// One folder's contents as read by the loader. path "" is This PC (drives).
struct PickerListing {
    uint64_t generation = 0;
    std::wstring path;
    std::vector<PickerEntry> entries;
    DWORD error = ERROR_SUCCESS;
};

bool IsPickerImageName(std::wstring_view name);
// Hidden and system items, "." and "..", and (in picture mode) other files
// stay out of the list.
bool PickerShowsEntry(DWORD attributes, std::wstring_view name, PickerMode mode);
// Folders first, then names in Explorer's numeric-aware order.
void SortPickerEntries(std::vector<PickerEntry>& entries);

// "C:\a\b" -> "C:\a"; a drive or share root -> "" (This PC).
std::wstring PickerParent(std::wstring_view path);
// Typed text to a path: trims spaces and quotes, expands %VARS%, accepts
// "/" and "C:", and drops trailing separators except on roots.
std::wstring NormalizePickerInput(std::wstring_view text);
bool SamePickerPath(std::wstring_view a, std::wstring_view b);

// What the primary button returns: the selected folder or picture, else the
// current folder in folder mode. Empty = nothing to pick (button disabled).
std::wstring PickerChosenPath(PickerMode mode, std::wstring_view current,
                              const PickerEntry* selected);

// Next entry after `from` whose name starts with `ch` (wraps); -1 if none.
int PickerTypeAhead(const std::vector<PickerEntry>& entries, int from, wchar_t ch);

// Back stack: Navigate pushes the old location, Back pops it.
class PickerHistory {
public:
    void Navigate(std::wstring from);
    bool CanGoBack() const { return !back_.empty(); }
    std::wstring Back();

private:
    std::vector<std::wstring> back_;
};

} // namespace pulse::ui
