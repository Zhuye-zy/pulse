#include "../ui/preview_handler_host.h"
#include <shobjidl.h>
#include <atomic>
#include <cstdio>

namespace pulse::ui {
extern IUnknown* (*g_preview_handler_factory_for_test)();
}
namespace {
HANDLE entered = nullptr, release_preview = nullptr, finished = nullptr;
std::atomic<HWND> preview_parent{nullptr};
class SlowHandler final : public IPreviewHandler, public IInitializeWithFile {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IPreviewHandler) *out = static_cast<IPreviewHandler*>(this);
        else if (iid == IID_IInitializeWithFile) *out = static_cast<IInitializeWithFile*>(this);
        if (!*out) return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n = --refs_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE Initialize(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetWindow(HWND hwnd, const RECT*) override { preview_parent = hwnd; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetRect(const RECT*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DoPreview() override {
        SetEvent(entered);
        WaitForSingleObject(release_preview, 5000);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Unload() override { SetEvent(finished); return S_OK; }
    HRESULT STDMETHODCALLTYPE SetFocus() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE QueryFocus(HWND* hwnd) override { *hwnd = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE TranslateAccelerator(MSG*) override { return S_FALSE; }
private:
    std::atomic<ULONG> refs_{1};
};
IUnknown* CreateSlowHandler() { return static_cast<IPreviewHandler*>(new SlowHandler); }
bool PumpUntil(HANDLE event) {
    const auto deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline) {
        if (WaitForSingleObject(event, 0) == WAIT_OBJECT_0) return true;
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        Sleep(5);
    }
    return false;
}
}
int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    release_preview = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    pulse::ui::g_preview_handler_factory_for_test = CreateSlowHandler;
    HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"preview regression", WS_OVERLAPPED | WS_VISIBLE,
        -32000, -32000, 300, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    bool ok = true;
    auto check = [&](bool value, const char* label) {
        std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label); ok &= value;
    };
    {
        pulse::ui::PreviewHandlerHost host;
        D2D1_RECT_F bounds{0, 0, 200, 200};
        host.Sync(owner, bounds, L"isolated.docx", FILE_ATTRIBUTE_NORMAL, 1, 1, 1,
            true, D2D1::ColorF(D2D1::ColorF::White), D2D1::ColorF(D2D1::ColorF::Black), true, true);
        check(PumpUntil(entered), "slow COM handler entered DoPreview");
        HWND overlay = preview_parent.load();
        check(overlay && !(GetWindowLongPtrW(overlay, GWL_STYLE) & WS_VISIBLE),
              "provider initializes inside a hidden overlay");
        host.Hide();
        SetEvent(release_preview);
        check(PumpUntil(finished), "cancelled provider is unloaded after returning");
        check(!(GetWindowLongPtrW(overlay, GWL_STYLE) & WS_VISIBLE),
              "late COM completion cannot show cancelled preview");
        ResetEvent(entered);
        ResetEvent(release_preview);
        host.Sync(owner, bounds, L"next.docx", FILE_ATTRIBUTE_NORMAL, 2, 1, 1,
            true, D2D1::ColorF(D2D1::ColorF::White), D2D1::ColorF(D2D1::ColorF::Black), true, true);
        check(PumpUntil(entered), "next preview initializes after cancellation");
        SetEvent(release_preview);
        const auto deadline = GetTickCount64() + 5000;
        while (host.state() != pulse::ui::PreviewHandlerHost::State::Shown && GetTickCount64() < deadline) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg); DispatchMessageW(&msg);
            }
            Sleep(5);
        }
        check(host.state() == pulse::ui::PreviewHandlerHost::State::Shown &&
            (GetWindowLongPtrW(preview_parent.load(), GWL_STYLE) & WS_VISIBLE),
            "current successful preview becomes visible");
    }
    DestroyWindow(owner);
    CloseHandle(entered); CloseHandle(release_preview); CloseHandle(finished);
    CoUninitialize();
    return ok ? 0 : 1;
}
