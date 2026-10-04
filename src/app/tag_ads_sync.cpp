#include "tag_ads_sync.h"
#include "session.h"
#include "../common/utf8_file.h"
#include "../fs/fs_enum.h"
#include <map>
#include <mutex>
#include <cwctype>
namespace pulse::app {
namespace {
std::mutex mutex;
std::wstring loaded_dir;
std::map<std::wstring, TagAdsUpdate> pending;
bool readable = false;
std::wstring Key(const std::wstring& path) {
    auto key = fs::NormalizePath(path);
    for (auto& ch : key) ch = static_cast<wchar_t>(towlower(ch));
    return key;
}
bool Number(std::wstring_view text, size_t& position, size_t& value) {
    value = 0; const size_t begin = position;
    while (position < text.size() && text[position] >= L'0' && text[position] <= L'9') {
        if (value > 16000000) return false;
        value = value * 10 + static_cast<size_t>(text[position++] - L'0');
    }
    return position > begin && position < text.size() && text[position++] == L':';
}
bool Field(std::wstring_view text, size_t& position, std::wstring& value) {
    size_t size = 0;
    if (!Number(text, position, size) || size > text.size() - position) return false;
    value.assign(text.substr(position, size)); position += size; return true;
}
void Load() {
    const auto dir = GetPulseDataDir();
    if (loaded_dir == dir && !loaded_dir.empty()) return;
    loaded_dir = dir; pending.clear(); readable = false;
    if (dir.empty()) return;
    const auto file = dir + L"\\tag_ads_pending.dat";
    if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) { readable = true; return; }
    }
    std::wstring text;
    constexpr std::wstring_view prefix = L"PULSE_ADS_PENDING_1\n";
    if (!ReadUtf8File(file,text) || !text.starts_with(prefix)) return;
    size_t pos = prefix.size();
    while (pos < text.size()) {
        TagAdsUpdate update; size_t count = 0;
        if (!Field(text,pos,update.path) || update.path.empty() || !Number(text,pos,count) || count > 100000) return;
        for (size_t i=0; i<count; ++i) {
            TagAdsRecord tag; size_t rgb = 0;
            if (!Field(text,pos,tag.id) || !Field(text,pos,tag.name) || !Number(text,pos,rgb) || rgb > 0xFFFFFF) return;
            tag.rgb = static_cast<uint32_t>(rgb); update.tags.push_back(std::move(tag));
        }
        pending[Key(update.path)] = std::move(update);
    }
    readable = true;
}
bool Save(const std::map<std::wstring, TagAdsUpdate>& records) {
    std::wstring text = L"PULSE_ADS_PENDING_1\n";
    auto field = [&](const std::wstring& value) { text += std::to_wstring(value.size()) + L":" + value; };
    for (const auto& [key,update] : records) {
        field(update.path); text += std::to_wstring(update.tags.size()) + L":";
        for (const auto& tag : update.tags) {
            field(tag.id); field(tag.name); text += std::to_wstring(tag.rgb & 0xFFFFFFu) + L":";
        }
    }
    return WriteUtf8FileAtomic(loaded_dir + L"\\tag_ads_pending.dat",text);
}
}
bool HasPendingTagAds(const std::wstring& path) {
    std::lock_guard lock(mutex); Load();
    return !readable || pending.contains(Key(path));
}
std::vector<std::wstring> SyncTagAdsUpdates(const std::vector<TagAdsUpdate>& updates) {
    std::unique_lock lock(mutex); Load();
    std::vector<std::wstring> failed;
    if (!readable) {
        for (const auto& update : updates) failed.push_back(update.path);
        if (failed.empty()) failed.push_back(loaded_dir);
        return failed;
    }
    auto staged = pending;
    for (const auto& update : updates) staged[Key(update.path)] = update;
    // Publish intent before releasing the short metadata lock.
    pending = staged;
    lock.unlock();
    if (staged.empty()) return failed;
    // Also persist retries: an earlier journal write may have failed while the
    // intended values were retained only in memory.
    if (!Save(staged)) {
        for (const auto& [key,update] : staged) failed.push_back(update.path);
        return failed;
    }
    size_t attempted = 0;
    for (auto it = staged.begin(); it != staged.end() && attempted < 64; ++attempted) {
        if (WriteTagAdsV2(it->second.path,it->second.tags)) it = staged.erase(it);
        else ++it;
    }
    const bool saved = Save(staged);
    lock.lock();
    if (saved) pending = std::move(staged);
    for (const auto& [key,update] : pending) failed.push_back(update.path);
    return failed;
}
}
