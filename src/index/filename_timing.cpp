#include "filename_timing.h"
#include "index_paths.h"
#include "pulse_version.h"
#include <mutex>

namespace pulse::index {
namespace {
uint64_t FileTimeValue(FILETIME time) noexcept {
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}
constexpr uint64_t kRotateBytes = 1024 * 1024;
constexpr const char* kNames[]{"wait", "topology", "journal", "notify", "delta_flush", "merge", "rebuild", "recovery", "name_pool_compact"};
std::string Utf8(const wchar_t* value) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}
void Write(const std::string& line) {
    const auto directory = DataDir();
    if (directory.empty()) return;
    const auto path = directory + L"\\pulse-index-timing.jsonl";
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) { CloseHandle(file); return; }
    if (static_cast<uint64_t>(size.QuadPart) + line.size() > kRotateBytes) {
        CloseHandle(file);
        if (!MoveFileExW(path.c_str(), (path + L".1").c_str(), MOVEFILE_REPLACE_EXISTING)) return;
        file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
    }
    LARGE_INTEGER end{}; SetFilePointerEx(file, end, nullptr, FILE_END);
    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(file);
}
}
FilenameTiming::Token FilenameTiming::Begin() noexcept {
    if (!IndexDiagnosticsEnabled() && !diagnostics::runtime::Enabled()) return {};
    FILETIME created{}, exited{}, kernel{}, user{};
    GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
    LARGE_INTEGER now{}, frequency{};
    QueryPerformanceCounter(&now); QueryPerformanceFrequency(&frequency);
    return {frequency.QuadPart ? static_cast<uint64_t>((now.QuadPart / frequency.QuadPart) * 1000000 +
        (now.QuadPart % frequency.QuadPart) * 1000000 / frequency.QuadPart) : 0,
        (FileTimeValue(kernel) + FileTimeValue(user)) / 10};
}
void FilenameTiming::End(FilenameStage stage, Token token, uint64_t changes, DWORD error, const char* reason, wchar_t volume) noexcept {
    if (!IndexDiagnosticsEnabled() && !diagnostics::runtime::Enabled()) return;
    const auto now = Begin();
    auto& counter = counters_[static_cast<size_t>(stage)];
    ++counter.calls;
    counter.wall_us += now.wall >= token.wall ? now.wall - token.wall : 0;
    counter.cpu_us += now.cpu >= token.cpu ? now.cpu - token.cpu : 0;
    counter.changes += changes;
    if (error) { ++counter.errors; counter.last_error = error; }
    counter.reason = reason;
    counter.volume = volume;
}
void FilenameTiming::Flush(bool force) noexcept {
    if (!IndexDiagnosticsEnabled() && !diagnostics::runtime::Enabled()) return;
    const auto now = GetTickCount64();
    if (!force && last_flush_ && now - last_flush_ < 60000) return;
    last_flush_ = now;
    for (size_t i = 0; i < counters_.size(); ++i) {
        const auto& c = counters_[i];
        if (!c.calls) continue;
        diagnostics::runtime::Event("index_stage_totals", {{"stage", i}, {"calls", c.calls},
            {"wall_us", c.wall_us}, {"cpu_us", c.cpu_us}, {"changes", c.changes},
            {"errors", c.errors}, {"last_error", c.last_error}});
    }
    if (!IndexDiagnosticsEnabled()) return;
    memory_.Capture(IndexMemoryPoint::TimingFlush);
    try {
        FILETIME time{}; GetSystemTimeAsFileTime(&time);
        std::string line = "{\"event\":\"filename_timing\",\"pid\":" + std::to_string(GetCurrentProcessId()) +
            ",\"role\":\"" + (MachineIndexScope() ? "filename_service" : "filename_user") +
            "\",\"version\":\"" PULSE_VERSION_STRING_A "\",\"build\":\"" + Utf8(PULSE_BUILD_ID) +
            "\",\"timestamp_filetime\":" + std::to_string(FileTimeValue(time)) + ",\"stages\":{";
        for (size_t i = 0; i < counters_.size(); ++i) {
            const auto& c = counters_[i];
            if (i) line += ',';
            line += "\"" + std::string(kNames[i]) + "\":{\"calls\":" + std::to_string(c.calls) +
                ",\"wall_us\":" + std::to_string(c.wall_us) + ",\"cpu_us\":" + std::to_string(c.cpu_us) +
                ",\"changes\":" + std::to_string(c.changes) + ",\"errors\":" + std::to_string(c.errors) +
                ",\"last_error\":" + std::to_string(c.last_error) + ",\"reason\":\"" + c.reason +
                "\",\"volume_letter\":\"" + (c.volume >= L'A' && c.volume <= L'Z' ? std::string(1, static_cast<char>(c.volume)) : "") + "\"}";
        }
        line += "},\"memory\":" + memory_.Json() + ",\"maintenance\":" + maintenance_.Json() + "}\n";
        Write(line);
    } catch (...) {}
}
}
