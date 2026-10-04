#include "../shell_host/ctx_handlers.h"

#include <cstdio>
#include <cwchar>

namespace {

bool passed = true;

void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    passed = passed && ok;
}

class DynamicMenu final : public IContextMenu3 {
public:
    HMENU submenu = nullptr;
    bool initialized = false;
    bool invoked = false;
    bool populate = true;
    bool plain_send_to = false;   // placeholder row without a flyout
    bool slow_before = false;     // a slow flyout above Send to
    HMENU slow = nullptr;
    bool slow_initialized = false;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IContextMenu &&
            iid != IID_IContextMenu2 && iid != IID_IContextMenu3) return E_NOINTERFACE;
        *out = static_cast<IContextMenu3*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs_; }
    HRESULT STDMETHODCALLTYPE QueryContextMenu(HMENU menu, UINT, UINT first, UINT, UINT) override {
        first_ = first;
        submenu = plain_send_to ? nullptr : CreatePopupMenu();
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_ID | MIIM_STRING | (submenu ? MIIM_SUBMENU : 0u);
        item.wID = first;
        item.dwTypeData = const_cast<wchar_t*>(L"Send to");
        item.hSubMenu = submenu;
        if (!InsertMenuItemW(menu, 1, TRUE, &item)) return E_FAIL;
        sendto_pos_ = 1;
        if (slow_before) {
            slow = CreatePopupMenu();
            MENUITEMINFOW s{sizeof(s)};
            s.fMask = MIIM_ID | MIIM_STRING | MIIM_SUBMENU;
            s.wID = first + 2;
            s.dwTypeData = const_cast<wchar_t*>(L"Slow handler");
            s.hSubMenu = slow;
            if (!InsertMenuItemW(menu, 0, TRUE, &s)) return E_FAIL;
            sendto_pos_ = 2;
            return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 4);
        }
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 2);
    }
    HRESULT STDMETHODCALLTYPE InvokeCommand(CMINVOKECOMMANDINFO* info) override {
        invoked = info && IS_INTRESOURCE(info->lpVerb) && LOWORD(info->lpVerb) == 1;
        return invoked ? S_OK : E_INVALIDARG;
    }
    HRESULT STDMETHODCALLTYPE GetCommandString(UINT_PTR offset, UINT flags, UINT*, CHAR* out,
                                               UINT count) override {
        if (flags != GCS_VERBW || !out) return E_NOTIMPL;
        return wcscpy_s(reinterpret_cast<wchar_t*>(out), count,
                        offset == 0 ? L"sendto" : offset == 2 ? L"slowverb" : L"sendtarget") == 0
            ? S_OK : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE HandleMenuMsg(UINT message, WPARAM wparam, LPARAM lparam) override {
        if (message == WM_INITMENUPOPUP && slow && reinterpret_cast<HMENU>(wparam) == slow) {
            Sleep(150); // longer than the shared nested-flyout budget
            if (!slow_initialized) AppendMenuW(slow, MF_STRING, first_ + 3, L"Slow item");
            slow_initialized = true;
            return S_OK;
        }
        if (message != WM_INITMENUPOPUP || reinterpret_cast<HMENU>(wparam) != submenu ||
            LOWORD(lparam) != sendto_pos_ || HIWORD(lparam) != FALSE) return E_INVALIDARG;
        if (!initialized && populate) AppendMenuW(submenu, MF_STRING, first_ + 1, L"Test destination");
        initialized = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE HandleMenuMsg2(UINT message, WPARAM wparam, LPARAM lparam,
                                           LRESULT* result) override {
        if (result) *result = 0;
        return HandleMenuMsg(message, wparam, lparam);
    }

private:
    ULONG refs_ = 1;
    UINT first_ = 0;
    UINT sendto_pos_ = 1;
};

void TestDynamicMenu(bool version3, bool send_to_only = false, bool empty = false) {
    DynamicMenu menu;
    menu.populate = !empty;
    pulse::shell::CtxHandlerSlot slot;
    slot.menu = &menu;
    slot.menu2 = &menu;
    slot.menu3 = version3 ? &menu : nullptr;
    slot.hmenu = CreatePopupMenu();
    slot.id_first = 100;
    slot.id_last = 355;
    if (send_to_only) slot.send_to_title = L"Send to";
    menu.QueryContextMenu(slot.hmenu, 0, slot.id_first, slot.id_last, CMF_NORMAL);
    if (send_to_only) AppendMenuW(slot.hmenu, MF_STRING, 102, L"Unrelated command");
    std::vector<pulse::shell::CtxItemOut> items;
    pulse::shell::CollectHandlerItems(slot, false, items);
    Check(menu.initialized, version3 ? "IContextMenu3 initializes a normal submenu" :
                                     "IContextMenu2 initializes a normal submenu");
    const bool children = items.size() == 2 && items[0].has_children && items[0].id == 0 &&
                          items[1].child && items[1].id == 101;
    if (empty) {
        Check(items.size() == 1 && !items.front().enabled && !items.front().has_children,
              "empty Send to is disabled instead of an inert clickable parent");
    } else {
        Check(children, send_to_only ? "Send to excludes unrelated default Shell commands" :
                                     "dynamic Send to is a flyout with its destination command");
    }
    if (children) {
        CMINVOKECOMMANDINFO info{sizeof(info)};
        info.lpVerb = MAKEINTRESOURCEA(items[1].id - slot.id_first);
        Check(SUCCEEDED(menu.InvokeCommand(&info)) && menu.invoked,
              "destination keeps the handler command offset");
    }
    DestroyMenu(slot.hmenu);
}

void TestSendToAfterSlowFlyout() {
    DynamicMenu menu;
    menu.slow_before = true;
    pulse::shell::CtxHandlerSlot slot;
    slot.menu = &menu;
    slot.menu2 = &menu;
    slot.menu3 = &menu;
    slot.hmenu = CreatePopupMenu();
    slot.id_first = 100;
    slot.id_last = 355;
    menu.QueryContextMenu(slot.hmenu, 0, slot.id_first, slot.id_last, CMF_NORMAL);
    std::vector<pulse::shell::CtxItemOut> items;
    pulse::shell::CollectHandlerItems(slot, false, items);
    bool flyout = false;
    for (size_t i = 0; i + 1 < items.size(); ++i)
        if (items[i].verb == L"sendto" && items[i].has_children && items[i + 1].child &&
            items[i + 1].id == 101) flyout = true;
    Check(menu.slow_initialized && menu.initialized && flyout,
          "Send to still fills after a slow flyout used up the shared budget (#77)");
    DestroyMenu(slot.hmenu);
}

void TestPlainSendToPlaceholder() {
    DynamicMenu menu;
    menu.plain_send_to = true;
    pulse::shell::CtxHandlerSlot slot;
    slot.menu = &menu;
    slot.menu2 = &menu;
    slot.menu3 = &menu;
    slot.hmenu = CreatePopupMenu();
    slot.id_first = 100;
    slot.id_last = 355;
    slot.send_to_title = L"Send to";
    menu.QueryContextMenu(slot.hmenu, 0, slot.id_first, slot.id_last, CMF_NORMAL);
    std::vector<pulse::shell::CtxItemOut> items;
    pulse::shell::CollectHandlerItems(slot, false, items);
    Check(items.size() == 1 && items[0].text == L"Send to" && !items[0].enabled &&
              !items[0].has_children,
          "a Send to row without destinations is disabled, never an inert command (#77)");
    DestroyMenu(slot.hmenu);
}

void TestInstalledSendTo(const wchar_t* fixture = nullptr) {
    wchar_t windows[MAX_PATH]{};
    GetWindowsDirectoryW(windows, MAX_PATH);
    const std::wstring path = fixture ? std::wstring(fixture) : std::wstring(windows) + L"\\win.ini";
    pulse::shell::CtxHandlerDesc handler;
    handler.clsid_text = L"{7BA4C740-9E81-11CF-99D3-00AA004AE837}";
    handler.name = L"SendTo";
    CLSIDFromString(handler.clsid_text.c_str(), &handler.clsid);
    pulse::shell::CtxBind bind;
    Check(pulse::shell::BindCtxSelection({path}, false, bind), "bind read-only Send to fixture");
    pulse::shell::CtxHandlerSlot slot;
    const HRESULT hr = pulse::shell::QueryOneHandler(handler, bind, nullptr, 100, 355,
                                                    CMF_NORMAL, slot);
    Check(SUCCEEDED(hr), "query installed Send to handler");
    std::vector<pulse::shell::CtxItemOut> items;
    pulse::shell::CollectHandlerItems(slot, false, items);
    size_t children = 0;
    bool isolated = true;
    for (const auto& item : items) {
        if (item.child && item.enabled) ++children;
        if ((!item.child && item.verb != L"sendto") ||
            (item.id && (item.id < slot.id_first || item.id > slot.id_last))) isolated = false;
    }
    std::printf("[INFO] installed Send to: %zu rows, %zu enabled destinations\n", items.size(), children);
    Check(!items.empty() && items.front().has_children && children > 0,
          "installed Send to exposes destinations instead of an inert parent");
    Check(isolated, "Send to exposes only its own group and allocated command IDs");
    pulse::shell::ReleaseHandlerSlot(slot);
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    TestDynamicMenu(false);
    TestDynamicMenu(true);
    TestDynamicMenu(true, true);
    TestDynamicMenu(true, true, true);
    TestDynamicMenu(true, false, true); // default menu: empty "sendto" flyout
    TestSendToAfterSlowFlyout();
    TestPlainSendToPlaceholder();
    if (argc > 1 && wcscmp(argv[1], L"--sendto") == 0) TestInstalledSendTo(argc > 2 ? argv[2] : nullptr);
    CoUninitialize();
    return passed ? 0 : 1;
}
