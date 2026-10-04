// shell_window_registry.cpp — see shell_window_registry.h.
#include "shell_window_registry.h"

// shlobj.h first: it brings in objbase (`interface`) under WIN32_LEAN_AND_MEAN.
#include <shlobj.h>
#include <exdisp.h>
#include <propvarutil.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <mutex>
#include <optional>
#include <type_traits>

namespace pulse::app {

using Microsoft::WRL::ComPtr;

struct ShellWindowRegistry::Shared {
    HWND window = nullptr;
    DWORD window_thread = 0;   // owns `window`; Register pairs with RegisterPending by it
    UINT select_message = 0;
    std::mutex mutex;
    std::optional<std::vector<ShellWindowEntry>> pending;
    DWORD thread_id = 0;
    std::atomic<bool> stopping{false};
};

namespace {

constexpr UINT kWakeMessage = WM_APP + 1;   // thread message: a new wanted set or Stop

} // namespace

void TraceShellWindows(const wchar_t* format, ...) {
#ifdef PULSE_WITH_SELFTEST
    static const std::wstring path = [] {
        wchar_t buffer[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(L"PULSE_TEST_SHELL_WINDOWS_LOG", buffer, MAX_PATH);
        return length > 0 && length < MAX_PATH ? std::wstring(buffer) : std::wstring();
    }();
    if (path.empty()) return;
    wchar_t line[1024]{};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(line, _TRUNCATE, format, args);
    va_end(args);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char text[1200]{};
    const int prefix = sprintf_s(text, "%02u:%02u:%02u.%03u ", now.wHour, now.wMinute, now.wSecond,
                                 now.wMilliseconds);
    const int body = WideCharToMultiByte(CP_UTF8, 0, line, -1, text + prefix,
                                         static_cast<int>(sizeof(text)) - prefix - 2, nullptr, nullptr);
    if (body <= 0) return;
    const size_t end = static_cast<size_t>(prefix + body - 1);
    text[end] = '\n';
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, text, static_cast<DWORD>(end + 1), &written, nullptr);
    CloseHandle(file);
#else
    (void)format;
#endif
}

namespace {


struct PidlDeleter {
    void operator()(std::remove_pointer_t<PIDLIST_ABSOLUTE>* pidl) const noexcept { CoTaskMemFree(pidl); }
};
using UniquePidl = std::unique_ptr<std::remove_pointer_t<PIDLIST_ABSOLUTE>, PidlDeleter>;

UniquePidl FolderPidl(const std::wstring& path) {
    PIDLIST_ABSOLUTE pidl = nullptr;
    const HRESULT hr = path.empty()
        ? SHGetKnownFolderIDList(FOLDERID_ComputerFolder, 0, nullptr, &pidl)
        : SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr);
    return SUCCEEDED(hr) ? UniquePidl(pidl) : UniquePidl();
}

std::wstring PidlName(PCIDLIST_ABSOLUTE pidl, SIGDN kind) {
    PWSTR name = nullptr;
    if (!pidl || FAILED(SHGetNameFromIDList(pidl, kind, &name)) || !name) return {};
    std::wstring result = name;
    CoTaskMemFree(name);
    return result;
}

HRESULT ReturnBstr(const std::wstring& text, BSTR* out) {
    if (!out) return E_POINTER;
    *out = SysAllocString(text.c_str());
    return *out ? S_OK : E_OUTOFMEMORY;
}

// IShellView over one pane: only SelectItem does anything. The shell asks
// for it through the document's IServiceProvider (IID_IFolderView service).
class FolderView final : public IShellView {
public:
    FolderView(uint64_t key, HWND window, UINT message) : key_(key), window_(window), message_(message) {}
    void SetFolder(UniquePidl folder) { folder_ = std::move(folder); }
    PCIDLIST_ABSOLUTE Folder() const { return folder_.get(); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IOleWindow || riid == IID_IShellView) {
            *ppv = static_cast<IShellView*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(++refs_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const long refs = --refs_;
        if (refs == 0) delete this;
        return static_cast<ULONG>(refs);
    }

    IFACEMETHODIMP GetWindow(HWND* hwnd) override {
        if (!hwnd) return E_POINTER;
        *hwnd = window_;
        return S_OK;
    }
    IFACEMETHODIMP ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }

