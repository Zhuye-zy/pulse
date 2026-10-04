// explorer_window_takeover.cpp — see explorer_window_takeover.h.
#include "explorer_window_takeover.h"

#include "shell_window_plan.h"
#include "shell_window_registry.h"   // TraceShellWindows

// shlobj.h first: it brings in objbase (`interface`) under WIN32_LEAN_AND_MEAN.
#include <shlobj.h>
#include <exdisp.h>
#include <exdispid.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <atomic>
#include <algorithm>
#include <mutex>
#include <set>
#include <type_traits>

namespace pulse::app {

using Microsoft::WRL::ComPtr;

struct ExplorerWindowTakeover::Shared {
    HWND window = nullptr;
    UINT message = 0;
    std::atomic<bool> stopping{false};
    std::atomic<DWORD> thread_id{0};
    std::mutex handoffs_mutex;
    std::vector<std::weak_ptr<ExplorerHandoff>> handoffs;
};

namespace {

constexpr UINT kStopMessage = WM_APP + 1;   // thread messages
constexpr UINT kScanMessage = WM_APP + 2;
constexpr UINT kPollMs = 50;

struct PidlDeleter {
    void operator()(std::remove_pointer_t<PIDLIST_ABSOLUTE>* pidl) const noexcept { CoTaskMemFree(pidl); }
};
using UniquePidl = std::unique_ptr<std::remove_pointer_t<PIDLIST_ABSOLUTE>, PidlDeleter>;

bool IsExplorerProcess(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t image[MAX_PATH]{};
    DWORD size = ARRAYSIZE(image);
    const bool ok = QueryFullProcessImageNameW(process, 0, image, &size) != FALSE;
    CloseHandle(process);
    return ok && _wcsicmp(PathFindFileNameW(image), L"explorer.exe") == 0;
}

bool IsThisPc(PCIDLIST_ABSOLUTE pidl) {
    PIDLIST_ABSOLUTE computer = nullptr;
    if (FAILED(SHGetKnownFolderIDList(FOLDERID_ComputerFolder, 0, nullptr, &computer))) return false;
    const bool same = ILIsEqual(pidl, computer) != FALSE;
    CoTaskMemFree(computer);
    return same;
}

// DShellWindowsEvents sink. The scan runs from the message loop rather than
// inside Explorer's callback.
class WindowEvents final : public IDispatch {
public:
    explicit WindowEvents(DWORD thread_id) : thread_id_(thread_id) {}

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDispatch || riid == DIID_DShellWindowsEvents) {
            *ppv = static_cast<IDispatch*>(this);
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
    IFACEMETHODIMP Invoke(DISPID id, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override {
        if (id == DISPID_WINDOWREGISTERED) PostThreadMessageW(thread_id_, kScanMessage, 0, 0);
        return S_OK;
    }

private:
    ~WindowEvents() = default;
    std::atomic<long> refs_{1};
    const DWORD thread_id_;
};

struct Candidate {
    HWND hwnd = nullptr;
    ULONGLONG registered_at = 0;
    ComPtr<IWebBrowser2> browser;
    DWORD process_id = 0;
    ExplorerTakeoverRequest submitted;
};

class Watcher {
public:
    explicit Watcher(ExplorerWindowTakeover::Shared& shared) : shared_(shared) {}
    ~Watcher() {
        for (auto& candidate : candidates_)
            if (candidate.submitted.handoff) candidate.submitted.handoff->Cancel();
        if (point_ && advise_cookie_) point_->Unadvise(advise_cookie_);
        if (timer_) KillTimer(nullptr, timer_);
    }
    Watcher(const Watcher&) = delete;
    Watcher& operator=(const Watcher&) = delete;

    bool Start() {
        HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows_));
        if (FAILED(hr)) {
            TraceShellWindows(L"takeover: CoCreateInstance(ShellWindows) hr=0x%08x", static_cast<unsigned>(hr));
            return false;
        }
        // Windows open now are the user's; only later ones are candidates.
        ForEachExplorerWindow([&](HWND hwnd, IWebBrowser2*) { known_.insert(hwnd); });
        ComPtr<IConnectionPointContainer> container;
        hr = windows_.As(&container);
        if (SUCCEEDED(hr)) hr = container->FindConnectionPoint(DIID_DShellWindowsEvents, &point_);
        if (SUCCEEDED(hr)) {
            sink_.Attach(new WindowEvents(GetCurrentThreadId()));
            hr = point_->Advise(sink_.Get(), &advise_cookie_);
        }
        TraceShellWindows(L"takeover: watching, %zu windows already open, advise hr=0x%08x", known_.size(),
                          static_cast<unsigned>(hr));
        return SUCCEEDED(hr);
    }

