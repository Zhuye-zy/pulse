#include "folder_sizes.h"
#include "../fs/fs_enum.h"
#include "../fs/fs_watch.h"
#include "../common/json_utils.h"
#include "../common/utf8_file.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <string_view>
#include <limits>
#include <set>
#include "../common/path_utils.h"
#include "../index/index_feed.h"

namespace pulse::app {
namespace {
constexpr size_t kCacheLimit = 4096;
constexpr ULONGLONG kFreshMs = 60000;
std::wstring Key(std::wstring path) {
    path = fs::NormalizePath(std::move(path));
    while (path.size() > 7 && path.back() == L'\\') path.pop_back();
    for (auto& c : path) c = static_cast<wchar_t>(std::towlower(c));
    return path;
}
bool Within(const std::wstring& path, const std::wstring& parent) {
    return path.starts_with(parent) && (path.size() == parent.size() ||
        (!parent.empty() && parent.back() == L'\\') || path[parent.size()] == L'\\');
}
bool AddBytes(uint64_t& total, uint64_t bytes) {
    if (bytes > UINT64_MAX - total) return false;
    total += bytes;
    return true;
}
struct Scan {
    struct Frame {
        std::wstring path;
        HANDLE find = INVALID_HANDLE_VALUE;
        WIN32_FIND_DATAW data{};
        uint64_t bytes = 0;
        bool started = false, partial = false, read_any = false;
    };
    std::wstring path;
    uint64_t epoch = 0;
    std::vector<Frame> stack;
    FolderSizeValue result{FolderSizeState::Calculating};
    bool done = false;
    explicit Scan(std::wstring p, uint64_t e) : path(std::move(p)), epoch(e) {
        stack.push_back(Frame{path});
    }
    ~Scan() { for (auto& f : stack) if (f.find != INVALID_HANDLE_VALUE) FindClose(f.find); }
    void FinishFrame() {
        auto f = std::move(stack.back());
        stack.pop_back();
        if (f.find != INVALID_HANDLE_VALUE) FindClose(f.find);
        if (stack.empty()) {
            done = true;
            result.bytes = f.bytes;
            result.has_value = f.read_any;
            result.state = !f.read_any ? FolderSizeState::Unavailable :
                (f.partial ? FolderSizeState::Partial : FolderSizeState::Ready);
        } else {
            auto& parent = stack.back();
            const bool added = AddBytes(parent.bytes, f.bytes);
            parent.partial |= f.partial || !added;
        }
    }
    // Round-robin slices keep a large first folder from starving its siblings.
    void Step(const std::atomic<bool>& stopping) {
        const auto deadline = GetTickCount64() + 12;
        unsigned processed = 0;
        while (!done && !stopping && GetTickCount64() < deadline && processed++ < 256) {
            auto& f = stack.back();
            bool available = false;
            if (!f.started) {
                f.started = true;
                const DWORD attrs = GetFileAttributesW(f.path.c_str());
                if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                    f.partial = true; FinishFrame(); continue;
                }
                // Never traverse junctions/symlinks or hydrate cloud directories.
                if (attrs & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE)) {
                    f.partial = true; FinishFrame(); continue;
                }
                f.find = FindFirstFileExW((f.path + L"\\*").c_str(), FindExInfoBasic,
                    &f.data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
                available = f.find != INVALID_HANDLE_VALUE;
                if (!available) {
                    const DWORD error = GetLastError();
                    f.read_any = error == ERROR_FILE_NOT_FOUND || error == ERROR_NO_MORE_FILES;
                    f.partial = !f.read_any;
                    FinishFrame(); continue;
                }
                f.read_any = true;
            } else {
                available = FindNextFileW(f.find, &f.data) != FALSE;
                if (!available) {
                    f.partial |= GetLastError() != ERROR_NO_MORE_FILES;
                    FinishFrame(); continue;
                }
            }
            const auto& d = f.data;
            if (wcscmp(d.cFileName, L".") == 0 || wcscmp(d.cFileName, L"..") == 0) continue;
            if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if ((d.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE)) ||
                    stack.size() >= 128) { f.partial = true; continue; }
                const auto child = f.path + L"\\" + d.cFileName;
                stack.push_back(Frame{child});
            } else {
                // Logical size includes cloud placeholders without opening content.
                const uint64_t bytes = (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
                f.partial |= !AddBytes(f.bytes, bytes);
            }
        }
    }
};
}

