// app_prefs.cpp — Persist general settings; sync 开机自启 with the Run key.
#include "app_prefs.h"
#include "win_e_agent.h"
#include "session.h"
#include "../ui/panel_metrics.h"
#include "../common/json_utils.h"
#include "../common/utf8_file.h"
#include <windows.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <algorithm>
#include <cwctype>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

namespace pulse::app {
namespace {

constexpr const wchar_t* kRunKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kRunValue = L"Pulse";

std::wstring ExePath() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return n ? std::wstring(path, n) : L"";
}

} // namespace

namespace {

void DeleteStoredWallpapers(const std::wstring& dir) {
    static constexpr const wchar_t* kExt[] = {
        L".jpg", L".jpeg", L".png", L".bmp", L".webp", L".jfif", L".img"
    };
    for (const wchar_t* ext : kExt)
        DeleteFileW((dir + L"\\wallpaper" + ext).c_str());
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

} // namespace

void AppPrefs::ResetToDefaults() {
    launch_on_startup = false;
    keep_running_on_close = false;
    open_folders_in_pulse = false;
    shell_tag_menu = false;
    verify_copies = false;
    show_status_performance = false;
    show_pinned_tab_names = true;
    list_smart_date = true;
    list_zebra_rows = true;
    list_size_bar = false;
    folder_sort_mode = 0;
    details_columns = ui::kDetailsColumnsDefault;
    startup_open = 0;
    new_tab_open = 0;
    home_folder.clear();
    text_render = 0;
    folder_views.Clear();
    folder_sorts.Clear();
    folder_groups.Clear();
    search_pinyin = true;
    global_search_enabled = false;
    global_search_modifiers = 1;
    global_search_key = 32;
    show_hidden_files = false;
    show_protected_os_files = false;
    blank_click_go_back = false;
    change_tracking_enabled = false;
    change_tracking_days = 7;
    theme_mode = -1;
    language = L"system";
    window_effect = L"mica-alt";
    background_image.clear();
    wallpaper_look = 50;
    wallpaper_blur = 14;
    row_height = 34;
    sidebar_width = 224;
    address_search_current = false;
    address_search_content = false;
    tray_icon_size = 48;
    show_hints = true;
    tips_seen = 0;
    accent_rgb.clear();
    accent_follow_system = false;
    custom_tag_colors.clear();
    duplicate_scan_scope = 0;
    duplicate_scan_folder.clear();
    duplicate_scan_drive.clear();
}

std::wstring AppPrefs::ToJson() const {
    std::wstring escaped_effect;
    std::wstring escaped_image;
    std::wstring escaped_language;
    std::wstring escaped_home;
    pulse::json::Escape(window_effect, escaped_effect);
    pulse::json::Escape(home_folder, escaped_home);
    pulse::json::Escape(background_image, escaped_image);
    pulse::json::Escape(language, escaped_language);
    std::wstring out = L"{\n  \"version\":4,\n  \"launch_on_startup\":";
    out += launch_on_startup ? L"true" : L"false";
    out += L",\n  \"keep_running_on_close\":";
    out += keep_running_on_close ? L"true" : L"false";
    out += L",\n  \"open_folders_in_pulse\":";
    out += open_folders_in_pulse ? L"true" : L"false";
    out += L",\n  \"shell_tag_menu\":";
    out += shell_tag_menu ? L"true" : L"false";
    out += L",\n  \"verify_copies\":";
    out += verify_copies ? L"true" : L"false";
    out += L",\n  \"show_status_performance\":";
    out += show_status_performance ? L"true" : L"false";
    out += L",\n  \"show_pinned_tab_names\":";
    out += show_pinned_tab_names ? L"true" : L"false";
    out += L",\n  \"list_smart_date\":";
    out += list_smart_date ? L"true" : L"false";
    out += L",\n  \"list_zebra_rows\":";
    out += list_zebra_rows ? L"true" : L"false";
    out += L",\n  \"list_size_bar\":";
    out += list_size_bar ? L"true" : L"false";
    out += L",\n  \"list_tag_name_color\":";
    out += list_tag_name_color ? L"true" : L"false";
    out += L",\n  \"vertical_tabs\":";
    out += vertical_tabs ? L"true" : L"false";
    out += L",\n  \"sidebar_collapsed\":";
    out += sidebar_collapsed ? L"true" : L"false";
    out += L",\n  \"folder_sort_mode\":";
    out += std::to_wstring(folder_sort_mode >= 0 && folder_sort_mode <= 2 ? folder_sort_mode : 0);
    out += L",\n  \"details_columns\":";
    out += std::to_wstring(ui::NormalizeDetailsColumns(details_columns));
    out += L",\n  \"startup_open\":";
    out += startup_open == 1 ? L"1" : L"0";
    out += L",\n  \"new_tab_open\":";
    out += new_tab_open == 1 ? L"1" : L"0";
    out += L",\n  \"home_folder\":\"";
    out += escaped_home;
    out += L"\"";
    out += L",\n  \"text_render\":";
    out += std::to_wstring(text_render >= 0 && text_render <= 2 ? text_render : 0);
    out += L",\n  \"show_hidden_files\":";
    out += show_hidden_files ? L"true" : L"false";
    out += L",\n  \"show_protected_os_files\":";
    out += show_protected_os_files ? L"true" : L"false";
    out += L",\n  \"search_pinyin\":";
    out += search_pinyin ? L"true" : L"false";
    out += L",\n  \"global_search_enabled\":";
    out += global_search_enabled ? L"true" : L"false";
    out += L",\n  \"global_search_modifiers\":" + std::to_wstring(global_search_modifiers);
    out += L",\n  \"global_search_key\":" + std::to_wstring(global_search_key);
    out += L",\n  \"blank_click_go_back\":";
    out += blank_click_go_back ? L"true" : L"false";
    out += L",\n  \"change_tracking_enabled\":";
    out += change_tracking_enabled ? L"true" : L"false";
    out += L",\n  \"change_tracking_days\":";
    out += std::to_wstring(change_tracking_days == 1 || change_tracking_days == 3 ? change_tracking_days : 7);
    out += L",\n  \"theme_mode\":";
    out += std::to_wstring(theme_mode);
    out += L",\n  \"language\":\"";
    out += escaped_language;
    out += L"\"";
    out += L",\n  \"window_effect\":\"";
    out += escaped_effect;
    out += L"\",\n  \"background_image\":\"";
    out += escaped_image;
    out += L"\",\n  \"row_height\":";
    out += std::to_wstring(row_height);
    out += L",\n  \"sidebar_width\":" + std::to_wstring(sidebar_width);
    out += L",\n  \"address_search_current\":" + std::to_wstring(address_search_current);
    out += L",\n  \"address_search_content\":" + std::to_wstring(address_search_content);
    out += L",\n  \"tray_icon_size\":";
    out += std::to_wstring(tray_icon_size);
    out += L",\n  \"show_hints\":";
    out += show_hints ? L"true" : L"false";
    out += L",\n  \"tips_seen\":" + std::to_wstring(tips_seen);
    // Legacy three-level keys stay for older builds reading the same app.json.
    out += L",\n  \"wallpaper_look\":";
    out += std::to_wstring(wallpaper_look < 38 ? 0 : (wallpaper_look < 63 ? 1 : 2));
    out += L",\n  \"wallpaper_blur\":";
    out += std::to_wstring(wallpaper_blur <= 0 ? 0 : (wallpaper_blur <= 21 ? 1 : 2));
    out += L",\n  \"panel_transparency\":" + std::to_wstring(wallpaper_look);
    out += L",\n  \"wallpaper_blur_px\":" + std::to_wstring(wallpaper_blur);
    out += L",\n  \"accent_rgb\":\"";
    {
        std::wstring escaped_accent;
        pulse::json::Escape(accent_rgb, escaped_accent);
        out += escaped_accent;
    }
    out += L"\",\n  \"accent_follow_system\":" + std::to_wstring(accent_follow_system);
    out += L",\n  \"custom_tag_colors\":[";
    for (size_t i = 0; i < custom_tag_colors.size(); ++i) {
        wchar_t hex[8]{};
        swprintf_s(hex, L"%06X", custom_tag_colors[i] & 0x00FFFFFFu);
        if (i > 0) out += L",";
        out += L"\"";
        out += hex;
        out += L"\"";
    }
    out += L"],\n  \"duplicate_scan_scope\":";
    out += std::to_wstring(duplicate_scan_scope);
    out += L",\n  \"duplicate_scan_folder\":\"";
    {
        std::wstring escaped;
        pulse::json::Escape(duplicate_scan_folder, escaped);
        out += escaped;
    }
    out += L"\",\n  \"duplicate_scan_drive\":\"";
    {
        std::wstring escaped;
        pulse::json::Escape(duplicate_scan_drive, escaped);
        out += escaped;
    }
    out += L"\",\n  \"last_seen_version\":\"";
    {
        std::wstring escaped;
        pulse::json::Escape(last_seen_version, escaped);
        out += escaped;
    }
    out += L"\",\n  \"tray_dests\":\"";
    {
        std::wstring escaped;
        pulse::json::Escape(tray_dests, escaped);
        out += escaped;
    }
    out += L"\"";
    folder_views.AppendJson(out);
    folder_sorts.AppendJson(out);
    folder_groups.AppendJson(out);
    out += L"\n}\n";
    return out;
}

bool AppPrefs::FromJson(const std::wstring& json) {
    if (json.empty()) return false;
    folder_views.ReadJson(json);
    folder_sorts.ReadJson(json);
    folder_groups.ReadJson(json);
    launch_on_startup = pulse::json::ExtractBool(json, L"launch_on_startup", false);
    keep_running_on_close = pulse::json::ExtractBool(json, L"keep_running_on_close", false);
    open_folders_in_pulse = pulse::json::ExtractBool(json, L"open_folders_in_pulse", false);
    shell_tag_menu = pulse::json::ExtractBool(json, L"shell_tag_menu", false);
    verify_copies = pulse::json::ExtractBool(json, L"verify_copies", false);
    show_status_performance = pulse::json::ExtractBool(json, L"show_status_performance", false);
    show_pinned_tab_names = pulse::json::ExtractBool(json, L"show_pinned_tab_names", true);
    list_smart_date = pulse::json::ExtractBool(json, L"list_smart_date", true);
    list_zebra_rows = pulse::json::ExtractBool(json, L"list_zebra_rows", true);
    list_size_bar = pulse::json::ExtractBool(json, L"list_size_bar", false);
    list_tag_name_color = pulse::json::ExtractBool(json, L"list_tag_name_color", false);
    vertical_tabs = pulse::json::ExtractBool(json, L"vertical_tabs", false);
    sidebar_collapsed = pulse::json::ExtractBool(json, L"sidebar_collapsed", false);
    folder_sort_mode = pulse::json::ExtractInt(json, L"folder_sort_mode", 0);
    if (folder_sort_mode < 0 || folder_sort_mode > 2) folder_sort_mode = 0;
    details_columns = ui::NormalizeDetailsColumns(static_cast<uint32_t>(pulse::json::ExtractInt(
        json, L"details_columns", static_cast<int>(ui::kDetailsColumnsDefault))));
    startup_open = pulse::json::ExtractInt(json, L"startup_open", 0) == 1 ? 1 : 0;
    new_tab_open = pulse::json::ExtractInt(json, L"new_tab_open", 0) == 1 ? 1 : 0;
    home_folder = pulse::json::ExtractString(json, L"home_folder");
    text_render = pulse::json::ExtractInt(json, L"text_render", 0);
    if (text_render < 0 || text_render > 2) text_render = 0;
    search_pinyin = pulse::json::ExtractBool(json, L"search_pinyin", true);
    global_search_enabled = pulse::json::ExtractBool(json, L"global_search_enabled", false);
    const int modifiers = pulse::json::ExtractInt(json, L"global_search_modifiers", 1);
    const int key = pulse::json::ExtractInt(json, L"global_search_key", 32);
    global_search_modifiers = modifiers > 0 && modifiers <= 15 ? static_cast<uint32_t>(modifiers) : 1;
    global_search_key = key > 0 && key <= 254 ? static_cast<uint32_t>(key) : 32;
    show_hidden_files = pulse::json::ExtractBool(json, L"show_hidden_files", false);
    show_protected_os_files = pulse::json::ExtractBool(json, L"show_protected_os_files", false);
    blank_click_go_back = pulse::json::ExtractBool(json, L"blank_click_go_back", false);
    change_tracking_enabled = pulse::json::ExtractBool(json, L"change_tracking_enabled", false);
    change_tracking_days = pulse::json::ExtractInt(json, L"change_tracking_days", 7);
    if (change_tracking_days != 1 && change_tracking_days != 3 && change_tracking_days != 7)
        change_tracking_days = 7;
    theme_mode = pulse::json::ExtractInt(json, L"theme_mode", -1);
    if (theme_mode < -1 || theme_mode > 2) theme_mode = -1;
    language = pulse::json::ExtractString(json, L"language", L"system");
    if (language != L"system" && language != L"zh-CN" && language != L"en-US")
        language = L"system";
    window_effect = pulse::json::ExtractString(json, L"window_effect", L"mica-alt");
    if (window_effect == L"dwm-blur") window_effect = L"acrylic-material";
    else if (window_effect.empty()) window_effect = L"mica-alt";
    background_image = pulse::json::ExtractString(json, L"background_image");
    row_height = pulse::json::ExtractInt(json, L"row_height", 34);
    sidebar_width = pulse::json::ExtractInt(json, L"sidebar_width", 224);
    // The stored value is the user's intent; the window caps it while drawing.
    if (sidebar_width < static_cast<int>(ui::kSidebarMinWidthDip) ||
        sidebar_width > static_cast<int>(ui::kPanelWidthMaxDip)) sidebar_width = 224;
    address_search_current = pulse::json::ExtractInt(json, L"address_search_current", 0) != 0;
    address_search_content = pulse::json::ExtractInt(json, L"address_search_content", 0) != 0;
    if (row_height < 24 || row_height > 48) row_height = 34;
    tray_icon_size = pulse::json::ExtractInt(json, L"tray_icon_size", 48);
    show_hints = pulse::json::ExtractBool(json, L"show_hints", true);
    const int seen = pulse::json::ExtractInt(json, L"tips_seen", 0);
    tips_seen = seen > 0 ? static_cast<uint32_t>(seen) : 0u;
    if (tray_icon_size < 32 || tray_icon_size > 64) tray_icon_size = 48;
    {
        // Continuous values; migrate the former 0/1/2 levels when absent.
        static constexpr int kLookLevels[] = {25, 50, 75};
        static constexpr int kBlurLevels[] = {0, 14, 28};
        int legacy = pulse::json::ExtractInt(json, L"wallpaper_look", 1);
        if (legacy < 0 || legacy > 2) legacy = 1;
        wallpaper_look = pulse::json::ExtractInt(json, L"panel_transparency", kLookLevels[legacy]);
        if (wallpaper_look < 0 || wallpaper_look > 100) wallpaper_look = kLookLevels[legacy];
        legacy = pulse::json::ExtractInt(json, L"wallpaper_blur", 1);
        if (legacy < 0 || legacy > 2) legacy = 1;
        wallpaper_blur = pulse::json::ExtractInt(json, L"wallpaper_blur_px", kBlurLevels[legacy]);
        if (wallpaper_blur < 0 || wallpaper_blur > 40) wallpaper_blur = kBlurLevels[legacy];
    }
    accent_rgb = pulse::json::ExtractString(json, L"accent_rgb");
    accent_follow_system = pulse::json::ExtractInt(json, L"accent_follow_system", 0) != 0;
    uint32_t accent_parsed = 0;
    if (!accent_rgb.empty() && ParseAccentRgb(accent_rgb, accent_parsed)) {
        wchar_t hex[8]{};
        swprintf_s(hex, L"%06X", accent_parsed);
        accent_rgb = hex;
    } else {
        accent_rgb.clear();
    }
    custom_tag_colors.clear();
    for (const std::wstring& entry :
         pulse::json::ExtractStringArray(json, L"custom_tag_colors")) {
        const wchar_t* text = entry.c_str();
        if (*text == L'#') ++text;
        wchar_t* end = nullptr;
        const unsigned long v = wcstoul(text, &end, 16);
        if (end && *end == L'\0' && v <= 0xFFFFFFul && wcslen(text) == 6)
            custom_tag_colors.push_back(static_cast<uint32_t>(v));
    }
    duplicate_scan_scope = pulse::json::ExtractInt(json, L"duplicate_scan_scope", 0);
    if (duplicate_scan_scope < 0 || duplicate_scan_scope > 2) duplicate_scan_scope = 0;
    duplicate_scan_folder = pulse::json::ExtractString(json, L"duplicate_scan_folder");
    duplicate_scan_drive = pulse::json::ExtractString(json, L"duplicate_scan_drive");
    last_seen_version = pulse::json::ExtractString(json, L"last_seen_version");
    tray_dests = pulse::json::ExtractString(json, L"tray_dests");
    return true;
}

bool AppPrefs::StoreBackgroundImage(const std::wstring& source_path) {
    if (source_path.empty()) return false;
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty()) {
        background_image = source_path;
        return true;
    }
    const wchar_t* ext = PathFindExtensionW(source_path.c_str());
    std::wstring dest = dir + L"\\wallpaper";
    dest += (ext && ext[0]) ? ext : L".img";
    if (!SamePath(source_path, dest)) {
        DeleteStoredWallpapers(dir);
        if (!CopyFileW(source_path.c_str(), dest.c_str(), FALSE)) {
            background_image = source_path;
            return true;
        }
    }
    background_image = dest;
    return true;
}

