#pragma once

// Reads folders for the picker off the UI thread. Each Load runs on its own
// short-lived thread and posts a PickerListing back; slow or dead network
// paths therefore never block the dialog, and results that arrive after the
// dialog closed are dropped.

#include "folder_picker_model.h"

#include <memory>
#include <mutex>

namespace pulse::ui {

class PickerLoader {
public:
    PickerLoader(HWND hwnd, UINT message);
    ~PickerLoader();
    PickerLoader(const PickerLoader&) = delete;
    PickerLoader& operator=(const PickerLoader&) = delete;

    // path "" lists the drives of This PC.
    void Load(uint64_t generation, std::wstring path, PickerMode mode);
    // Takes ownership of a posted listing (the message's LPARAM).
    static std::unique_ptr<PickerListing> Take(LPARAM lparam);

private:
    struct Target {
        std::mutex lock;
        HWND hwnd = nullptr;
        UINT message = 0;
    };
    std::shared_ptr<Target> target_;
};

// The blocking read itself, shared by the loader and tests.
PickerListing ReadPickerListing(const std::wstring& path, PickerMode mode);

} // namespace pulse::ui
