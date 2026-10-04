#include "../app/update_installer.h"
#include "../app/update_transport.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>

int wmain(int argc, wchar_t** argv) {
    using namespace pulse::app;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        if (!ok) ++failures;
    };
    const auto parameters = UpdateInstallParameters(L"C:\\Program Files\\Pulse\\pulse.exe");
    check(parameters == L"/SP- /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /PULSEUPDATE=1 /LOG /DIR=\"C:\\Program Files\\Pulse\"",
        "update uses silent restart mode and quotes the existing installation directory");
    check(UpdateInstallParameters(L"pulse.exe").empty() && UpdateInstallParameters(L"C:\\bad\"path\\pulse.exe").empty(),
        "invalid installer target cannot inject command switches");
    HWND window = CreateWindowExW(0, L"STATIC", L"Update test", 0, 0, 0, 0, 0, HWND_MESSAGE,
        nullptr, GetModuleHandleW(nullptr), nullptr);
    if (argc == 4 && std::wstring_view(argv[1]) == L"--download") {
        UpdateResult release;
        release.update_available = true;
        release.download_page = argv[2];
        release.installer_sha256 = argv[3];
        UpdateInstaller installer;
        check(installer.Start(release, window, WM_APP + 1), "live installer download starts");
        const auto deadline = GetTickCount64() + 180000;
        DWORD error = ERROR_TIMEOUT;
        unsigned samples = 0;
        uint64_t largest_sample = 0;
        bool bounds_ok = true;
        while (GetTickCount64() < deadline && !installer.TakeResult(error)) {
            const auto progress = installer.Progress();
            largest_sample = (std::max)(largest_sample, progress.received_bytes);
            bounds_ok &= progress.percent() >= -1 && progress.percent() <= 100;
            ++samples;
            Sleep(20);
        }
        const auto final_progress = installer.Progress();
        check(bounds_ok && samples > 0, "live progress snapshots stay bounded");
        check(final_progress.phase == UpdatePhase::Ready && final_progress.received_bytes > 0,
            "verified package retains actual consumed byte count at ready stage");
        check(final_progress.percent() == -1, "ready-to-install is a stage, not installation percentage");
        std::cout << "progress_samples=" << samples << " largest_sample=" << largest_sample
            << " received=" << final_progress.received_bytes << " total=" << final_progress.total_bytes << '\n';
        check(error == ERROR_SUCCESS, "live HTTPS installer downloaded and hash verified without execution");
        std::cout << "diagnostic=" << error << '\n';
        installer.Stop();
        check(!installer.Progress().active() && installer.Progress().received_bytes == 0,
            "stop clears the progress snapshot without installer execution");
        // Exercise the actual HTTPS reader with a failing consumer: rejected bytes must not count.
        std::atomic<bool> stop{false};
        UpdateError category = UpdateError::None;
        DWORD failure = 0;
        uint64_t notified_bytes = 0;
        bool rejected_chunk = false;
        const bool result = ReadUpdateWithFallback(release.download_page, 512ull * 1024 * 1024, stop,
            [&] { notified_bytes = 0; return true; },
            [&](const void*, DWORD) { rejected_chunk = true; SetLastError(ERROR_WRITE_FAULT); return false; },
            category, failure,
            [&](std::wstring_view url, uint64_t limit, const std::atomic<bool>& cancelled,
                const std::function<bool(const void*, DWORD)>& consume, UpdateError& kind, DWORD& error) {
                return ReadUpdateResponseWithProgress(url, limit, cancelled, consume, kind, error,
                    [&](uint64_t bytes, uint64_t) { notified_bytes = bytes; });
            });
        check(!result && rejected_chunk && category == UpdateError::LocalIo && notified_bytes == 0,
            "actual HTTPS reader never reports rejected/unwritten chunk bytes");
        DestroyWindow(window);
        return failures ? 1 : 0;
    }
    namespace fs = std::filesystem;
    const auto root = fs::absolute(fs::path(L"bench_data") / (L"update-test-" + std::to_wstring(GetCurrentProcessId())));
    fs::create_directories(root);
    const auto file = root / L"安装包.exe";
    std::ofstream(file, std::ios::binary) << "abc";
    constexpr wchar_t hash[] = L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    HANDLE guard = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(VerifyUpdateInstaller(guard, hash), "signed SHA256 matches exact installer bytes");
    check(!VerifyUpdateInstaller(guard, std::wstring(64, L'0')), "changed installer hash rejected");
    check(!VerifyUpdateInstaller(guard, L"short"), "malformed installer hash rejected");
    HANDLE writer = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(writer == INVALID_HANDLE_VALUE, "verified installer is locked against replacement before launch");
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    CloseHandle(guard);
    std::ofstream(file, std::ios::binary | std::ios::trunc) << "changed";
    guard = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(!VerifyUpdateInstaller(guard, hash), "tampered installer bytes rejected");
    CloseHandle(guard);
    UpdateError category = UpdateError::None;
    DWORD error = 0;
    std::atomic<bool> cancelled{false};
    auto consume = [](const void*, DWORD) { return true; };
    check(!ReadUpdateResponse(L"http://example.test/update.exe", 100, cancelled, consume, category, error) &&
        category == UpdateError::InsecureUrl, "HTTP installer URL rejected before network access");
    check(!ReadUpdateResponse(L"https://user:password@example.test/update.exe", 100, cancelled, consume, category, error) &&
        category == UpdateError::InsecureUrl, "credential-bearing URL rejected");
    cancelled = true;
    check(!ReadUpdateResponse(L"https://example.test/update.exe", 100, cancelled, consume, category, error) &&
        error == ERROR_CANCELLED, "cancelled request does not access the network");
    check(ParseUpdateContentLength(L"8388608") == 8388608, "response length parses exact byte count");
    check(ParseUpdateContentLength(L"4294967296") == 4294967296ull, "response length is not limited to DWORD");
    for (const auto header : {L"", L"0", L"-1", L"123x", L"1,2", L"18446744073709551616"})
        check(ParseUpdateContentLength(header) == 0, "missing, zero, malformed or overflowing length is unknown");
    check(ParseUpdateContentLength(L"18446744073709551615") == (std::numeric_limits<uint64_t>::max)(),
        "maximum uint64 length parses without wrapping");
    UpdateProgress progress{UpdatePhase::Downloading, 3, 8};
    check(progress.percent() == 37, "download percentage derives only from consumed bytes");
    bool exact_boundaries = true;
    for (uint64_t value = 0; value <= 100; ++value)
        exact_boundaries &= UpdateProgress{UpdatePhase::Downloading, value, 100}.percent() == value;
    check(exact_boundaries, "all integer percentage boundaries are exact including 29 and 58 percent");
    const auto maximum_count = (std::numeric_limits<uint64_t>::max)();
    check(UpdateProgress{UpdatePhase::Downloading, maximum_count / 2, maximum_count}.percent() == 49 &&
          UpdateProgress{UpdatePhase::Downloading, maximum_count, maximum_count}.percent() == 100,
        "percentage calculation neither overflows nor rounds up enormous counters");
    progress.total_bytes = 0;
    check(progress.percent() == -1, "unknown response length stays indeterminate");
    progress = {UpdatePhase::Downloading, 9, 8};
    check(progress.percent() == -1, "contradictory total never fabricates a percentage");
    for (const auto phase : {UpdatePhase::Connecting, UpdatePhase::Verifying, UpdatePhase::Ready, UpdatePhase::WaitingOperations,
                            UpdatePhase::Launching, UpdatePhase::Installing}) {
        progress = {phase, 8, 8};
        check(progress.active() && progress.percent() == -1, "non-download stages never imply installation percentage");
    }
    // Exercise the actual fallback driver: new source must see zero bytes and unknown total.
    cancelled = false;
    unsigned attempts = 0, resets = 0;
    bool clean_attempt = true;
    std::string destination;
    const auto reset = [&] { ++resets; destination.clear(); progress = {UpdatePhase::Connecting}; return true; };
    const auto write = [&](const void* bytes, DWORD size) { destination.append(static_cast<const char*>(bytes), size); return true; };
    const auto reader = [&](std::wstring_view, uint64_t, const std::atomic<bool>&,
                            const std::function<bool(const void*, DWORD)>& sink, UpdateError& kind, DWORD& error) {
        ++attempts;
        clean_attempt &= progress.phase == UpdatePhase::Connecting && !progress.received_bytes &&
            !progress.total_bytes && destination.empty();
        sink("abc", 3);
        progress = {UpdatePhase::Downloading, 3, attempts == 1 ? 8ull : 0ull};
        kind = attempts == 1 ? UpdateError::Network : UpdateError::None;
        error = attempts == 1 ? ERROR_CONNECTION_ABORTED : ERROR_SUCCESS;
        return attempts != 1;
    };
    check(ReadUpdateWithFallback(L"https://github.com/jimmgreen/pulse/releases/download/v1/test.exe", 100,
        cancelled, reset, write, category, error, reader) && clean_attempt && attempts == 2 && resets == 2 &&
        destination == "abc" && progress.received_bytes == 3 && progress.percent() == -1,
        "fallback resets partial bytes and previous source denominator before next attempt");
    cancelled = true;
    const auto before_attempts = attempts;
    check(!ReadUpdateWithFallback(L"https://github.com/jimmgreen/pulse/releases/download/v1/test.exe", 100,
        cancelled, reset, write, category, error, reader) && attempts == before_attempts && error == ERROR_CANCELLED,
        "cancellation prevents fallback and cannot revive prior progress");
    UpdateInstaller installer;
    check(!installer.Progress().active(), "new installer has no active status-bar progress");
    UpdateResult invalid;
    check(UpdateInstallErrorFromExitCode(0) == ERROR_SUCCESS, "completed setup succeeds");
    check(UpdateInstallErrorFromExitCode(2) == ERROR_CANCELLED &&
        UpdateInstallErrorFromExitCode(5) == ERROR_CANCELLED, "setup cancellation is reported");
    for (DWORD code : {1ul, 3ul, 4ul, 6ul, 7ul, 8ul, 999ul}) {
        check(UpdateInstallErrorFromExitCode(code) == ERROR_INSTALL_FAILURE,
            "failed or unknown setup exit is not mistaken for success");
    }
    check(!installer.TakeInstallResult(error), "no completion before installer launch");
    check(!installer.Start(invalid, window, WM_APP + 1), "unverified update cannot start installer download");
    const auto before = GetTickCount64();
    installer.Stop();
    check(GetTickCount64() - before < 100, "shutdown does not wait for network activity");
    check(!installer.Progress().active() && !installer.Progress().received_bytes, "idle stop clears all progress fields");
    check(!installer.Launch(window, error), "installer cannot launch before successful verification");
    fs::remove_all(root);
    DestroyWindow(window);
    return failures ? 1 : 0;
}