void AppPrefs::ClearBackgroundImage() {
    const std::wstring dir = GetPulseDataDir();
    if (!dir.empty() && !background_image.empty()) {
        const std::wstring prefix = dir + L"\\wallpaper";
        if (background_image.size() >= prefix.size() &&
            CompareStringOrdinal(background_image.c_str(), static_cast<int>(prefix.size()),
                                 prefix.c_str(), static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL) {
            DeleteFileW(background_image.c_str());
        }
    }
    background_image.clear();
}

bool AppPrefs::ReadLaunchOnStartup() const {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    wchar_t value[MAX_PATH] = {};
    DWORD bytes = sizeof(value);
    DWORD type = 0;
    const LONG st = RegQueryValueExW(key, kRunValue, nullptr, &type,
                                     reinterpret_cast<LPBYTE>(value), &bytes);
    RegCloseKey(key);
    if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return false;
    return value[0] != 0;
}

bool AppPrefs::ApplyLaunchOnStartup(bool on) {
    launch_on_startup = on;
    if (!persist) return true;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return false;
    LONG st = ERROR_SUCCESS;
    if (on) {
        const std::wstring exe = ExePath();
        if (exe.empty()) { RegCloseKey(key); return false; }
        const std::wstring cmd = L"\"" + exe + L"\"";
        st = RegSetValueExW(key, kRunValue, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(cmd.c_str()),
                            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        st = RegDeleteValueW(key, kRunValue);
        if (st == ERROR_FILE_NOT_FOUND) st = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return st == ERROR_SUCCESS;
}

std::wstring FolderOpenCommandLine(const std::wstring& exe) {
    if (exe.empty()) return {};
    return L"\"" + exe + L"\" \"%1\"";
}

bool FolderOpenCommandIsOurs(const std::wstring& command, const std::wstring& exe) {
    if (command.empty() || exe.empty()) return false;
    size_t i = 0;
    while (i < command.size() && iswspace(command[i])) ++i;
    std::wstring token;
    if (i < command.size() && command[i] == L'"') {
        ++i;
        const size_t start = i;
        while (i < command.size() && command[i] != L'"') ++i;
        token = command.substr(start, i - start);
    } else {
        const size_t start = i;
        while (i < command.size() && !iswspace(command[i])) ++i;
        token = command.substr(start, i - start);
    }
    return !token.empty() &&
           CompareStringOrdinal(token.c_str(), -1, exe.c_str(), -1, TRUE) == CSTR_EQUAL;
}

namespace {

constexpr const wchar_t* kFolderOpenClasses[] = { L"Directory", L"Drive" };

std::wstring FolderOpenKey(const wchar_t* cls) {
    return std::wstring(L"Software\\Classes\\") + cls + L"\\shell\\open";
}

std::wstring FolderShellKey(const wchar_t* cls) {
    return std::wstring(L"Software\\Classes\\") + cls + L"\\shell";
}

std::wstring ReadRegDefault(const std::wstring& key) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS)
        return {};
    wchar_t value[1024] = {};
    DWORD bytes = sizeof(value);
    DWORD type = 0;
    const LONG st = RegQueryValueExW(h, nullptr, nullptr, &type,
                                     reinterpret_cast<LPBYTE>(value), &bytes);
    RegCloseKey(h);
    if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return {};
    return value;
}

bool WriteFolderOpenClass(const wchar_t* cls, const std::wstring& exe) {
    const std::wstring open = FolderOpenKey(cls);
    const std::wstring command = open + L"\\command";
    HKEY h = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, command.c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS)
        return false;
    const std::wstring line = FolderOpenCommandLine(exe);
    const LONG st = RegSetValueExW(h, nullptr, 0, REG_SZ,
                                   reinterpret_cast<const BYTE*>(line.c_str()),
                                   static_cast<DWORD>((line.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    if (st != ERROR_SUCCESS) return false;
    h = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, open.c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS)
        return false;
    const wchar_t empty[] = L"";
    const LONG de = RegSetValueExW(h, L"DelegateExecute", 0, REG_SZ,
                                   reinterpret_cast<const BYTE*>(empty), sizeof(wchar_t));
    RegCloseKey(h);
    if (de != ERROR_SUCCESS) return false;

    // HKLM Directory/Drive shell default is "none", so double-click never uses
    // the open verb and falls through to Folder → Explorer. Point HKCU at open.
    h = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, FolderShellKey(cls).c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS)
        return false;
    const wchar_t open_verb[] = L"open";
    const LONG def = RegSetValueExW(h, nullptr, 0, REG_SZ,
                                    reinterpret_cast<const BYTE*>(open_verb),
                                    sizeof(open_verb));
    RegCloseKey(h);
    return def == ERROR_SUCCESS;
}

bool ClearFolderOpenClass(const wchar_t* cls, const std::wstring& exe) {
    const std::wstring command = ReadRegDefault(FolderOpenKey(cls) + L"\\command");
    if (!command.empty() && !FolderOpenCommandIsOurs(command, exe)) return true;
    SHDeleteKeyW(HKEY_CURRENT_USER, FolderOpenKey(cls).c_str());
    const std::wstring shell = FolderShellKey(cls);
    if (_wcsicmp(ReadRegDefault(shell).c_str(), L"open") == 0) {
        HKEY h = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, shell.c_str(), 0, KEY_SET_VALUE, &h) == ERROR_SUCCESS) {
            RegDeleteValueW(h, nullptr);
            RegCloseKey(h);
        }
    }
    return true;
}

