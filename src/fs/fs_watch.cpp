// fs_watch.cpp
#include "fs_watch.h"
#include "fs_enum.h"
#include <algorithm>

namespace pulse::fs {

namespace {

std::vector<DirNotifyEvent> ParseNotifyBuffer(const BYTE* data, DWORD bytes,
                                              std::wstring& pending_old, bool emit_old) {
    std::vector<DirNotifyEvent> out;
    if (!data || bytes < sizeof(FILE_NOTIFY_INFORMATION)) return out;
    const BYTE* p = data;
    const BYTE* end = data + bytes;
    while (p + sizeof(FILE_NOTIFY_INFORMATION) <= end) {
        auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
        const size_t name_chars = info->FileNameLength / sizeof(WCHAR);
        const BYTE* name_end = reinterpret_cast<const BYTE*>(info->FileName) + info->FileNameLength;
        if (name_end > end) break;
        std::wstring name(info->FileName, name_chars);
        if (info->Action == FILE_ACTION_RENAMED_OLD_NAME) {
            pending_old = std::move(name);
            if (emit_old) out.push_back({FILE_ACTION_RENAMED_OLD_NAME, pending_old, {}});
        } else if (info->Action == FILE_ACTION_RENAMED_NEW_NAME) {
            DirNotifyEvent ev;
            ev.action = FILE_ACTION_RENAMED_NEW_NAME;
            ev.name = std::move(name);
            ev.old_name = std::move(pending_old);
            pending_old.clear();
            out.push_back(std::move(ev));
        } else {
            DirNotifyEvent ev;
            ev.action = info->Action;
            ev.name = std::move(name);
            out.push_back(std::move(ev));
        }
        if (info->NextEntryOffset == 0) break;
        p += info->NextEntryOffset;
    }
    return out;
}

} // namespace

namespace { std::atomic<unsigned> active_watchers{0}; }

struct DirWatch::State {
    std::wstring path_;
    bool subtree_ = false;
    ChangeCallback callback_;
    std::mutex callback_mutex_;
    std::atomic<HANDLE> hDir_{INVALID_HANDLE_VALUE};
    HANDLE hStop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<bool> running_{true};
    std::atomic<bool> armed_{false};
    OVERLAPPED overlapped_{};
    BY_HANDLE_FILE_INFORMATION identity_{};
    bool identity_valid_ = false;
    std::wstring pending_rename_old_;
    DWORD buffer_[64 * 1024 / sizeof(DWORD)]{};
    ~State() {
        const HANDLE directory = hDir_.load();
        if (directory != INVALID_HANDLE_VALUE) CloseHandle(directory);
        if (hStop_) CloseHandle(hStop_);
        --active_watchers;
    }
    static HANDLE OpenDirectory(const std::wstring& path);
    static bool ReadIdentity(HANDLE handle, BY_HANDLE_FILE_INFORMATION& identity);
    bool WatchedPathReplaced() const;
    bool ReopenDirectory();
    void Notify(bool overflow, std::vector<DirNotifyEvent> events);
    void WorkerThread();
};

DirWatch::DirWatch() = default;
DirWatch::~DirWatch() { Stop(); }

bool DirWatch::Start(const std::wstring& path, ChangeCallback cb, bool subtree) {
    Stop();
    if (path.empty()) return false;
    // Bound retired workers even when a network provider ignores cancellation.
    if (active_watchers.fetch_add(1) >= 128) { --active_watchers; return false; }
    std::shared_ptr<State> state;
    try { state = std::make_shared<State>(); }
    catch (...) { --active_watchers; return false; }
    if (!state->hStop_) return false;
    path_ = NormalizePath(path);
    state->path_ = path_;
    state->subtree_ = subtree;
    state->callback_ = std::move(cb);
    try { thread_ = std::thread([state] { state->WorkerThread(); }); }
    catch (...) { return false; }
    state_ = std::move(state);
    return true;
}

bool DirWatch::Armed() const { return state_ && state_->armed_.load(); }

void DirWatch::Stop() {
    if (!state_) return;
    state_->running_ = false;
    SetEvent(state_->hStop_);
    {
        // Callbacks only dispatch notifications. Quiesce them before owners die.
        std::lock_guard lock(state_->callback_mutex_);
        state_->callback_ = {};
    }
    if (thread_.joinable()) {
        CancelSynchronousIo(thread_.native_handle());
        thread_.detach();
    }
    state_.reset(); // The worker owns handles and pending OVERLAPPED until drained.
}

HANDLE DirWatch::State::OpenDirectory(const std::wstring& path) {
    return CreateFileW(
        path.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);
}

bool DirWatch::State::ReadIdentity(HANDLE handle, BY_HANDLE_FILE_INFORMATION& identity) {
    return handle != INVALID_HANDLE_VALUE &&
           GetFileInformationByHandle(handle, &identity) != FALSE;
}

bool DirWatch::State::WatchedPathReplaced() const {
    if (!identity_valid_) return false;
    HANDLE current = OpenDirectory(path_);
    if (current == INVALID_HANDLE_VALUE) return true;
    BY_HANDLE_FILE_INFORMATION identity{};
    const bool readable = ReadIdentity(current, identity);
    CloseHandle(current);
    if (!readable) return true;
    return identity.dwVolumeSerialNumber != identity_.dwVolumeSerialNumber ||
           identity.nFileIndexHigh != identity_.nFileIndexHigh ||
           identity.nFileIndexLow != identity_.nFileIndexLow;
}

bool DirWatch::State::ReopenDirectory() {
    armed_ = false;
    const HANDLE old = hDir_.exchange(INVALID_HANDLE_VALUE, std::memory_order_acq_rel);
    if (old != INVALID_HANDLE_VALUE) CloseHandle(old);
    identity_valid_ = false;

    while (running_) {
        HANDLE directory = OpenDirectory(path_);
        if (directory != INVALID_HANDLE_VALUE) {
            BY_HANDLE_FILE_INFORMATION identity{};
            if (ReadIdentity(directory, identity)) {
                identity_ = identity;
                identity_valid_ = true;
                hDir_.store(directory, std::memory_order_release);
                return true;
            }
            CloseHandle(directory);
        }
        if (WaitForSingleObject(hStop_, 1000) == WAIT_OBJECT_0) break;
    }
    return false;
}

void DirWatch::State::Notify(bool overflow, std::vector<DirNotifyEvent> events) {
    std::lock_guard lock(callback_mutex_);
    if (!running_ || !callback_) return;
    try {
        callback_(overflow, std::move(events));
    } catch (...) {
    }
}

void DirWatch::State::WorkerThread() {
    if (!ReopenDirectory()) return;
    bool newly_armed = true;
    while (running_) {
        ZeroMemory(&overlapped_, sizeof(overlapped_));
        overlapped_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped_.hEvent) break;
        HANDLE events[2] = { hStop_, overlapped_.hEvent };
        const HANDLE directory = hDir_.load(std::memory_order_acquire);
        if (directory == INVALID_HANDLE_VALUE) {
            CloseHandle(overlapped_.hEvent);
            break;
        }

        BOOL ok = ReadDirectoryChangesW(
            directory,
            buffer_,
            static_cast<DWORD>(sizeof(buffer_)),
            subtree_ ? TRUE : FALSE,
            FILE_NOTIFY_CHANGE_FILE_NAME |
                FILE_NOTIFY_CHANGE_DIR_NAME |
                FILE_NOTIFY_CHANGE_ATTRIBUTES |
                FILE_NOTIFY_CHANGE_SIZE |
                FILE_NOTIFY_CHANGE_LAST_WRITE |
                FILE_NOTIFY_CHANGE_CREATION |
                FILE_NOTIFY_CHANGE_SECURITY,
            nullptr,
            &overlapped_,
            nullptr);

        if (!ok && GetLastError() != ERROR_IO_PENDING) {
            CloseHandle(overlapped_.hEvent);
            if (!running_) break;
            pending_rename_old_.clear();
            Notify(true, {});
            if (!ReopenDirectory()) break;
            newly_armed = true;
            continue;
        }

        // An initial/reopened watch may have missed changes while
        // the handle was opening. Reconcile only after the read is armed.
        armed_ = true;
        if (newly_armed) Notify(true, {});
        newly_armed = false;

        bool replaced = false;
        bool stopped = false;
        for (;;) {
            const DWORD wait = WaitForMultipleObjects(2, events, FALSE, 500);
            if (wait == WAIT_TIMEOUT) {
                if (!WatchedPathReplaced()) continue;
                CancelIoEx(directory, &overlapped_);
                WaitForSingleObject(overlapped_.hEvent, INFINITE);
                replaced = true;
                break;
            }
            if (wait == WAIT_OBJECT_0) {
                CancelIoEx(directory, &overlapped_);
                WaitForSingleObject(overlapped_.hEvent, INFINITE);
                stopped = true;
                break;
            }
            if (wait != WAIT_OBJECT_0 + 1) {
                CancelIoEx(directory, &overlapped_);
                WaitForSingleObject(overlapped_.hEvent, INFINITE);
            }
            break;
        }

        DWORD transferred = 0;
        const BOOL got = GetOverlappedResult(directory, &overlapped_, &transferred, FALSE);
        CloseHandle(overlapped_.hEvent);
        overlapped_.hEvent = nullptr;

        if (stopped || !running_) break;

        if (replaced) {
            pending_rename_old_.clear();
            Notify(true, {});
            if (!ReopenDirectory()) break;
            newly_armed = true;
            continue;
        }

        if (!got) {
            const DWORD err = GetLastError();
            if (err == ERROR_OPERATION_ABORTED) continue;
            pending_rename_old_.clear();
            Notify(true, {});
            if (err != ERROR_NOTIFY_ENUM_DIR && !ReopenDirectory()) break;
            newly_armed = true;
            continue;
        }

        if (transferred == 0) {
            pending_rename_old_.clear();
            Notify(true, {});
            continue;
        }
        Notify(false, ParseNotifyBuffer(reinterpret_cast<const BYTE*>(buffer_), transferred,
                                        pending_rename_old_, subtree_));
    }
}

