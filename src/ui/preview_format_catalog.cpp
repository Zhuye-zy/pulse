// Keep MF header feature gates consistent with the Windows 8.1 app target
// (same as video_preview.cpp).
#undef NTDDI_VERSION
#define NTDDI_VERSION 0x06030000
#include "preview_format_catalog.h"
#include "video_preview.h"
#include "../common/localization.h"
#include "../common/preview_extensions.h"
#include <mfapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <initializer_list>
#include <mutex>
#include <thread>

namespace pulse::ui {
namespace {

const wchar_t* Pick(const wchar_t* zh, const wchar_t* en) { return l10n::Pick(zh, en); }

template <size_t N>
void AppendTable(std::vector<std::wstring>& out, const std::wstring_view (&table)[N]) {
    for (const auto& extension : table) out.emplace_back(extension.substr(1));
}

bool Contains(const std::vector<std::wstring>& list, const std::wstring& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

std::vector<PreviewFormatGroup> BuildGroups() {
    namespace f = preview::formats;
    std::vector<PreviewFormatGroup> groups;
    std::vector<std::wstring> used;
    auto add = [&](const wchar_t* zh, const wchar_t* en, const wchar_t* note_zh,
                   const wchar_t* note_en, std::vector<std::wstring> extensions) {
        PreviewFormatGroup group{Pick(zh, en), Pick(note_zh, note_en), {}};
        for (auto& extension : extensions) {
            if (Contains(used, extension)) continue;
            used.push_back(extension);
            group.extensions.push_back(std::move(extension));
        }
        if (!group.extensions.empty()) groups.push_back(std::move(group));
    };

    std::vector<std::wstring> images;
    AppendTable(images, f::kImage);
    AppendTable(images, f::kVector);
    AppendTable(images, f::kPsd);
    AppendTable(images, f::kMetaFile);
    add(L"图片", L"Images",
        L"GIF / WebP / APNG 动图可播放；图标可切换尺寸；HEIC / AVIF 需要下方的系统扩展",
        L"Animated GIF / WebP / APNG play; icons switch sizes; HEIC / AVIF need the system extensions below",
        std::move(images));

    std::vector<std::wstring> documents;
    AppendTable(documents, f::kPdfRaster);
    for (const wchar_t* extension : {L"md", L"docx", L"epub", L"ipynb"}) documents.emplace_back(extension);
    add(L"文档", L"Documents",
        L"DOCX 无需安装 Office；EPUB 带目录、按章阅读；Markdown 和 Notebook 按排版显示",
        L"DOCX without Office; EPUB with contents, one chapter at a time; Markdown and notebooks rendered",
        std::move(documents));

    std::vector<std::wstring> data;
    for (const wchar_t* extension : {L"csv", L"tsv", L"xlsx", L"json", L"xml", L"yaml", L"yml",
                                     L"toml", L"ini", L"cfg", L"conf", L"properties"})
        data.emplace_back(extension);
    add(L"表格与数据", L"Tables & data",
        L"CSV / TSV / XLSX 以表格显示；JSON / XML 为可折叠的树",
        L"CSV / TSV / XLSX as a table; JSON / XML as a collapsible tree", std::move(data));

    std::vector<std::wstring> text;
    AppendTable(text, f::kText);
    add(L"代码与文本", L"Code & text", L"语法高亮", L"Syntax highlighting", std::move(text));

    // Quick Look plays what VideoPreview accepts (Media Foundation).
    std::vector<std::wstring> media;
    for (const wchar_t* extension : {L"mp4", L"mkv", L"mov", L"webm", L"avi", L"wmv", L"m4v",
                                     L"mpg", L"mpeg", L"m2ts", L"mts", L"3gp", L"mp3", L"flac",
                                     L"wav", L"m4a", L"aac", L"wma", L"ogg", L"oga", L"opus",
                                     L"aif", L"aiff"})
        if (VideoPreview::Supports(std::wstring(L"x.") + extension)) media.emplace_back(extension);
    add(L"音视频", L"Audio & video",
        L"由系统解码器播放；HEVC / AV1 视频需要下方的系统扩展",
        L"Played by the system decoders; HEVC / AV1 video need the system extensions below",
        std::move(media));

    std::vector<std::wstring> archives;
    AppendTable(archives, f::kArchive);
    add(L"压缩包", L"Archives", L"浏览内容，无需解压", L"Browse the contents without extracting",
        std::move(archives));

    std::vector<std::wstring> fonts;
    AppendTable(fonts, f::kFont);
    add(L"字体", L"Fonts", L"显示样张，无需安装",
        L"Specimen page without installing", std::move(fonts));
    return groups;
}

struct GroupCache {
    std::mutex mutex;
    l10n::Language language = l10n::Language::System;
    bool built = false;
    std::vector<PreviewFormatGroup> groups;
};
GroupCache& Cache() { static GroupCache cache; return cache; }

bool HasPackage(std::initializer_list<const wchar_t*> prefixes) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT,
            L"Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages",
            0, KEY_ENUMERATE_SUB_KEYS, &key) != ERROR_SUCCESS)
        return false;
    bool found = false;
    wchar_t name[256];
    for (DWORD i = 0; !found; ++i) {
        DWORD length = ARRAYSIZE(name);
        if (RegEnumKeyExW(key, i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        for (const wchar_t* prefix : prefixes)
            if (_wcsnicmp(name, prefix, wcslen(prefix)) == 0) { found = true; break; }
    }
    RegCloseKey(key);
    return found;
}

// Media Foundation subtype GUIDs built from their FOURCC so no newer SDK
// gate or mfuuid.lib is needed.
constexpr GUID VideoSubtype(DWORD fourcc) {
    return GUID{fourcc, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
}
constexpr DWORD FourCc(char a, char b, char c, char d) {
    return static_cast<DWORD>(static_cast<unsigned char>(a)) |
           (static_cast<DWORD>(static_cast<unsigned char>(b)) << 8) |
           (static_cast<DWORD>(static_cast<unsigned char>(c)) << 16) |
           (static_cast<DWORD>(static_cast<unsigned char>(d)) << 24);
}

// Bit 0: HEVC decoder, bit 1: AV1 decoder (any software, hardware or Store MFT).
unsigned VideoDecoders() {
    HMODULE plat = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!plat) return 0;
    unsigned mask = 0;
    using Startup = HRESULT (WINAPI*)(ULONG, DWORD);
    using Shutdown = HRESULT (WINAPI*)();
    using EnumEx = HRESULT (WINAPI*)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO*,
                                     const MFT_REGISTER_TYPE_INFO*, IMFActivate***, UINT32*);
    const auto startup = reinterpret_cast<Startup>(GetProcAddress(plat, "MFStartup"));
    const auto shutdown = reinterpret_cast<Shutdown>(GetProcAddress(plat, "MFShutdown"));
    const auto enumerate = reinterpret_cast<EnumEx>(GetProcAddress(plat, "MFTEnumEx"));
    if (startup && shutdown && enumerate && SUCCEEDED(startup(MF_VERSION, MFSTARTUP_LITE))) {
        constexpr GUID kVideoDecoderCategory{0xd6c02d4b, 0x6833, 0x45b4, {0x97, 0x1a, 0x05, 0xa4, 0xb0, 0x4b, 0xab, 0x91}};
        const GUID video_major = VideoSubtype(FourCc('v', 'i', 'd', 's'));
        const GUID subtypes[] = {VideoSubtype(FourCc('H', 'E', 'V', 'C')), VideoSubtype(FourCc('A', 'V', '0', '1'))};
        for (unsigned i = 0; i < 2; ++i) {
            const MFT_REGISTER_TYPE_INFO input{video_major, subtypes[i]};
            IMFActivate** found = nullptr;
            UINT32 count = 0;
            if (SUCCEEDED(enumerate(kVideoDecoderCategory, MFT_ENUM_FLAG_ALL, &input, nullptr, &found, &count))) {
                if (count > 0) mask |= 1u << i;
                for (UINT32 k = 0; k < count; ++k)
                    if (found[k]) found[k]->Release();
                CoTaskMemFree(found);
            }
        }
        shutdown();
    }
    FreeLibrary(plat);
    return mask;
}

std::atomic<unsigned> g_codecs{0};
std::atomic<bool> g_probe_running{false};
std::atomic<bool> g_probe_again{false};

unsigned ProbeCodecs() {
    const unsigned decoders = VideoDecoders();
    unsigned mask = kPreviewCodecsDetected;
    if (HasPackage({L"Microsoft.HEIFImageExtension_"})) mask |= 1u << 0;
    if ((decoders & 1u) || HasPackage({L"Microsoft.HEVCVideoExtension_", L"Microsoft.HEVCVideoExtensions_"}))
        mask |= 1u << 1;
    if ((decoders & 2u) || HasPackage({L"Microsoft.AV1VideoExtension_"})) mask |= 1u << 2;
    if (HasPackage({L"Microsoft.WebpImageExtension_"})) mask |= 1u << 3;
    return mask;
}

// MFTEnumEx can pump COM messages. Run on the UI thread from BuildVm, it let a
// title-bar WM_NCHITTEST re-enter BuildVm and start another probe before the
// first one finished, until the stack overflowed. One worker, never re-entered.
void RunCodecProbe(HWND notify) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        g_codecs.store(ProbeCodecs());
        if (notify) InvalidateRect(notify, nullptr, FALSE);
        if (g_probe_again.exchange(false)) continue;
        g_probe_running.store(false);
        // A refresh may have arrived between the check above and the store.
        if (!g_probe_again.exchange(false) || g_probe_running.exchange(true)) break;
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

float ChipWidth(const std::wstring& text, float scale) {
    return (6.6f * static_cast<float>(text.size()) + 16.0f) * scale;
}

// Rough width of a note line in the small UI font (CJK is about twice as wide).
float EstimateWidth(const std::wstring& text, float scale) {
    float width = 0;
    for (const wchar_t c : text) width += c >= 0x2E80 ? 12.5f : 6.6f;
    return width * scale;
}

} // namespace