    IFACEMETHODIMP TranslateAccelerator(MSG*) override { return S_FALSE; }
    IFACEMETHODIMP EnableModeless(BOOL) override { return E_NOTIMPL; }
    IFACEMETHODIMP UIActivate(UINT) override { return E_NOTIMPL; }
    IFACEMETHODIMP Refresh() override { return E_NOTIMPL; }
    IFACEMETHODIMP CreateViewWindow(IShellView*, LPCFOLDERSETTINGS, IShellBrowser*, RECT*, HWND*) override {
        return E_NOTIMPL;
    }
    IFACEMETHODIMP DestroyViewWindow() override { return E_NOTIMPL; }
    IFACEMETHODIMP GetCurrentInfo(LPFOLDERSETTINGS) override { return E_NOTIMPL; }
    IFACEMETHODIMP AddPropertySheetPages(DWORD, LPFNSVADDPROPSHEETPAGE, LPARAM) override { return E_NOTIMPL; }
    IFACEMETHODIMP SaveViewState() override { return E_NOTIMPL; }
    IFACEMETHODIMP SelectItem(PCUITEMID_CHILD item, SVSIF flags) override {
        TraceShellWindows(L"SelectItem key=%llx flags=0x%x", static_cast<unsigned long long>(key_), flags);
        if (!item) return E_INVALIDARG;
        // SVSI_DESELECT (0) alone: nothing to show.
        if ((flags & (SVSI_SELECT | SVSI_EDIT | SVSI_FOCUSED | SVSI_ENSUREVISIBLE)) == 0) return S_OK;
        if (!folder_) return E_FAIL;
        UniquePidl full(ILCombine(folder_.get(), item));
        if (!full) return E_OUTOFMEMORY;
        auto request = std::make_unique<ShellSelectRequest>();
        request->key = key_;
        request->path = PidlName(full.get(), SIGDN_DESKTOPABSOLUTEPARSING);
        request->flags = flags;
        if (request->path.empty()) return E_FAIL;
        if (PostMessageW(window_, message_, 0, reinterpret_cast<LPARAM>(request.get()))) request.release();
        return S_OK;
    }
    IFACEMETHODIMP GetItemObject(UINT, REFIID, void** ppv) override {
        if (ppv) *ppv = nullptr;
        return E_NOTIMPL;
    }

private:
    ~FolderView() = default;
    std::atomic<long> refs_{1};
    const uint64_t key_;
    const HWND window_;
    const UINT message_;
    UniquePidl folder_;
};

// IWebBrowserApp::get_Document result. The shell only queries it for
// IServiceProvider; the IDispatch side has no members.
class FolderDocument final : public IDispatch, public IServiceProvider {
public:
    explicit FolderDocument(FolderView* view) : view_(view) {}

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDispatch) *ppv = static_cast<IDispatch*>(this);
        else if (riid == IID_IServiceProvider) *ppv = static_cast<IServiceProvider*>(this);
        else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(++refs_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const long refs = --refs_;
        if (refs == 0) delete this;
        return static_cast<ULONG>(refs);
    }

    IFACEMETHODIMP GetTypeInfoCount(UINT* count) override {
        if (!count) return E_POINTER;
        *count = 0;
        return S_OK;
    }
    IFACEMETHODIMP GetTypeInfo(UINT, LCID, ITypeInfo** info) override {
        if (info) *info = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return DISP_E_UNKNOWNNAME; }
    IFACEMETHODIMP Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override {
        return DISP_E_MEMBERNOTFOUND;
    }

    IFACEMETHODIMP QueryService(REFGUID service, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        wchar_t guid[40]{};
        StringFromGUID2(service, guid, ARRAYSIZE(guid));
        TraceShellWindows(L"QueryService %s", guid);
        if (service == IID_IFolderView || service == IID_IShellView) return view_->QueryInterface(riid, ppv);
        return E_NOINTERFACE;
    }

private:
    ~FolderDocument() = default;
    std::atomic<long> refs_{1};
    ComPtr<FolderView> view_;
};