struct FolderSizes::Impl {
    struct Entry {
        FolderSizeValue value;
        ULONGLONG completed = 0, used = 0, not_before = 0;
        uint64_t epoch = 0;
    };
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::map<std::wstring, Entry> cache;
    std::map<std::wstring, bool> wanted;
    std::set<std::wstring> manual, blocked_auto;
    std::vector<std::wstring> roots;
    // The previous Sync arguments: repainting the same rows changes nothing.
    std::vector<FolderSizeRequest> last_visible;
    std::vector<std::wstring> last_roots;
    std::function<std::wstring()> cache_path;
    std::thread worker;
    std::atomic<bool> stopping{false}, changed{false};
    bool dirty = false;
    bool index_enabled = true;
    HANDLE index_cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~Impl() { if (index_cancel) CloseHandle(index_cancel); }

    void Invalidate(const std::wstring& path) {
        const auto key = Key(path);
        std::lock_guard lock(mutex);
        for (auto& [p, e] : cache) if (Within(key, p) || Within(p, key)) {
            e.completed = 0; ++e.epoch;
            e.not_before = GetTickCount64() + 200;
            e.value.state = e.value.has_value ? FolderSizeState::Updating : FolderSizeState::Calculating;
        }
        changed = true;
        wake.notify_one();
    }
    void Load(const std::wstring& file) {
        std::wstring text;
        if (file.empty() || !ReadUtf8File(file, text)) return;
        std::lock_guard lock(mutex);
        for (const auto& record : json::ExtractStringArray(text, L"folders")) {
            const auto tab = record.find(L'\t');
            if (tab == std::wstring::npos || tab + 3 >= record.size() || record[tab + 2] != L'\t') continue;
            wchar_t* end = nullptr;
            const auto bytes = wcstoull(record.c_str(), &end, 10);
            if (end != record.c_str() + tab) continue;
            const auto path = Key(record.substr(tab + 3));
            if (path.empty() || fs::IsVirtualPath(path)) continue;
            auto& e = cache[path];
            e.value = {FolderSizeState::Updating, bytes, true};
            if (cache.size() >= kCacheLimit) break;
        }
        changed = true;
    }
    void Save(const std::wstring& file) {
        if (file.empty()) return;
        std::wstring text = L"{\"folders\":[";
        {
            std::lock_guard lock(mutex);
            if (!dirty) return;
            bool first = true;
            for (const auto& [p, e] : cache) {
                if (!e.value.has_value) continue;
                if (!first) text += L",";
                first = false;
                text += L"\"";
                json::Escape(std::to_wstring(e.value.bytes) + L"\t" +
                    (e.value.state == FolderSizeState::Partial ? L"1\t" : L"0\t") + p, text);
                text += L"\"";
            }
            dirty = false;
        }
        text += L"]}";
        WriteUtf8FileAtomic(file, text);
    }
    void Run() {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        const auto file = cache_path ? cache_path() : std::wstring();
        const auto cache_key = file.empty() ? std::wstring() : Key(file);
        Load(file);
        std::map<std::wstring, std::unique_ptr<fs::DirWatch>> watches;
        std::map<std::wstring, UINT> drive_types;
        auto driveType = [&](const std::wstring& p) {
            const auto plain = path::StripExtendedPathPrefix(p);
            if (plain.size() < 3 || plain[1] != L':') return static_cast<UINT>(DRIVE_REMOTE);
            const auto root = plain.substr(0, 3);
            auto it = drive_types.find(root);
            if (it == drive_types.end()) it = drive_types.emplace(root, GetDriveTypeW(root.c_str())).first;
            return it->second;
        };
        std::deque<std::unique_ptr<Scan>> jobs;
        index::IndexFeedConnection connection(index_cancel);
        ULONGLONG next_index = 0;
        size_t index_cursor = 0;
        std::set<std::wstring> indexed_paths;
        ULONGLONG last_save = GetTickCount64();
        while (!stopping) {
            std::vector<std::wstring> desired_roots;
            std::vector<std::wstring> classify;
            {
                std::lock_guard lock(mutex);
                for (const auto& [p, automatic] : wanted) {
                    if (automatic && !manual.contains(p)) classify.push_back(p);
                }
            }
            for (const auto& p : classify) {
                const UINT type = driveType(p);
                if (type != DRIVE_FIXED && type != DRIVE_RAMDISK) {
                    std::lock_guard lock(mutex);
                    blocked_auto.insert(p);
                    if (!manual.contains(p) && wanted.contains(p)) wanted[p] = false;
                    changed = true;
                }
            }
            if (index_enabled && !stopping && GetTickCount64() >= next_index) {
                std::vector<std::wstring> paths;
                std::vector<uint64_t> epochs;
                {
                    std::lock_guard lock(mutex);
                    if (!stopping) ResetEvent(index_cancel);
                    for (const auto& [p, automatic] : wanted) {
                        if (!automatic || manual.contains(p) || fs::IsUncPath(p)) continue;
                        paths.push_back(p); epochs.push_back(cache[p].epoch);
                    }
                    std::erase_if(indexed_paths, [&](const auto& p) { return !wanted.contains(p); });
                    // Multiple panes can exceed one protocol batch. Rotate so
                    // every visible folder continues receiving index updates.
                    if (!paths.empty()) {
                        index_cursor %= paths.size();
                        std::rotate(paths.begin(), paths.begin() + index_cursor, paths.end());
                        std::rotate(epochs.begin(), epochs.begin() + index_cursor, epochs.end());
                        index_cursor = (index_cursor + index::kFolderSizeBatch) % paths.size();
                        if (paths.size() > index::kFolderSizeBatch) {
                            paths.resize(index::kFolderSizeBatch); epochs.resize(index::kFolderSizeBatch);
                        }
                    }
                }
                std::vector<index::IndexedFolderSize> values;
                const bool received = !paths.empty() && connection.FolderSizes(paths, values);
                next_index = GetTickCount64() + (received || paths.empty() ? 1000 : 10000);
                std::lock_guard lock(mutex);
                for (size_t i = 0; i < paths.size(); ++i) {
                    auto found = cache.find(paths[i]);
                    if (found == cache.end() || !wanted.contains(paths[i]) ||
                        found->second.epoch != epochs[i] || manual.contains(paths[i])) continue;
                    auto& e = found->second;
                    if (received && values[i].available) {
                        if (e.value.state != FolderSizeState::Indexed || e.value.bytes != values[i].bytes) changed = true;
                        e.value = {FolderSizeState::Indexed, values[i].bytes, true};
                        e.completed = GetTickCount64();
                        indexed_paths.insert(paths[i]);
                        dirty = true;
                        std::erase_if(jobs, [&](const auto& job) { return job->path == paths[i]; });
                    } else if (indexed_paths.erase(paths[i]) || e.value.state == FolderSizeState::Indexed) {
                        e.completed = 0;
                        e.value.state = FolderSizeState::Updating;
                        changed = true;
                    }
                }
            }
            {
                std::unique_lock lock(mutex);
                std::erase_if(jobs, [&](const auto& j) {
                    const auto found = wanted.find(j->path);
                    return found == wanted.end() || !found->second || cache[j->path].epoch != j->epoch;
                });
                const auto now = GetTickCount64();
                for (const auto& [p, automatic] : wanted) {
                    if (!automatic) continue;
                    auto& e = cache[p];
                    if (indexed_paths.contains(p) && !manual.contains(p)) continue;
                    if (now < e.not_before) continue;
                    const bool watched = std::any_of(watches.begin(), watches.end(), [&](const auto& w) {
                        return w.second->Armed() && Within(p, w.first);
                    });
                    if (e.completed && (watched || now - e.completed < kFreshMs)) continue;
                    if (std::any_of(jobs.begin(), jobs.end(), [&](const auto& j) { return j->path == p; })) continue;
                    e.value.state = e.value.has_value ? FolderSizeState::Updating : FolderSizeState::Calculating;
                    if (jobs.size() >= 32) break;
                    jobs.push_back(std::make_unique<Scan>(p, e.epoch));
                    changed = true;
                }
                desired_roots = roots;
                if (jobs.empty()) wake.wait_for(lock, std::chrono::milliseconds(250));
            }
            std::erase_if(watches, [&](const auto& pair) {
                const bool remove = std::find(desired_roots.begin(), desired_roots.end(), pair.first) == desired_roots.end();
                if (remove) Invalidate(pair.first);
                return remove;
            });
            for (const auto& root : desired_roots) if (!watches.contains(root)) {
                if (driveType(root) == DRIVE_REMOTE) continue;
                Invalidate(root);
                auto watch = std::make_unique<fs::DirWatch>();
                watch->Start(root, [this, root, cache_key](bool overflow, std::vector<fs::DirNotifyEvent> events) {
                    if (overflow) Invalidate(root);
                    else for (const auto& event : events) {
                        const auto changed_path = Key(root + L"\\" + event.name);
                        // Do not create a scan/save/watch feedback loop in our data folder.
                        if (!cache_key.empty() && (changed_path == cache_key || changed_path == cache_key + L".tmp")) continue;
                        Invalidate(root + L"\\" + event.name);
                        if (!event.old_name.empty()) Invalidate(root + L"\\" + event.old_name);
                    }
                }, true);
                watches.emplace(root, std::move(watch));
            }
            if (!jobs.empty() && !stopping) {
                auto job = std::move(jobs.front()); jobs.pop_front();
                job->Step(stopping);
                if (job->done) {
                    std::lock_guard lock(mutex);
                    auto it = cache.find(job->path);
                    if (it != cache.end() && it->second.epoch == job->epoch && wanted.contains(job->path)) {
                        auto& e = it->second;
                        // A failed refresh retains the cached value and labels it incomplete.
                        if (!job->result.has_value && e.value.has_value) e.value.state = FolderSizeState::Cached;
                        else e.value = job->result;
                        e.completed = GetTickCount64();
                        dirty = true; changed = true;
                    }
                } else jobs.push_back(std::move(job));
                // Keep metadata enumeration from monopolizing a busy disk.
                std::unique_lock lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(4), [&] { return stopping.load(); });
            }
            if (GetTickCount64() - last_save >= 3000) { Save(file); last_save = GetTickCount64(); }
        }
        watches.clear();
        Save(file);
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    }
};