void NotifyAssocChanged() {
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

bool FolderOpenClassIsConfigured(const wchar_t* cls, const std::wstring& exe) {
    const std::wstring command = ReadRegDefault(FolderOpenKey(cls) + L"\\command");
    if (!FolderOpenCommandIsOurs(command, exe)) return false;
    // Keep the repair path for older installs that wrote the command but left
    // the class default verb as "none" (the HKLM default).
    return _wcsicmp(ReadRegDefault(FolderShellKey(cls)).c_str(), L"open") == 0;
}

bool FolderOpenClassNeedsClear(const wchar_t* cls, const std::wstring& exe) {
    const std::wstring command = ReadRegDefault(FolderOpenKey(cls) + L"\\command");
    if (!command.empty()) return FolderOpenCommandIsOurs(command, exe);
    // A previous cleanup or an interrupted registration can leave only the
    // HKCU shell default behind; ClearFolderOpenClass removes that residue.
    return _wcsicmp(ReadRegDefault(FolderShellKey(cls)).c_str(), L"open") == 0;
}

} // namespace

bool AppPrefs::ReadFolderOpen() const {
    const std::wstring exe = ExePath();
    if (exe.empty()) return false;
    const std::wstring command =
        ReadRegDefault(FolderOpenKey(L"Directory") + L"\\command");
    return FolderOpenCommandIsOurs(command, exe);
}