void DirWatchSet::Sync(const std::vector<std::wstring>& paths, Callback cb) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = std::move(cb);
    }
    std::vector<std::wstring> wanted;
    wanted.reserve(paths.size());
    for (const auto& path : paths) {
        if (path.empty() || IsVirtualPath(path)) continue;
        wanted.push_back(NormalizePath(path));
    }
    std::sort(wanted.begin(), wanted.end());
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());

    for (auto it = watches_.begin(); it != watches_.end();) {
        if (std::binary_search(wanted.begin(), wanted.end(), it->first)) ++it;
        else it = watches_.erase(it);
    }
    for (const auto& path : wanted) {
        if (watches_.contains(path)) continue;
        auto watch = std::make_unique<DirWatch>();
        const std::wstring key = path;
        if (!watch->Start(path, [this, key](bool overflow, std::vector<DirNotifyEvent> events) {
            Callback invoke;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                invoke = callback_;
            }
            if (invoke) invoke(key, overflow, std::move(events));
        })) {
            continue;
        }
        watches_[path] = std::move(watch);
    }
}

void DirWatchSet::Stop() {
    watches_.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = {};
}

bool DirWatchSet::Armed(const std::wstring& path) const {
    const auto it = watches_.find(NormalizePath(path));
    return it != watches_.end() && it->second && it->second->Armed();
}

} // namespace pulse::fs
