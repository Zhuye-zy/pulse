#include "../index/network_agent_client.h"
#include <cstdio>
#include <future>

namespace pulse::index {
struct NetworkAgentClientTestAccess {
    static void Start(NetworkAgentClient& client, const std::wstring& pipe, std::promise<void>& done) {
        client.pipe_name_ = pipe;
        client.running_ = true;
        // This live process handle bypasses helper launching without touching the real singleton.
        client.agent_process_ = OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId());
        client.session_requests_[7] = 77;
        client.search_thread_ = std::thread([&client, &done] {
            Query query; query.session_id = 7; query.needle = L"fixture";
            client.SearchRequest(query, 77);
            done.set_value();
        });
    }
    static bool Released(NetworkAgentClient& client) {
        std::lock_guard lock(client.pipe_mu_);
        return client.active_pipe_ == INVALID_HANDLE_VALUE;
    }
};
}
int main() {
    using namespace pulse;
    const auto name = LR"(\\.\pipe\PulseNetworkFixture-)" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64());
    HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (pipe == INVALID_HANDLE_VALUE || !release) return 2;
    std::thread server([&] {
        if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            ipc::MsgHeader header{};
            if (ipc::PipeRead(pipe, reinterpret_cast<uint8_t*>(&header), sizeof(header))) {
                std::vector<uint8_t> payload(header.payload_size);
                if (!payload.empty()) ipc::PipeRead(pipe, payload.data(), static_cast<DWORD>(payload.size()));
                WaitForSingleObject(release, 15000);
            }
        }
        DisconnectNamedPipe(pipe);
    });
    index::NetworkAgentClient client;
    std::promise<void> done; auto completed = done.get_future();
    const auto start = GetTickCount64();
    index::NetworkAgentClientTestAccess::Start(client, name, done);
    const bool timely = completed.wait_for(std::chrono::seconds(12)) == std::future_status::ready;
    const auto elapsed = GetTickCount64() - start;
    index::SearchResult result;
    const bool terminal = timely && client.TakeResult(77, result) && result.error != ERROR_SUCCESS;
    const bool released = timely && index::NetworkAgentClientTestAccess::Released(client);
    SetEvent(release);
    const auto stop = GetTickCount64(); client.Stop();
    const bool joined = GetTickCount64() - stop < 2000;
    server.join(); CloseHandle(pipe); CloseHandle(release);
    printf("[%s] silent isolated pipe produces a terminal search failure after deadline (%llu ms)\n",
        timely && terminal && elapsed >= 4500 ? "PASS" : "FAIL", elapsed);
    printf("[%s] timed-out request releases pipe and joins client thread\n", released && joined ? "PASS" : "FAIL");
    return timely && terminal && elapsed >= 4500 && released && joined ? 0 : 1;
}