const std::vector<PreviewFormatGroup>& PreviewFormatGroups() {
    auto& cache = Cache();
    std::lock_guard lock(cache.mutex);
    const l10n::Language language = l10n::effective_language();
    if (!cache.built || cache.language != language) {
        cache.groups = BuildGroups();
        cache.language = language;
        cache.built = true;
    }
    return cache.groups;
}

size_t PreviewFormatCount() {
    size_t count = 0;
    for (const auto& group : PreviewFormatGroups()) count += group.extensions.size();
    return count;
}

PreviewCodecInfo PreviewCodec(int index) {
    switch (index) {
    case 0: return {Pick(L"HEIF 图像扩展", L"HEIF Image Extensions"),
                    Pick(L"HEIC / HEIF 照片（iPhone 常用）", L"HEIC / HEIF photos (common on iPhone)"),
                    L"9PMMSR1CGPWG"};
    case 1: return {Pick(L"HEVC 视频扩展", L"HEVC Video Extensions"),
                    Pick(L"HEVC (H.265) 视频，以及 HEIC 照片的解码", L"HEVC (H.265) video, and decoding HEIC photos"),
                    L"9NMZLZ57R3T7"};
    case 2: return {Pick(L"AV1 视频扩展", L"AV1 Video Extension"),
                    Pick(L"AVIF 图片和 AV1 视频", L"AVIF images and AV1 video"), L"9MVZQVXJBQ9V"};
    default: return {Pick(L"WebP 图像扩展", L"WebP Image Extensions"),
                     Pick(L"WebP 图片和动图", L"WebP images and animations"), nullptr};
    }
}

