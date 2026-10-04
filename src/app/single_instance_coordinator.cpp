#include "single_instance_coordinator.h"

#include <limits>
#include <algorithm>
#include <cstring>
#include <objbase.h>

namespace pulse::app {
namespace {

constexpr wchar_t kMutexName[] = L"Local\\Pulse.Singleton";
constexpr wchar_t kWindowClass[] = L"PulseMainWindow";
constexpr ULONG_PTR kOpenPathMessage = 0x50554C53; // 'PULS'
constexpr ULONG_PTR kOpenRequestMessage = 0x50554C32; // 'PUL2'
constexpr size_t kMaxForwardedPathChars = 32768;
struct OpenRequestHeader {
    uint32_t version = 2;
    uint32_t chars = 0;
    uint64_t deadline = 0;
    std::array<unsigned char, 16> id{};
};
static_assert(sizeof(OpenRequestHeader) == 32);

} // namespace

SingleInstanceCoordinator::~SingleInstanceCoordinator() {
    Release();
}

SingleInstanceCoordinator::AcquireResult SingleInstanceCoordinator::Acquire(
    std::wstring_view mutex_name) {
    if (mutex_) return AcquireResult::Primary;
    const std::wstring name = mutex_name.empty() ? kMutexName : std::wstring(mutex_name);
    SetLastError(ERROR_SUCCESS);
    HANDLE mutex = CreateMutexW(nullptr, TRUE, name.c_str());
    if (!mutex) return AcquireResult::Failed;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return AcquireResult::Existing;
    }
    mutex_ = mutex;
    return AcquireResult::Primary;
}

void SingleInstanceCoordinator::Release() {
    if (!mutex_) return;
    ReleaseMutex(mutex_);
    CloseHandle(mutex_);
    mutex_ = nullptr;
}

bool SingleInstanceCoordinator::ForwardOpenPath(const std::wstring& path,
                                                DWORD timeout_ms) const {
    OpenRequest request;
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) return false;
    std::memcpy(request.id.data(), &id, sizeof(id));
    request.deadline = GetTickCount64() + timeout_ms;
    request.path = path;
    auto payload = EncodeOpenRequest(request);
    if (payload.empty()) return false;
    COPYDATASTRUCT data{};
    data.dwData = kOpenRequestMessage;
    data.cbData = static_cast<DWORD>(payload.size());
    data.lpData = payload.data();
    // All attempts use the same ID and deadline. A timeout may mean that the
    // first message was accepted, so never retry with a fresh ID.
    while (GetTickCount64() < request.deadline) {
        if (HWND hwnd = FindWindowW(kWindowClass, nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid) AllowSetForegroundWindow(pid);
            const auto now = GetTickCount64();
            if (now >= request.deadline) break;
            const UINT remaining = static_cast<UINT>((std::min)(uint64_t{500}, request.deadline - now));
            DWORD_PTR result = 0;
            if (SendMessageTimeoutW(hwnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
                                    SMTO_ABORTIFHUNG | SMTO_BLOCK, remaining, &result))
                return result == TRUE;
        }
        Sleep(25);
    }
    return false;
}

std::vector<unsigned char> SingleInstanceCoordinator::EncodeOpenRequest(const OpenRequest& request) {
    if (request.path.size() >= kMaxForwardedPathChars || request.path.find(L'\0') != std::wstring::npos)
        return {};
    OpenRequestHeader header;
    header.chars = static_cast<uint32_t>(request.path.size() + 1);
    header.deadline = request.deadline;
    header.id = request.id;
    std::vector<unsigned char> payload(sizeof(header) + header.chars * sizeof(wchar_t));
    std::memcpy(payload.data(), &header, sizeof(header));
    std::memcpy(payload.data() + sizeof(header), request.path.c_str(), header.chars * sizeof(wchar_t));
    return payload;
}

bool SingleInstanceCoordinator::DecodeOpenRequest(const COPYDATASTRUCT* data, OpenRequest& request) {
    if (!data || data->dwData != kOpenRequestMessage || !data->lpData ||
        data->cbData < sizeof(OpenRequestHeader)) return false;
    OpenRequestHeader header;
    std::memcpy(&header, data->lpData, sizeof(header));
    if (header.version != 2 || !header.chars || header.chars > kMaxForwardedPathChars ||
        data->cbData != sizeof(header) + header.chars * sizeof(wchar_t) ||
        std::all_of(header.id.begin(), header.id.end(), [](unsigned char c) { return c == 0; })) return false;
    std::wstring path(header.chars, L'\0');
    std::memcpy(path.data(), static_cast<const unsigned char*>(data->lpData) + sizeof(header),
                header.chars * sizeof(wchar_t));
    if (path.back() != L'\0') return false;
    path.pop_back();
    if (path.find(L'\0') != std::wstring::npos) return false;
    request = {header.id, header.deadline, std::move(path)};
    return true;
}

SingleInstanceCoordinator::OpenAcceptance SingleInstanceCoordinator::AcceptOpenRequest(
    const OpenRequest& request, uint64_t now) {
    if (now >= request.deadline || request.deadline - now > 60000) return OpenAcceptance::Invalid;
    std::erase_if(accepted_, [now](const auto& item) { return item.second.deadline <= now; });
    if (const auto found = accepted_.find(request.id); found != accepted_.end())
        return found->second.path == request.path && found->second.deadline == request.deadline
            ? OpenAcceptance::Duplicate : OpenAcceptance::Invalid;
    // Do not evict a live ID: its delayed retry would open a duplicate tab.
    if (accepted_.size() >= 256) return OpenAcceptance::Invalid;
    accepted_.emplace(request.id, request);
    return OpenAcceptance::New;
}

ULONG_PTR SingleInstanceCoordinator::OpenRequestMessageId() noexcept { return kOpenRequestMessage; }

bool SingleInstanceCoordinator::DecodeOpenPath(const COPYDATASTRUCT* data,
                                               std::wstring& path) {
    path.clear();
    if (!data || data->dwData != kOpenPathMessage || !data->lpData ||
        data->cbData < sizeof(wchar_t) || data->cbData % sizeof(wchar_t) != 0) {
        return false;
    }
    const size_t chars = data->cbData / sizeof(wchar_t);
    if (chars > kMaxForwardedPathChars) return false;
    const auto* text = static_cast<const wchar_t*>(data->lpData);
    if (text[chars - 1] != L'\0') return false;
    path.assign(text, chars - 1);
    return path.find(L'\0') == std::wstring::npos;
}

const wchar_t* SingleInstanceCoordinator::WindowClassName() noexcept {
    return kWindowClass;
}

ULONG_PTR SingleInstanceCoordinator::OpenPathMessageId() noexcept {
    return kOpenPathMessage;
}

} // namespace pulse::app