// What IShellWindows holds for a pane. Location* also lets programs that list
// shell windows (Shell.Application.Windows()) see which folder a pane shows.
class BrowserApp final : public IWebBrowserApp {
public:
    BrowserApp(HWND window, FolderDocument* document, FolderView* view)
        : window_(window), document_(document), view_(view) {
        // SHDocVw's type library gives scripts (Shell.Application.Windows())
        // names for the properties below. {EAB22AC0-30C1-11CF-A7EB-0000C05BAE0B}
        static constexpr GUID kLibShDocVw = {0xEAB22AC0, 0x30C1, 0x11CF, {0xA7, 0xEB, 0x00, 0x00, 0xC0, 0x5B, 0xAE, 0x0B}};
        ComPtr<ITypeLib> library;
        if (SUCCEEDED(LoadRegTypeLib(kLibShDocVw, 1, 1, LOCALE_NEUTRAL, &library)))
            library->GetTypeInfoOfGuid(IID_IWebBrowserApp, &type_info_);
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDispatch || riid == IID_IWebBrowser ||
            riid == IID_IWebBrowserApp) {
            *ppv = static_cast<IWebBrowserApp*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(++refs_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const long refs = --refs_;
        if (refs == 0) delete this;
        return static_cast<ULONG>(refs);
    }

    // IDispatch
    IFACEMETHODIMP GetTypeInfoCount(UINT* count) override {
        if (!count) return E_POINTER;
        *count = type_info_ ? 1 : 0;
        return S_OK;
    }
    IFACEMETHODIMP GetTypeInfo(UINT index, LCID, ITypeInfo** info) override {
        if (!info) return E_POINTER;
        *info = nullptr;
        if (index != 0 || !type_info_) return DISP_E_BADINDEX;
        return type_info_.CopyTo(info);
    }
    IFACEMETHODIMP GetIDsOfNames(REFIID, LPOLESTR* names, UINT count, LCID, DISPID* ids) override {
        if (!type_info_) return DISP_E_UNKNOWNNAME;
        return DispGetIDsOfNames(type_info_.Get(), names, count, ids);
    }
    IFACEMETHODIMP Invoke(DISPID id, REFIID, LCID, WORD flags, DISPPARAMS* params, VARIANT* result,
                          EXCEPINFO* exception, UINT* arg_error) override {
        if (!type_info_) return DISP_E_MEMBERNOTFOUND;
        return DispInvoke(static_cast<IWebBrowserApp*>(this), type_info_.Get(), id, flags, params, result,
                          exception, arg_error);
    }

    // IWebBrowser
    IFACEMETHODIMP GoBack() override { return E_NOTIMPL; }
    IFACEMETHODIMP GoForward() override { return E_NOTIMPL; }
    IFACEMETHODIMP GoHome() override { return E_NOTIMPL; }
    IFACEMETHODIMP GoSearch() override { return E_NOTIMPL; }
    IFACEMETHODIMP Navigate(BSTR, VARIANT*, VARIANT*, VARIANT*, VARIANT*) override { return E_NOTIMPL; }
    IFACEMETHODIMP Refresh() override { return E_NOTIMPL; }
    IFACEMETHODIMP Refresh2(VARIANT*) override { return E_NOTIMPL; }
    IFACEMETHODIMP Stop() override { return E_NOTIMPL; }
    IFACEMETHODIMP get_Application(IDispatch** out) override { return NoDispatch(out); }
    IFACEMETHODIMP get_Parent(IDispatch** out) override { return NoDispatch(out); }
    IFACEMETHODIMP get_Container(IDispatch** out) override { return NoDispatch(out); }
    IFACEMETHODIMP get_Document(IDispatch** out) override {
        if (!out) return E_POINTER;
        *out = static_cast<IDispatch*>(document_.Get());
        (*out)->AddRef();
        return S_OK;
    }
    IFACEMETHODIMP get_TopLevelContainer(VARIANT_BOOL* out) override { return Bool(out, true); }
    IFACEMETHODIMP get_Type(BSTR* out) override { return NoBstr(out); }
    IFACEMETHODIMP get_Left(long*) override { return E_NOTIMPL; }
    IFACEMETHODIMP put_Left(long) override { return E_NOTIMPL; }
    IFACEMETHODIMP get_Top(long*) override { return E_NOTIMPL; }
    IFACEMETHODIMP put_Top(long) override { return E_NOTIMPL; }
    IFACEMETHODIMP get_Width(long*) override { return E_NOTIMPL; }
    IFACEMETHODIMP put_Width(long) override { return E_NOTIMPL; }
    IFACEMETHODIMP get_Height(long*) override { return E_NOTIMPL; }
    IFACEMETHODIMP put_Height(long) override { return E_NOTIMPL; }
    IFACEMETHODIMP get_LocationName(BSTR* out) override {
        return ReturnBstr(PidlName(view_->Folder(), SIGDN_NORMALDISPLAY), out);
    }
    IFACEMETHODIMP get_LocationURL(BSTR* out) override {
        // Explorer reports "" for folders without a file system path (This PC).
        const std::wstring path = PidlName(view_->Folder(), SIGDN_FILESYSPATH);
        std::wstring url;
        if (!path.empty()) {
            wchar_t buffer[2084]{};   // INTERNET_MAX_URL_LENGTH (wininet.h)
            DWORD length = ARRAYSIZE(buffer);
            if (SUCCEEDED(UrlCreateFromPathW(path.c_str(), buffer, &length, 0))) url = buffer;
        }
        return ReturnBstr(url, out);
    }
    IFACEMETHODIMP get_Busy(VARIANT_BOOL* out) override { return Bool(out, false); }

    // IWebBrowserApp
    IFACEMETHODIMP Quit() override { return E_NOTIMPL; }
    IFACEMETHODIMP ClientToWindow(int*, int*) override { return E_NOTIMPL; }
    IFACEMETHODIMP PutProperty(BSTR, VARIANT) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetProperty(BSTR, VARIANT*) override { return E_NOTIMPL; }
    IFACEMETHODIMP get_Name(BSTR* out) override { return ReturnBstr(L"Pulse", out); }
    IFACEMETHODIMP get_HWND(SHANDLE_PTR* out) override {
        if (!out) return E_POINTER;
        *out = reinterpret_cast<SHANDLE_PTR>(window_);
        return S_OK;
    }
    IFACEMETHODIMP get_FullName(BSTR* out) override { return NoBstr(out); }
    IFACEMETHODIMP get_Path(BSTR* out) override { return NoBstr(out); }
    IFACEMETHODIMP get_Visible(VARIANT_BOOL* out) override { return Bool(out, IsWindowVisible(window_) != FALSE); }
    IFACEMETHODIMP put_Visible(VARIANT_BOOL) override { return E_NOTIMPL; }
    IFACEMETHODIMP get_StatusBar(VARIANT_BOOL*) override { return E_NOTIMPL; }
    IFACEMETHODIMP put_StatusBar(VARIANT_BOOL) override { return E_NOTIMPL; }
    IFACEMETHODIMP get_StatusText(BSTR* out) override { return NoBstr(out); }
    IFACEMETHODIMP put_StatusText(BSTR) override { return E_NOTIMPL; }
    IFACEMETHODIMP get_ToolBar(int*) override { return E_NOTIMPL; }
    IFACEMETHODIMP put_ToolBar(int) override { return E_NOTIMPL; }
    IFACEMETHODIMP get_MenuBar(VARIANT_BOOL*) override { return E_NOTIMPL; }
    IFACEMETHODIMP put_MenuBar(VARIANT_BOOL) override { return E_NOTIMPL; }
    IFACEMETHODIMP get_FullScreen(VARIANT_BOOL*) override { return E_NOTIMPL; }
    IFACEMETHODIMP put_FullScreen(VARIANT_BOOL) override { return E_NOTIMPL; }

private:
    ~BrowserApp() = default;
    static HRESULT NoDispatch(IDispatch** out) {
        if (out) *out = nullptr;
        return E_NOTIMPL;
    }
    static HRESULT NoBstr(BSTR* out) {
        if (out) *out = nullptr;
        return E_NOTIMPL;
    }
    static HRESULT Bool(VARIANT_BOOL* out, bool value) {
        if (!out) return E_POINTER;
        *out = value ? VARIANT_TRUE : VARIANT_FALSE;
        return S_OK;
    }

    std::atomic<long> refs_{1};
    const HWND window_;
    ComPtr<ITypeInfo> type_info_;
    ComPtr<FolderDocument> document_;
    ComPtr<FolderView> view_;
};

// Lives on the registry thread; owns every IShellWindows call.
class Worker {
public:
    explicit Worker(ShellWindowRegistry::Shared& shared) : shared_(shared) {}
    ~Worker() { RevokeAll(); }
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    void Apply() {
        {
            std::lock_guard<std::mutex> lock(shared_.mutex);
            if (shared_.pending) {
                wanted_ = std::move(*shared_.pending);
                shared_.pending.reset();
                dirty_ = true;
            }
        }
        if (!dirty_) return;
        // No Explorer shell (yet): keep the set and retry on the next publish.
        if (!windows_) {
            const HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows_));
            TraceShellWindows(L"CoCreateInstance(ShellWindows) hr=0x%08x", static_cast<unsigned>(hr));
            if (FAILED(hr)) return;
        }
        TraceShellWindows(L"apply wanted=%zu registered=%zu", wanted_.size(), registered_.size());
        dirty_ = false;
        for (const ShellWindowAction& action : PlanShellWindowChanges(Current(), wanted_)) {
            switch (action.kind) {
            case ShellWindowActionKind::Revoke: Revoke(action.key); break;
            case ShellWindowActionKind::Register: Register(action.key, action.path); break;
            case ShellWindowActionKind::Navigate: Navigate(action.key, action.path); break;
            }
        }
    }

    void RevokeAll() {
        while (!registered_.empty()) Revoke(registered_.begin()->first);
        windows_.Reset();
    }