    void Scan() {
        for (auto it = known_.begin(); it != known_.end();) it = IsWindow(*it) ? std::next(it) : known_.erase(it);
        ForEachExplorerWindow([&](HWND hwnd, IWebBrowser2* browser) {
            if (!hwnd || !known_.insert(hwnd).second) return;
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == GetCurrentProcessId() || !IsExplorerProcess(pid)) return;
            // Shift held: File Explorer is wanted this time.
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
                TraceShellWindows(L"takeover: hwnd=%p left alone (Shift)", hwnd);
                return;
            }
            Candidate candidate;
            candidate.hwnd = hwnd;
            candidate.registered_at = GetTickCount64();
            candidate.browser = browser;
            candidate.process_id = pid;
            candidates_.push_back(std::move(candidate));
            TraceShellWindows(L"takeover: new Explorer window hwnd=%p", hwnd);
        });
        UpdateTimer();
    }

    void Poll() {
        for (auto it = candidates_.begin(); it != candidates_.end();) {
            if (!IsWindow(it->hwnd)) {
                if (it->submitted.handoff) it->submitted.handoff->Cancel();
                it = candidates_.erase(it);
                continue;
            }
            if (it->submitted.handoff) {
                const auto handoff = it->submitted.handoff;
                if (shared_.stopping || GetTickCount64() >= handoff->deadline_tick)
                    handoff->Cancel(HandoffState::Expired);
                const auto state = handoff->state.load();
                if (state == HandoffState::Pending) { ++it; continue; }
                bool close_committed = false;
                if (state == HandoffState::Ready && SourceUnchanged(*it)) {
                    std::lock_guard lock(shared_.handoffs_mutex);
                    close_committed = !shared_.stopping && handoff->ClaimClose(GetTickCount64());
                }
                // ClaimClose is the commitment point. Stop can cancel requests
                // before it, but cannot revoke an already committed COM call.
                if (close_committed) it->browser->Quit();
                else handoff->Cancel();
                it = candidates_.erase(it);
                continue;
            }
            ExplorerWindowProbe probe;
            probe.age_ms = static_cast<unsigned>(GetTickCount64() - it->registered_at);
            ExplorerTakeoverRequest request;
            Read(*it, probe, request);
            const ExplorerTakeoverStep step = DecideExplorerTakeover(probe);
            if (step == ExplorerTakeoverStep::Wait) {
                ++it;
                continue;
            }
            TraceShellWindows(L"takeover: hwnd=%p %s after %u ms folder=[%s] selected=%zu", it->hwnd,
                              step == ExplorerTakeoverStep::Take ? L"taken" : L"left", probe.age_ms,
                              request.folder.c_str(), request.names.size());
            if (step == ExplorerTakeoverStep::Take && Take(*it, std::move(request))) {
                ++it;
                continue;
            }
            it = candidates_.erase(it);
        }
        UpdateTimer();
    }

