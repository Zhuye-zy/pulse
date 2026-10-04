#include "preview_host_client.h"
#include "../preview_host/doc_payload.h"
#include "../preview_host/archive_listing.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
void Write(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
std::string Zip(const std::vector<std::string>& names) {
    std::string data, central;
    const auto put = [](std::string& out, size_t at, uint32_t value, size_t size) {
        for (size_t i = 0; i < size; ++i) out[at + i] = static_cast<char>(value >> (8 * i));
    };
    for (const auto& name : names) {
        std::string local(30, '\0');
        put(local, 0, 0x04034B50, 4);
        put(local, 4, 20, 2);
        put(local, 26, static_cast<uint32_t>(name.size()), 2);
        std::string header(46, '\0');
        put(header, 0, 0x02014B50, 4);
        put(header, 6, 20, 2);
        put(header, 28, static_cast<uint32_t>(name.size()), 2);
        put(header, 42, static_cast<uint32_t>(data.size()), 4);
        data += local + name;
        central += header + name;
    }
    std::string end(22, '\0');
    put(end, 0, 0x06054B50, 4);
    put(end, 8, static_cast<uint32_t>(names.size()), 2);
    put(end, 10, static_cast<uint32_t>(names.size()), 2);
    put(end, 12, static_cast<uint32_t>(central.size()), 4);
    put(end, 16, static_cast<uint32_t>(data.size()), 4);
    return data + central + end;
}
}

int wmain(int argc, wchar_t** argv) {
    namespace fs = std::filesystem;
    namespace ipc = pulse::ipc;
    const fs::path dir = fs::current_path() / L"bench_data" /
        (L"preview-regression-" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(dir);
    pulse_test::Host host;
    if (!host.Start()) { Check(false, "start preview host"); return 1; }
    const std::string title = "# \xE4\xB8\xAD\xE6\x96\x87\n";
    for (const bool bom : {false, true}) {
        for (int cut = 1; cut <= 3; ++cut) {
            std::string bytes = (bom ? "\xEF\xBB\xBF" : "") + title;
            bytes.resize(32768 - cut, 'a');
            bytes += "\xF0\x9F\x98\x80\nend";
            const fs::path path = dir / (std::to_wstring(cut) + (bom ? L"-bom.md" : L".md"));
            Write(path, bytes);
            pulse_test::Result r;
            const bool got = host.Request(path.wstring(), r, MAXDWORD, 1024,
                ipc::PreviewRequestKind::Content, ipc::kPreviewRequestFlagRichText);
            Check(got && r.response.kind == ipc::PreviewContentKind::Markdown &&
                r.text.find(L"中文") != std::wstring::npos &&
                ((r.response.flags & ipc::kPreviewFlagEncodingMask) >> ipc::kPreviewFlagEncodingShift) == static_cast<uint32_t>(bom ?
                    ipc::PreviewTextEncoding::Utf8Bom : ipc::PreviewTextEncoding::Utf8),
                "UTF-8 prefix split keeps Chinese and encoding");
        }
    }
    if (argc >= 2) {
        pulse_test::Result r;
        Check(host.Request(argv[1], r, MAXDWORD, 1024, ipc::PreviewRequestKind::Content,
                ipc::kPreviewRequestFlagRichText) && r.response.kind == ipc::PreviewContentKind::Markdown &&
            r.text.find(L"源码审查与修复任务清单") != std::wstring::npos &&
            ((r.response.flags & ipc::kPreviewFlagEncodingMask) >> ipc::kPreviewFlagEncodingShift) == static_cast<uint32_t>(ipc::PreviewTextEncoding::Utf8),
            "reported document previews with original Chinese title");
    }
    for (const size_t depth : {size_t{64}, size_t{65}, size_t{20000}}) {
        std::string name;
        for (size_t i = 1; i < depth; ++i) name += "x/";
        name += "leaf.txt";
        const fs::path path = dir / (std::to_wstring(depth) + L".zip");
        Write(path, Zip({name, "normal.txt"}));
        std::wstring payload, error;
        uint32_t read = 0;
        Check(pulse::preview::MakeArchiveListing(path.wstring(), payload, read, &error) &&
            payload.find(L"normal.txt") != std::wstring::npos &&
            (payload.find(L"leaf.txt") != std::wstring::npos) == (depth <= 64) &&
            payload.substr(0, payload.find(L'\n')).ends_with(depth > 64 ? L"\t1" : L"\t0"),
            "archive depth budget preserves normal entries and reports omission");
    }
    for (size_t size : {size_t{199999}, size_t{200000}, size_t{200001}}) {
        pulse::preview::DocPayload payload(600000);
        payload.Begin(L'p');
        payload.Text(std::wstring(size, L'中'), pulse::preview::kDocBold);
        Check(payload.text().size() <= 200000, "single run respects block limit");
        payload.End();
        Check(payload.full() == (size > 200000), "block limit reports truncation exactly");
        Check(payload.str().find(L"中") != std::wstring::npos,
              "truncated block retains readable prefix");
    }
    {
        pulse::preview::DocPayload payload(600000);
        payload.Begin(L'p');
        payload.Text(std::wstring(199999, L'a'));
        payload.Text(L"\xD83D\xDE00tail", pulse::preview::kDocBold);
        Check(payload.text().size() == 199999, "block truncation never splits surrogate pair");
        payload.End();
        Check(payload.full(), "multiple runs report truncation");
    }
    host.Stop();
    {
        pulse::preview::DocPayload payload(600000);
        payload.Begin(L'p');
        payload.Text(std::wstring(199999, L'a') + L'\xD83D');
        payload.Text(L"\xDE00");
        Check(payload.text().size() == 199999 && payload.full(),
              "surrogate split across separate runs stays intact at limit");
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
    Check(!ec, "isolated preview fixtures cleaned");
    return failures ? 1 : 0;
}
