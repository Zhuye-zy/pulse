// fs_watch.h — ReadDirectoryChangesW directory watcher.
// Overlapped reads return ERROR_IO_PENDING until a change arrives; that is
// success, not a reason to reopen the directory. CreateFile runs on the
// watch thread so UNC/SMB cannot stall the UI.
#pragma once
#include <windows.h>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pulse::fs {

struct DirNotifyEvent {
    DWORD action = 0;
    std::wstring name;
    std::wstring old_name;
};

class DirWatch {
public:
    using ChangeCallback = std::function<void(bool overflow, std::vector<DirNotifyEvent> events)>;

    DirWatch();
    ~DirWatch();

    bool Start(const std::wstring& path, ChangeCallback cb, bool subtree = false);
    void Stop();
    bool Armed() const;
    const std::wstring& path() const { return path_; }

private:
    struct State;
    std::shared_ptr<State> state_;
    std::wstring path_;
    std::thread thread_;

};

class DirWatchSet {
public:
    using Callback = std::function<void(const std::wstring& path, bool overflow,
                                        std::vector<DirNotifyEvent> events)>;

    void Sync(const std::vector<std::wstring>& paths, Callback cb);
    void Stop();
    bool Armed(const std::wstring& path) const;

private:
    std::mutex mutex_;
    Callback callback_;
    std::unordered_map<std::wstring, std::unique_ptr<DirWatch>> watches_;
};

} // namespace pulse::fs