FolderSizes::FolderSizes() : impl_(std::make_unique<Impl>()) {}
FolderSizes::~FolderSizes() { Stop(); }
void FolderSizes::SetCachePath(std::function<std::wstring()> path) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->worker.joinable()) impl_->cache_path = std::move(path);
}
void FolderSizes::SetIndexEnabled(bool enabled) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->worker.joinable()) impl_->index_enabled = enabled;
}
void FolderSizes::Sync(std::vector<FolderSizeRequest> visible, std::vector<std::wstring> roots) {
    auto& s = *impl_;
    std::lock_guard lock(s.mutex);
    if (s.stopping) return;
    // Every paint syncs, and a Size sort sends each folder of the listing (#58).
    const auto same_request = [](const FolderSizeRequest& a, const FolderSizeRequest& b) {
        return a.automatic == b.automatic && a.path == b.path;
    };
    if (s.cache.size() <= kCacheLimit && roots == s.last_roots &&
        std::equal(visible.begin(), visible.end(), s.last_visible.begin(), s.last_visible.end(), same_request))
        return;
    s.last_visible = visible;
    s.last_roots = roots;
    std::map<std::wstring, bool> wanted;
    for (const auto& request : visible) {
        const auto key = Key(request.path);
        wanted[key] = (request.automatic && !s.blocked_auto.contains(key)) || s.manual.contains(key);
        s.cache[key].used = GetTickCount64();
    }
    bool cancelled = false;
    for (const auto& [p, automatic] : s.wanted) if (!wanted.contains(p)) {
        ++s.cache[p].epoch;
        s.manual.erase(p);
        cancelled |= automatic;
    }
    s.wanted = std::move(wanted);
    for (auto& root : roots) root = Key(root);
    std::sort(roots.begin(), roots.end()); roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    s.roots = std::move(roots);
    while (s.cache.size() > kCacheLimit) {
        auto oldest = s.cache.end();
        for (auto it = s.cache.begin(); it != s.cache.end(); ++it)
            if (!s.wanted.contains(it->first) && (oldest == s.cache.end() || it->second.used < oldest->second.used)) oldest = it;
        if (oldest == s.cache.end()) break;
        s.cache.erase(oldest);
    }
    if (!s.worker.joinable() && !s.wanted.empty()) s.worker = std::thread([&s] { s.Run(); });
    if (cancelled && s.worker.joinable()) CancelSynchronousIo(s.worker.native_handle());
    if (cancelled && s.index_cancel) SetEvent(s.index_cancel);
    s.wake.notify_one();
}
void FolderSizes::Calculate(const std::wstring& path) {
    auto& s = *impl_;
    std::lock_guard lock(s.mutex);
    const auto key = Key(path);
    auto it = s.wanted.find(key);
    if (it == s.wanted.end()) return;
    it->second = true;
    s.manual.insert(key);
    auto& e = s.cache[key]; e.completed = 0; e.not_before = 0; ++e.epoch;
    e.value.state = e.value.has_value ? FolderSizeState::Updating : FolderSizeState::Calculating;
    s.changed = true; s.wake.notify_one();
}
FolderSizeValue FolderSizes::Get(const std::wstring& path) const {
    auto& s = *impl_;
    std::lock_guard lock(s.mutex);
    const auto key = Key(path);
    const auto it = s.cache.find(key);
    auto value = it == s.cache.end() ? FolderSizeValue{} : it->second.value;
    const auto wanted = s.wanted.find(key);
    if (wanted != s.wanted.end() && !wanted->second) {
        if (value.state == FolderSizeState::Calculating) value.state = FolderSizeState::Manual;
        if (value.state == FolderSizeState::Updating) value.state = FolderSizeState::Cached;
    }
    if (!value.has_value && value.state == FolderSizeState::Manual) {
        const auto request = s.wanted.find(key);
        if (request != s.wanted.end() && request->second) value.state = FolderSizeState::Calculating;
    }
    return value;
}
std::unordered_map<std::wstring, uint64_t> FolderSizes::KnownChildren(const std::wstring& parent) const {
    std::unordered_map<std::wstring, uint64_t> children;
    std::wstring prefix = Key(parent);
    if (prefix.empty() || fs::IsVirtualPath(prefix)) return children;
    if (prefix.back() != L'\\') prefix += L'\\';
    auto& s = *impl_;
    std::lock_guard lock(s.mutex);
    for (auto it = s.cache.lower_bound(prefix); it != s.cache.end() && it->first.starts_with(prefix); ++it) {
        if (!it->second.value.has_value) continue;
        std::wstring_view rest(it->first);
        rest.remove_prefix(prefix.size());
        while (!rest.empty() && rest.front() == L'\\') rest.remove_prefix(1);
        if (rest.empty() || rest.find(L'\\') != std::wstring_view::npos) continue;
        children.emplace(std::wstring(rest), it->second.value.bytes);
    }
    return children;
}
void FolderSizes::Invalidate(const std::wstring& path) { impl_->Invalidate(path); }
bool FolderSizes::TakeChanged() { return impl_->changed.exchange(false); }
void FolderSizes::Stop() {
    auto& s = *impl_; s.stopping = true; s.wake.notify_all();
    if (s.index_cancel) SetEvent(s.index_cancel);
    if (s.worker.joinable()) { CancelSynchronousIo(s.worker.native_handle()); s.worker.join(); }
}
}