private:
    template <class Fn>
    void ForEachExplorerWindow(Fn&& fn) {
        long count = 0;
        if (!windows_ || FAILED(windows_->get_Count(&count))) return;
        for (long i = 0; i < count; ++i) {
            VARIANT index;
            VariantInit(&index);
            index.vt = VT_I4;
            index.lVal = i;
            ComPtr<IDispatch> item;
            if (windows_->Item(index, &item) != S_OK || !item) continue;
            // Pulse's own panes (shell_window_registry) are IWebBrowserApp only.
            ComPtr<IWebBrowser2> browser;
            if (FAILED(item.As(&browser))) continue;
            SHANDLE_PTR handle = 0;
            if (FAILED(browser->get_HWND(&handle)) || !handle) continue;
            fn(reinterpret_cast<HWND>(handle), browser.Get());
        }
    }

    static void Read(const Candidate& candidate, ExplorerWindowProbe& probe, ExplorerTakeoverRequest& request) {
        ComPtr<IServiceProvider> provider;
        ComPtr<IShellBrowser> shell_browser;
        ComPtr<IShellView> view;
        ComPtr<IFolderView2> folder_view;
        ComPtr<IPersistFolder2> folder;
        if (FAILED(candidate.browser.As(&provider)) ||
            FAILED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&shell_browser))) ||
            FAILED(shell_browser->QueryActiveShellView(&view)) || !view || FAILED(view.As(&folder_view)) ||
            FAILED(folder_view->GetFolder(IID_PPV_ARGS(&folder))))
            return;
        PIDLIST_ABSOLUTE raw = nullptr;
        if (FAILED(folder->GetCurFolder(&raw)) || !raw) return;
        UniquePidl pidl(raw);
        probe.view_ready = true;
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetNameFromIDList(pidl.get(), SIGDN_FILESYSPATH, &path)) && path) {
            // A real directory: not a .zip or other file shown as a folder.
            const DWORD attributes = GetFileAttributesW(path);
            probe.supported = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
            request.folder = path;
            CoTaskMemFree(path);
        } else if (IsThisPc(pidl.get())) {
            probe.supported = true;
        }
        if (!probe.supported) return;
        ComPtr<IShellItemArray> items;
        DWORD count = 0;
        if (folder_view->GetSelection(FALSE, &items) != S_OK || !items || FAILED(items->GetCount(&count))) return;
        for (DWORD i = 0; i < count; ++i) {
            ComPtr<IShellItem> item;
            PWSTR name = nullptr;
            if (FAILED(items->GetItemAt(i, &item)) ||
                FAILED(item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &name)) || !name)
                return;
            request.names.emplace_back(name);
            CoTaskMemFree(name);
        }
        request.selection_read = true;
        probe.selected = request.names.size();
    }

    bool SourceUnchanged(const Candidate& candidate) {
        // IShellWindows exposes active views, not a reliable complete tab
        // inventory on Windows 11. Never close a potentially tabbed window.
        using VersionFn = LONG (WINAPI*)(OSVERSIONINFOW*);
        const auto version_fn = reinterpret_cast<VersionFn>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        OSVERSIONINFOW version{};
        version.dwOSVersionInfoSize = sizeof(version);
        if (!version_fn || version_fn(&version) != 0 || version.dwBuildNumber >= 22000) return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(candidate.hwnd, &pid);
        SHANDLE_PTR handle = 0;
        if (pid != candidate.process_id || !IsExplorerProcess(pid) ||
            FAILED(candidate.browser->get_HWND(&handle)) ||
            reinterpret_cast<HWND>(handle) != candidate.hwnd) return false;
        size_t views = 0;
        ForEachExplorerWindow([&](HWND hwnd, IWebBrowser2*) {
            if (GetAncestor(hwnd, GA_ROOT) == GetAncestor(candidate.hwnd, GA_ROOT)) ++views;
        });
        if (views != 1) return false;
        ExplorerWindowProbe probe;
        ExplorerTakeoverRequest current;
        Read(candidate, probe, current);
        if (!probe.view_ready || !probe.supported || !current.selection_read ||
            current.folder != candidate.submitted.folder) return false;
        auto previous = candidate.submitted.names;
        std::sort(previous.begin(), previous.end());
        std::sort(current.names.begin(), current.names.end());
        return previous == current.names;
    }

    bool Take(Candidate& candidate, ExplorerTakeoverRequest request) {
        if (!request.selection_read || shared_.stopping) return false;
        request.handoff = std::make_shared<ExplorerHandoff>(GetTickCount64() + 10000);
        candidate.submitted = request;
        std::lock_guard lock(shared_.handoffs_mutex);
        if (shared_.stopping) { request.handoff->Cancel(); return false; }
        std::erase_if(shared_.handoffs, [](const auto& value) { return value.expired(); });
        shared_.handoffs.push_back(request.handoff);
        auto posted = std::make_unique<ExplorerTakeoverRequest>(std::move(request));
        if (!PostMessageW(shared_.window, shared_.message, 0, reinterpret_cast<LPARAM>(posted.get()))) {
            candidate.submitted.handoff->Cancel();
            return false;
        }
        posted.release();
        return true;
    }

    void UpdateTimer() {
        if (!candidates_.empty() && !timer_) timer_ = SetTimer(nullptr, 0, kPollMs, nullptr);
        else if (candidates_.empty() && timer_) {
            KillTimer(nullptr, timer_);
            timer_ = 0;
        }
    }

    ExplorerWindowTakeover::Shared& shared_;
    ComPtr<IShellWindows> windows_;
    ComPtr<IConnectionPoint> point_;
    ComPtr<WindowEvents> sink_;
    DWORD advise_cookie_ = 0;
    std::set<HWND> known_;
    std::vector<Candidate> candidates_;
    UINT_PTR timer_ = 0;
};