bool AppPrefs::ApplyFolderOpen(bool on) {
    if (!persist) { open_folders_in_pulse = on; return true; }
    const std::wstring exe = ExePath();
    if (exe.empty()) return false;
    bool ok = true;
    bool changed = false;
    for (const wchar_t* cls : kFolderOpenClasses) {
        if (on) {
            // Load() calls this to repair old registrations. Avoid rewriting
            // an already-correct association and rebroadcasting a global
            // Explorer refresh every time Pulse is launched by the shell.
            if (!FolderOpenClassIsConfigured(cls, exe)) {
                changed = true;
                ok = WriteFolderOpenClass(cls, exe) && ok;
            }
        } else {
            if (FolderOpenClassNeedsClear(cls, exe)) {
                changed = true;
                ok = ClearFolderOpenClass(cls, exe) && ok;
            }
        }
    }
    if (changed && ok) NotifyAssocChanged();
    if (!ok) {
        open_folders_in_pulse = ReadFolderOpen();
        return false;
    }
    open_folders_in_pulse = on;
    return ok;
}

namespace {
constexpr wchar_t kWinEVerbKey[] =
    L"Software\\Classes\\CLSID\\{52205fd8-5dfb-447d-801a-d0b52f2e83e1}\\shell\\opennewwindow";
constexpr wchar_t kWinEBackupValue[] = L"PulseBackup";

bool SetRegString(HKEY key, const wchar_t* name, const std::wstring& value) {
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                          static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

std::wstring ReadRegString(const std::wstring& path, const wchar_t* name) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS)
        return {};
    wchar_t value[2048]{};
    DWORD bytes = sizeof(value) - sizeof(wchar_t);
    DWORD type = 0;
    const LONG st = RegQueryValueExW(h, name, nullptr, &type, reinterpret_cast<LPBYTE>(value), &bytes);
    RegCloseKey(h);
    if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return {};
    return value;
}
} // namespace

