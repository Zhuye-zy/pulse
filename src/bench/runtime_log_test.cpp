#include "../common/runtime_log.h"
#include "../common/diagnostics_exporter.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

using namespace pulse::diagnostics::runtime;
namespace {
std::string Read(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
}
int main() {
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"runtime-log-" + std::to_wstring(GetCurrentProcessId())));
    std::filesystem::create_directories(root);
    bool ok = true;
    auto check = [&](bool value, const char* label) { ok &= value; std::cout << (value ? "[PASS] " : "[FAIL] ") << label << '\n'; };
    Event("not_initialized");
    check(!Enabled(), "uninitialized logging is disabled");
    check(!Initialize(root.wstring(), "C:\\private\\secret.txt"), "reject path as component identifier");
    check(Initialize(root.wstring(), "test", {2 * 1024 * 1024, 40}), "initialize isolated runtime logging");
    const auto id = NextId();
    SetLastError(ERROR_ACCESS_DENIED);
    Event("operation_begin", {{"operation", id}, {"error", 5}});
    check(GetLastError() == ERROR_ACCESS_DENIED, "recording preserves caller Win32 error state");
    Event("C:\\private\\secret.txt");
    Event("bad_field", {{"query phrase", 123}});
    const auto started = GetTickCount64();
    std::vector<std::thread> producers;
    for (unsigned thread = 0; thread < 4; ++thread) producers.emplace_back([thread] {
        for (unsigned i = 0; i < 100; ++i) Event("parallel_event", {{"producer", thread}, {"index", i}});
    });
    for (auto& producer : producers) producer.join();
    std::cout << "[TIME] enqueue_400_ms=" << GetTickCount64() - started << '\n';
    Sleep(100);
    Event("operation_end", {{"operation", id}});
    Shutdown();
    const auto file = root / L"Diagnostics" / L"Runtime" / (L"test-" + std::to_wstring(GetCurrentProcessId()) + L".jsonl");
    const auto text = Read(file);
    check(!Enabled(), "shutdown disables producers and drains queued records");
    check(text.find("process_start") != std::string::npos && text.find("process_stop") != std::string::npos,
        "normal lifecycle has start and stop records");
    check(text.find("operation_begin") != std::string::npos && text.find("operation_end") != std::string::npos,
        "operation correlation survives asynchronous delivery");
    size_t count = 0, at = 0;
    while ((at = text.find("\"event\":\"parallel_event\"", at)) != std::string::npos) { ++count; ++at; }
    check(count == 400, "concurrent producers preserve all records within queue capacity");
    check(text.find("process_health") != std::string::npos && text.find("private_bytes") != std::string::npos &&
        text.find("logical_processors") != std::string::npos, "health includes CPU normalization and memory evidence");
    check(text.find("secret.txt") == std::string::npos && text.find("query phrase") == std::string::npos &&
        text.find("\"dropped_events\":2") != std::string::npos, "invalid identifiers are omitted and counted");
    check(text.find("\"utc\":") != std::string::npos && text.find("\"version\":") != std::string::npos &&
        text.find("\"session\":") != std::string::npos, "records identify time, build and process session");
    const auto exported = root / L"export";
    std::filesystem::create_directory(exported);
    pulse::diagnostics::ExportOptions export_options;
    export_options.source_root = root.wstring(); export_options.destination = exported.wstring();
    export_options.include_dumps = false;
    check(pulse::diagnostics::Export(export_options) && Read(exported / L"Runtime" / file.filename()) == text,
        "real runtime records survive the existing diagnostics export pipeline");
    const auto rotated = root / L"rotation";
    std::filesystem::create_directory(rotated);
    check(Initialize(rotated.wstring(), "test", {4096, 60000}), "restart logger with isolated rotation threshold");
    for (unsigned i = 0; i < 100; ++i) Event("rotation_event", {{"index", i}});
    Shutdown();
    const auto rotated_file = rotated / L"Diagnostics" / L"Runtime" / file.filename();
    const auto previous = std::filesystem::path(rotated_file.wstring() + L".1");
    check(std::filesystem::exists(previous) && std::filesystem::file_size(previous) <= 4096 &&
        std::filesystem::file_size(rotated_file) <= 4096, "rotation bounds current and previous log files");
    check(Read(rotated_file).find("process_stop") != std::string::npos, "rotation retains latest shutdown evidence");
    // Keep only isolated artifacts for an independent JSON parser check.
    std::wcout << L"[INFO] fixture=" << root.wstring() << L'\n';
    return ok ? 0 : 1;
}