unsigned DetectPreviewCodecs(bool refresh, HWND notify) {
    const unsigned mask = g_codecs.load();
    if ((mask & kPreviewCodecsDetected) && !refresh) return mask;
    if (g_probe_running.exchange(true)) {
        if (refresh) g_probe_again.store(true);
        return mask;
    }
    try {
        std::thread(RunCodecProbe, notify).detach();
    } catch (...) {
        g_probe_running.store(false); // no thread: report "not detected" and retry next paint
    }
    return mask;
}

bool OpenPreviewCodecStore(HWND owner, int index) {
    const auto info = PreviewCodec(index);
    if (!info.store_id) return false;
    const std::wstring url = std::wstring(L"ms-windows-store://pdp/?ProductId=") + info.store_id;
    return reinterpret_cast<INT_PTR>(ShellExecuteW(owner, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

float LayoutPreviewFormats(const D2D1_RECT_F& area, float scale,
                           std::vector<PreviewFormatChip>* chips,
                           std::vector<PreviewFormatRow>* rows) {
    const auto& groups = PreviewFormatGroups();
    const float left = area.left + 16 * scale, right = area.right - 16 * scale;
    const bool stacked = right - left < 380 * scale;
    const float name_width = 108 * scale;
    const float chips_left = stacked ? left : left + name_width;
    const float chip_height = 22 * scale, gap = 5 * scale;
    float y = area.top + 4 * scale;
    for (size_t g = 0; g < groups.size(); ++g) {
        PreviewFormatRow row{};
        row.bounds.left = area.left; row.bounds.right = area.right; row.bounds.top = y;
        y += 10 * scale;
        row.name = D2D1::RectF(left, y, stacked ? right : chips_left - 8 * scale, y + chip_height);
        if (stacked) y += chip_height + 4 * scale;
        float x = chips_left;
        for (size_t i = 0; i < groups[g].extensions.size(); ++i) {
            const float w = ChipWidth(groups[g].extensions[i], scale);
            if (x > chips_left && x + w > right) { x = chips_left; y += chip_height + 4 * scale; }
            if (chips) chips->push_back({D2D1::RectF(x, y, x + w, y + chip_height), g, i});
            x += w + gap;
        }
        y += chip_height;
        if (!groups[g].note.empty()) {
            const float available = (std::max)(right - chips_left, 1.0f);
            const float lines = (std::max)(1.0f, std::ceil(EstimateWidth(groups[g].note, scale) / available));
            row.note = D2D1::RectF(chips_left, y + 4 * scale, right, y + (4 + 18 * lines) * scale);
            y += (4 + 18 * lines) * scale;
        }
        y += 10 * scale;
        row.bounds.bottom = y;
        if (rows) rows->push_back(row);
    }
    return y + 4 * scale - area.top;
}

} // namespace pulse::ui