DWORD WINAPI TakeoverThread(void* param) {
    std::shared_ptr<ExplorerWindowTakeover::Shared> shared =
        std::move(*static_cast<std::shared_ptr<ExplorerWindowTakeover::Shared>*>(param));
    delete static_cast<std::shared_ptr<ExplorerWindowTakeover::Shared>*>(param);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 1;
    {
        MSG msg{};
        PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);   // queue before Stop can post
        shared->thread_id = GetCurrentThreadId();
        Watcher watcher(*shared);
        // Without the shell (Explorer not running) there is nothing to take.
        if (!shared->stopping) watcher.Start();
        while (!shared->stopping && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (!msg.hwnd && msg.message == kScanMessage) {
                watcher.Scan();
                continue;
            }
            if (!msg.hwnd && msg.message == WM_TIMER) {
                watcher.Poll();
                continue;
            }
            if (!msg.hwnd && msg.message == kStopMessage) continue;
            // COM delivers Explorer's events through this loop.
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    CoUninitialize();
    return 0;
}

} // namespace

ExplorerWindowTakeover::ExplorerWindowTakeover(HWND window, UINT message)
    : shared_(std::make_shared<Shared>()) {
    shared_->window = window;
    shared_->message = message;
    auto* param = new std::shared_ptr<Shared>(shared_);
    thread_ = CreateThread(nullptr, 0, &TakeoverThread, param, 0, nullptr);
    if (!thread_) delete param;
}

ExplorerWindowTakeover::~ExplorerWindowTakeover() { Stop(); }

void ExplorerWindowTakeover::Stop(DWORD timeout_ms) {
    if (!thread_) return;
    {
        std::lock_guard lock(shared_->handoffs_mutex);
        shared_->stopping = true;
        for (auto& weak : shared_->handoffs) if (auto handoff = weak.lock()) handoff->Cancel();
    }
    // Before the thread has a queue the post fails; it checks `stopping`
    // right after creating one.
    if (const DWORD thread_id = shared_->thread_id) PostThreadMessageW(thread_id, kStopMessage, 0, 0);
    WaitForSingleObject(thread_, timeout_ms);
    CloseHandle(thread_);
    thread_ = nullptr;
}

} // namespace pulse::app