bool AppPrefs::ReadWinE() const {
    return win_e_agent::Enabled(ExePath());
}

bool AppPrefs::ApplyWinE(bool on) {
    if (!persist) { take_over_win_e = on; return true; }
    const std::wstring exe = ExePath();
    if (exe.empty()) return false;
    if (!win_e_agent::SetEnabled(exe, on)) {
        take_over_win_e = ReadWinE();
        return false;
    }
    take_over_win_e = ReadWinE();
    const std::wstring verb = kWinEVerbKey;
    const std::wstring command_key = verb + L"\\command";
    const std::wstring current = ReadRegString(command_key, nullptr);
    const bool ours = FolderOpenCommandIsOurs(current, exe);
    if (!ours) return true; // someone else's command (or none): leave it alone
    const std::wstring backup = ReadRegString(command_key, kWinEBackupValue);
    if (!backup.empty()) {
        HKEY h = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, command_key.c_str(), 0, KEY_SET_VALUE, &h) != ERROR_SUCCESS)
            return false;
        bool ok = SetRegString(h, nullptr, backup);
        const LONG delegate_deleted = RegDeleteValueW(h, L"DelegateExecute");
        ok = (delegate_deleted == ERROR_SUCCESS || delegate_deleted == ERROR_FILE_NOT_FOUND) && ok;
        if (ok) RegDeleteValueW(h, kWinEBackupValue);
        RegCloseKey(h);
        if (!ok) {
            take_over_win_e = ReadWinE();
            return false;
        }
        return true;
    }
    const LONG deleted = SHDeleteKeyW(HKEY_CURRENT_USER, verb.c_str());
    if (deleted != ERROR_SUCCESS && deleted != ERROR_FILE_NOT_FOUND) return false;
    // Drop the now-empty parents so Explorer falls back to its HKLM defaults.
    const std::wstring shell = verb.substr(0, verb.rfind(L'\\'));
    SHDeleteEmptyKeyW(HKEY_CURRENT_USER, shell.c_str());
    SHDeleteEmptyKeyW(HKEY_CURRENT_USER, shell.substr(0, shell.rfind(L'\\')).c_str());
    return true;
}