private:
    struct Registered {
        long cookie = 0;             // RegisterPending
        long window_cookie = 0;      // Register (normally the same window entry)
        std::wstring path;
        ComPtr<FolderView> view;
        ComPtr<BrowserApp> app;
    };

    std::vector<ShellWindowEntry> Current() const {
        std::vector<ShellWindowEntry> current;
        current.reserve(registered_.size());
        for (const auto& [key, entry] : registered_) current.push_back({key, entry.path});
        return current;
    }

    void Register(uint64_t key, const std::wstring& path) {
        UniquePidl folder = FolderPidl(path);
        TraceShellWindows(L"register key=%llx path=%s pidl=%d", static_cast<unsigned long long>(key), path.c_str(),
              folder ? 1 : 0);
        if (!folder) return;
        VARIANT location;
        VariantInit(&location);
        if (FAILED(InitVariantFromBuffer(folder.get(), ILGetSize(folder.get()), &location))) return;
        VARIANT root;
        VariantInit(&root);
        Registered entry;
        entry.path = path;
        // Pending first: SHOpenFolderAndSelectItems that just launched Pulse
        // for this folder only asks windows registered as pending for it.
        // Register then turns that entry into the window, matched by the
        // thread that owns the hwnd (the UI thread, not this one); a
        // mismatch leaves a pending entry the shell waits on until timeout.
        HRESULT hr = windows_->RegisterPending(static_cast<long>(shared_.window_thread), &location, &root,
                                               SWC_BROWSER, &entry.cookie);
        if (SUCCEEDED(hr)) {
            entry.view.Attach(new FolderView(key, shared_.window, shared_.select_message));
            entry.view->SetFolder(std::move(folder));
            ComPtr<FolderDocument> document;
            document.Attach(new FolderDocument(entry.view.Get()));
            entry.app.Attach(new BrowserApp(shared_.window, document.Get(), entry.view.Get()));
            hr = windows_->Register(entry.app.Get(), HandleToLong(shared_.window), SWC_BROWSER,
                                    &entry.window_cookie);
            TraceShellWindows(L"Register hr=0x%08x pending_cookie=%ld window_cookie=%ld", static_cast<unsigned>(hr),
                  entry.cookie, entry.window_cookie);
            if (SUCCEEDED(hr)) windows_->OnNavigate(entry.cookie, &location);
            else windows_->Revoke(entry.cookie);
        } else {
            TraceShellWindows(L"RegisterPending hr=0x%08x", static_cast<unsigned>(hr));
        }
        VariantClear(&location);
        if (SUCCEEDED(hr)) registered_[key] = std::move(entry);
    }

    void Navigate(uint64_t key, const std::wstring& path) {
        const auto it = registered_.find(key);
        if (it == registered_.end()) return;
        UniquePidl folder = FolderPidl(path);
        if (!folder) {
            Revoke(key);   // no shell folder any more (deleted, offline share)
            return;
        }
        VARIANT location;
        VariantInit(&location);
        if (FAILED(InitVariantFromBuffer(folder.get(), ILGetSize(folder.get()), &location))) return;
        it->second.view->SetFolder(std::move(folder));
        it->second.path = path;
        const HRESULT hr = windows_->OnNavigate(it->second.cookie, &location);
        TraceShellWindows(L"navigate key=%llx path=%s hr=0x%08x", static_cast<unsigned long long>(key), path.c_str(),
              static_cast<unsigned>(hr));
        VariantClear(&location);
    }

    void Revoke(uint64_t key) {
        const auto it = registered_.find(key);
        if (it == registered_.end()) return;
        TraceShellWindows(L"revoke key=%llx", static_cast<unsigned long long>(key));
        if (windows_) {
            windows_->Revoke(it->second.cookie);
            if (it->second.window_cookie && it->second.window_cookie != it->second.cookie)
                windows_->Revoke(it->second.window_cookie);
        }
        // The shell may still hold the objects; an emptied view selects nothing.
        if (it->second.view) it->second.view->SetFolder(UniquePidl());
        registered_.erase(it);
    }

    ShellWindowRegistry::Shared& shared_;
    ComPtr<IShellWindows> windows_;
    std::map<uint64_t, Registered> registered_;
    std::vector<ShellWindowEntry> wanted_;
    bool dirty_ = false;
};

