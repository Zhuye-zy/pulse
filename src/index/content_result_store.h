#pragma once
#include "content_search.h"
#include "../fs/fs_enum.h"
#include <functional>
#include <memory>
#include <vector>

namespace pulse::index {
// Per-query disk spool. Only a bounded set of display pages lives in memory.
// Append is called by the IPC reader, never by the window thread.
class ContentResultStore {
public:
    struct Row { fs::DirEntry entry; std::wstring snippet; uint64_t file_id=0; };
    using Filter = std::function<bool(const fs::DirEntry&)>;
    struct Selection {
        std::vector<std::pair<size_t, Row>> rows;
        std::vector<int> matches;
        DWORD error = 0;
    };
    static constexpr size_t kPageSize = 256, kCachePages = 8;
    ContentResultStore(HWND notify, UINT message);
    ~ContentResultStore();
    bool Append(const std::vector<ContentHit>& hits);
    bool StreamUpsert(const std::vector<ContentHit>& hits, ContentResultSort sort, bool descending);
    bool ApplyChanges(const std::vector<ContentHit>& hits, ContentResultSort sort, bool descending);
    size_t Count() const;
    size_t RawCount() const;
    bool SameContents(const ContentResultStore& other, HANDLE cancel) const;
    uint64_t Revision() const;
    uint64_t OrderRevision() const;
    DWORD Error() const;
    bool Filtering() const;
    bool Sorting() const;
    bool Get(size_t index, Row& row) const;
    bool Ready(size_t index) const;
    void Prefetch(size_t index) const;
    void SetFilter(Filter filter);
    void SetSort(ContentResultSort sort, bool descending);
    void Resolve(std::vector<int> indices, bool all, size_t count,
                 std::function<void(Selection)> complete, uint64_t expected_order = UINT64_MAX);
    void FindPaths(std::vector<std::wstring> paths, std::function<void(std::vector<int>)> complete);
    void FindPath(std::wstring path,std::function<void(int)> complete);
    void FindIdentity(uint64_t file_id,std::function<void(int)> complete);
    void Match(Filter filter,std::function<void(Selection)> complete);
    void Sum(std::vector<int> indices,bool all,size_t count,std::function<void(uint64_t,DWORD)> complete);
    size_t CachedRows() const;
    std::wstring CachePath() const;
private:
    bool UpsertChanges(const std::vector<ContentHit>& hits, ContentResultSort sort, bool descending, bool streaming);
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace pulse::index