int MenuRowHeightDip(int list_row_height) noexcept {
    return std::clamp(list_row_height + 2, 28, 40);
}

bool ParseAccentRgb(const std::wstring& text, uint32_t& rgb) noexcept {
    const wchar_t* p = text.c_str();
    if (!p || !*p) return false;
    if (*p == L'#') ++p;
    if (wcslen(p) != 6) return false;
    for (int i = 0; i < 6; ++i) {
        if (!iswxdigit(p[i])) return false;
    }
    wchar_t* end = nullptr;
    const unsigned long v = wcstoul(p, &end, 16);
    if (!end || *end != L'\0' || v > 0xFFFFFFul) return false;
    rgb = static_cast<uint32_t>(v);
    return true;
}

bool AppPrefs::Load() {
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty()) {
        launch_on_startup = ReadLaunchOnStartup();
        open_folders_in_pulse = ReadFolderOpen();
        take_over_win_e = ReadWinE();
        if (persist) {
            const std::wstring exe = ExePath();
            const std::wstring legacy_command = std::wstring(kWinEVerbKey) + L"\\command";
            if (FolderOpenCommandIsOurs(ReadRegString(legacy_command, nullptr), exe))
                ApplyWinE(true);
        }
        return false;
    }
    std::wstring json;
    if (ReadUtf8File(dir + L"\\app.json", json) && !json.empty()) {
        had_file = true;
        FromJson(json);
    }
    launch_on_startup = ReadLaunchOnStartup();
    open_folders_in_pulse = ReadFolderOpen();
    take_over_win_e = ReadWinE();
    if (persist) {
        const std::wstring exe = ExePath();
        const std::wstring legacy_command = std::wstring(kWinEVerbKey) + L"\\command";
        if (FolderOpenCommandIsOurs(ReadRegString(legacy_command, nullptr), exe))
            ApplyWinE(true);
        else if (take_over_win_e) win_e_agent::EnsureRunning(exe);
    }
    // Repair older installs that wrote open\command but left shell default as none.
    if (persist && open_folders_in_pulse)
        ApplyFolderOpen(true);
    return true;
}

bool AppPrefs::Save() const {
    if (!persist) return true;
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty()) return false;
    return WriteUtf8FileAtomic(dir + L"\\app.json", ToJson());
}

} // namespace pulse::app
