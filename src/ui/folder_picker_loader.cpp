#include "../common/windows_compat.h"
#include "folder_picker_loader.h"

#include "../common/localization.h"

#include <thread>

namespace pulse::ui {
namespace {

std::wstring DriveTypeName(UINT type) {
    switch (type) {
    case DRIVE_FIXED: return l10n::Get(l10n::StringId::LocalDisk);
    case DRIVE_REMOVABLE: return l10n::Get(l10n::StringId::RemovableDisk);
    case DRIVE_REMOTE: return l10n::Get(l10n::StringId::NetworkDrive);
    case DRIVE_CDROM: return l10n::Get(l10n::StringId::CdDrive);
    default: return l10n::Get(l10n::StringId::Drive);
    }
}

void ReadDrives(PickerListing& listing) {
    wchar_t roots[512]{};
    const DWORD length = GetLogicalDriveStringsW(ARRAYSIZE(roots) - 1, roots);
    if (length == 0 || length >= ARRAYSIZE(roots)) {
        listing.error = GetLastError();
        return;
    }
    // Empty card readers and optical drives must not raise "insert a disk".
    DWORD old_mode = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS, &old_mode);
    for (const wchar_t* root = roots; *root; root += wcslen(root) + 1) {
        const UINT type = GetDriveTypeW(root);
        if (type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN) continue;
        PickerEntry entry;
        entry.kind = PickerEntryKind::Drive;
        entry.path = root;
        wchar_t label[MAX_PATH + 1]{};
        const bool ready = GetVolumeInformationW(root, label, ARRAYSIZE(label), nullptr,
                                                 nullptr, nullptr, nullptr, 0) != FALSE;
        if (ready) {
            ULARGE_INTEGER free_bytes{}, total{};
            if (GetDiskFreeSpaceExW(root, &free_bytes, &total, nullptr)) {
                entry.size = total.QuadPart;
                entry.free = free_bytes.QuadPart;
            }
        }
        const std::wstring letter(root, 2);
        entry.name = (label[0] ? std::wstring(label) : DriveTypeName(type)) +
                     L" (" + letter + L")";
        listing.entries.push_back(std::move(entry));
    }
    SetThreadErrorMode(old_mode, nullptr);
}

void ReadFolder(PickerListing& listing, PickerMode mode) {
    std::wstring pattern = listing.path;
    if (!pattern.empty() && pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
                                   FindExSearchNameMatch, nullptr,
                                   FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        listing.error = GetLastError();
        // An empty drive root reports "no files" rather than an error.
        if (listing.error == ERROR_FILE_NOT_FOUND) {
            const DWORD attributes = GetFileAttributesW(listing.path.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & FILE_ATTRIBUTE_DIRECTORY))
                listing.error = ERROR_SUCCESS;
        }
        return;
    }
    const std::wstring prefix = listing.path.back() == L'\\' ? listing.path
                                                              : listing.path + L'\\';
    do {
        if (!PickerShowsEntry(data.dwFileAttributes, data.cFileName, mode)) continue;
        PickerEntry entry;
        entry.name = data.cFileName;
        entry.path = prefix + entry.name;
        entry.kind = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            ? PickerEntryKind::Folder : PickerEntryKind::Image;
        entry.modified = data.ftLastWriteTime;
        entry.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        listing.entries.push_back(std::move(entry));
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

} // namespace

PickerListing ReadPickerListing(const std::wstring& path, PickerMode mode) {
    PickerListing listing;
    listing.path = path;
    if (path.empty()) {
        ReadDrives(listing);
    } else {
        DWORD old_mode = 0;
        SetThreadErrorMode(SEM_FAILCRITICALERRORS, &old_mode);
        ReadFolder(listing, mode);
        SetThreadErrorMode(old_mode, nullptr);
    }
    SortPickerEntries(listing.entries);
    return listing;
}

PickerLoader::PickerLoader(HWND hwnd, UINT message) : target_(std::make_shared<Target>()) {
    target_->hwnd = hwnd;
    target_->message = message;
}

PickerLoader::~PickerLoader() {
    std::lock_guard<std::mutex> guard(target_->lock);
    target_->hwnd = nullptr;
}

void PickerLoader::Load(uint64_t generation, std::wstring path, PickerMode mode) {
    std::thread([target = target_, generation, path = std::move(path), mode]() {
        auto listing = std::make_unique<PickerListing>(ReadPickerListing(path, mode));
        listing->generation = generation;
        std::lock_guard<std::mutex> guard(target->lock);
        if (!target->hwnd) return;
        if (PostMessageW(target->hwnd, target->message, 0,
                         reinterpret_cast<LPARAM>(listing.get())))
            listing.release();
    }).detach();
}

std::unique_ptr<PickerListing> PickerLoader::Take(LPARAM lparam) {
    return std::unique_ptr<PickerListing>(reinterpret_cast<PickerListing*>(lparam));
}

} // namespace pulse::ui