DWORD WINAPI RegistryThread(void* param) {
    std::shared_ptr<ShellWindowRegistry::Shared> shared =
        std::move(*static_cast<std::shared_ptr<ShellWindowRegistry::Shared>*>(param));
    delete static_cast<std::shared_ptr<ShellWindowRegistry::Shared>*>(param);
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    TraceShellWindows(L"thread start CoInitializeEx hr=0x%08x", static_cast<unsigned>(init));
    if (FAILED(init)) return 1;
    {
        // Create the message queue before the first Apply: a Publish that
        // failed to post earlier is still waiting in `pending`.
        MSG msg{};
        PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        Worker worker(*shared);
        if (!shared->stopping) worker.Apply();
        while (!shared->stopping && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (!msg.hwnd && msg.message == kWakeMessage) {
                if (!shared->stopping) worker.Apply();
                continue;
            }
            // COM delivers the shell's calls through this loop.
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }   // ~Worker revokes everything while COM is still up
    CoUninitialize();
    return 0;
}

} // namespace

ShellWindowRegistry::ShellWindowRegistry(HWND window, UINT select_message)
    : shared_(std::make_shared<Shared>()) {
    shared_->window = window;
    shared_->window_thread = GetWindowThreadProcessId(window, nullptr);
    shared_->select_message = select_message;
    auto* param = new std::shared_ptr<Shared>(shared_);
    DWORD thread_id = 0;
    thread_ = CreateThread(nullptr, 0, &RegistryThread, param, 0, &thread_id);
    if (!thread_) {
        delete param;
        return;
    }
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->thread_id = thread_id;
}

ShellWindowRegistry::~ShellWindowRegistry() { Stop(); }

void ShellWindowRegistry::Publish(std::vector<ShellWindowEntry> wanted) {
    DWORD thread_id = 0;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->pending = std::move(wanted);
        thread_id = shared_->thread_id;
    }
    if (thread_id) PostThreadMessageW(thread_id, kWakeMessage, 0, 0);
}

void ShellWindowRegistry::Stop(DWORD timeout_ms) {
    if (!thread_) return;
    shared_->stopping = true;
    DWORD thread_id = 0;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        thread_id = shared_->thread_id;
    }
    if (thread_id) PostThreadMessageW(thread_id, kWakeMessage, 0, 0);
    WaitForSingleObject(thread_, timeout_ms);
    CloseHandle(thread_);
    thread_ = nullptr;
}

} // namespace pulse::app
