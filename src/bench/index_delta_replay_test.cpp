#include "../index/index_engine.h"
#include "../index/index_delta.h"
#include "../index/index_paths.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

namespace pulse::index {
struct EngineTestAccess {
    static bool PublishBase(Engine& engine) {
        Engine::Store store;
        std::vector<Engine::VolState> volumes;
        const auto path = CacheFilePath();
        return engine.FlattenLocked(store, volumes) && engine.WriteIndexFile(path, store, volumes, engine.built_unix_) &&
            MoveFileExW((path + L".tmp").c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    static void MakeLegacy(const std::wstring& path) {
        std::ifstream input(path, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
        input.close();
        // A genuine v1 Add has no ID after its one-byte opcode.
        bytes.erase(bytes.begin() + 17, bytes.begin() + 21);
        bytes[4] = 1;
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    static void Init(Engine& engine) {
        engine.running_ = true;
        engine.ready_ = true;
        engine.built_unix_ = 123456;
        for (wchar_t letter : {L'C', L'D'}) {
            Engine::VolState volume;
            volume.letter = letter;
            volume.volume_id = std::wstring(L"replay-fixture-") + letter;
            volume.root_idx = engine.AddNodeLocked(engine.live_, -1, std::wstring(1, letter) + L":",
                Engine::kFlagDir, 0, 0, 0, true);
            volume.first_idx = volume.root_idx;
            engine.vols_.push_back(std::move(volume));
        }
    }
    static bool Run() {
        bool ok = true;
        auto check = [&](bool value, const char* name) {
            std::cout << (value ? "[PASS] " : "[FAIL] ") << name << '\n'; ok &= value;
        };
        const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"delta-replay-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
        SetMachineIndexScope(false);
        auto select = [&](const wchar_t* name) {
            const auto directory = root / name;
            std::filesystem::create_directories(directory);
            SetActiveIndexDirectory(directory.wstring());
        };
        for (bool reversed : {false, true}) {
            select(reversed ? L"reverse" : L"interleaved");
            {
                Engine engine; Init(engine);
                DeltaLog c, d;
                check(c.Open(DeltaFilePathForVolume(engine.vols_[0].volume_id), engine.built_unix_) &&
                      d.Open(DeltaFilePathForVolume(engine.vols_[1].volume_id), engine.built_unix_), "open isolated volume WALs");
                d.QueueAdd(2, 1, Engine::kFlagDir, 0, 0, L"directory", 100);
                c.QueueAdd(3, 0, 0, 0, 1, L"file.txt", 200);
                d.QueueAdd(4, 2, 0, 0, 2, L"child.txt", 101);
                c.QueueAdd(5, 0, 0, 0, 3, L"deleted.txt", 201);
                d.QueuePatch(2, static_cast<uint8_t>(PatchBits::Name), 0, 0, 0, 0, L"renamed");
                d.QueuePatch(4, static_cast<uint8_t>(PatchBits::Attr), 0, 0, 42, 987, {});
                c.QueueTomb(5);
                c.QueueUsn(91, 111); d.QueueUsn(92, 222);
                check(c.Flush() && d.Flush(), "flush interleaved additions, patches, tombstone and cursors");
            }
            {
                Engine engine; Init(engine);
                if (reversed) std::reverse(engine.vols_.begin(), engine.vols_.end());
                check(engine.ReplayDeltasLocked(), "replay accepts stable IDs independent of volume order");
                check(engine.LiveCount() == 6 && engine.NodeAt(4).parent == 2 &&
                      (engine.NodeAt(2).flags & Engine::kFlagDir) && engine.NameOf(2) == L"renamed" &&
                      engine.NameOf(3) == L"file.txt", "child retains directory parent and no cross-volume reassignment");
                check(engine.AttrAt(4).size == 987 && engine.AttrAt(4).mtime == 42 &&
                      engine.IsTomb(5) && !engine.IsTomb(4), "patches and tombstones retain original targets");
                const auto& d = engine.vols_[reversed ? 0 : 1];
                check(engine.FindByFrnLocked(d, 100) == 2 && engine.FindByFrnLocked(d, 101) == 4 &&
                      d.journal_id == 92 && d.next_usn == 222, "volume FRN mapping and journal checkpoint survive replay");
            }
        }
        for (const auto* mode : {L"legacy", L"gap", L"duplicate", L"truncated", L"bad-parent"}) {
            select(mode);
            Engine engine; Init(engine);
            const auto path = DeltaFilePathForVolume(engine.vols_[0].volume_id);
            {
                DeltaLog log; check(log.Open(path, engine.built_unix_), "open rejection fixture");
                if (std::wstring_view(mode) == L"gap") log.QueueAdd(3, 0, 0, 0, 0, L"gap", 300);
                else {
                    log.QueueAdd(2, 0, 0, 0, 0, L"file", 300);
                    if (std::wstring_view(mode) == L"duplicate") log.QueueAdd(2, 0, 0, 0, 0, L"duplicate", 301);
                    if (std::wstring_view(mode) == L"bad-parent") log.QueueAdd(3, 2, 0, 0, 0, L"child", 301);
                }
                check(log.Flush(), "flush rejection fixture");
            }
            if (std::wstring_view(mode) == L"legacy") {
                MakeLegacy(path);
            }
            if (std::wstring_view(mode) == L"truncated") std::filesystem::resize_file(path, std::filesystem::file_size(path) - 1);
            check(!engine.ReplayDeltasLocked(), "reject legacy, noncontiguous, duplicate, truncated or invalid hierarchy WAL");
            if (std::wstring_view(mode) != L"bad-parent") check(engine.LiveCount() == 2, "failed preflight leaves original snapshot untouched");
            check(std::filesystem::exists(path), "rejected WAL remains available for diagnosis");
        }
        select(L"restart");
        {
            Engine engine; Init(engine);
            check(PublishBase(engine), "publish valid mapped baseline for legacy recovery");
            const auto path = DeltaFilePathForVolume(engine.vols_[0].volume_id);
            {
                DeltaLog log; check(log.Open(path, engine.built_unix_), "open legacy recovery WAL");
                log.QueueAdd(2, 0, 0, 0, 0, L"legacy-file", 300);
                check(log.Flush(), "flush legacy recovery WAL");
            }
            MakeLegacy(path);
        }
        {
            Engine rejected;
            check(!rejected.TryLoadCache() && !rejected.ready_ && rejected.folder_size_gap_ && rejected.LiveCount() == 0,
                "legacy cache load requests rebuild without exposing partial or complete stale results");
        }
        {
            // Exercise the publication/open-WAL sequence used after FullRebuild,
            // without enumerating any real volume or starting the worker.
            Engine recovered; Init(recovered); recovered.built_unix_ = 654321;
            check(PublishBase(recovered), "publish replacement baseline after legacy rejection");
            recovered.OpenDeltasLocked();
            auto* log = recovered.DeltaFor(L'C');
            check(log && log->BaseBuilt() == 654321, "replacement baseline opens v2 WAL instead of legacy records");
            if (log) {
                log->QueueAdd(2, 0, 0, 0, 7, L"after-recovery.txt", 400);
                check(log->Flush(), "persist stable-ID addition after recovery");
            }
        }
        for (int restart = 0; restart < 2; ++restart) {
            Engine restored;
            check(restored.TryLoadCache() && restored.ready_ && restored.LiveCount() == 3 &&
                  restored.NameOf(2) == L"after-recovery.txt" && restored.AttrAt(2).size == 7,
                "v2 recovery survives consecutive restarts without repeated rebuild");
        }
        for (const auto* mode : {L"cross-add", L"cross-patch", L"cross-move", L"cross-tomb"}) {
            select(mode);
            Engine engine; Init(engine);
            {
                DeltaLog log;
                check(log.Open(DeltaFilePathForVolume(engine.vols_[0].volume_id), engine.built_unix_),
                    "open cross-volume ownership fixture");
                if (std::wstring_view(mode) == L"cross-add") log.QueueAdd(2, 1, 0, 0, 0, L"wrong-volume", 500);
                else if (std::wstring_view(mode) == L"cross-patch")
                    log.QueuePatch(1, static_cast<uint8_t>(PatchBits::Attr), 0, 0, 42, 99, {});
                else if (std::wstring_view(mode) == L"cross-tomb") log.QueueTomb(1);
                else {
                    log.QueueAdd(2, 0, 0, 0, 0, L"same-volume", 500);
                    log.QueuePatch(2, static_cast<uint8_t>(PatchBits::Meta), 1, 0, 0, 0, {});
                }
                check(log.Flush(), "flush cross-volume ownership fixture");
            }
            check(!engine.ReplayDeltasLocked(), "reject cross-volume addition, patch target, move or tombstone");
        }
        select(L"walk");
        {
            Engine engine; Init(engine); engine.vols_.clear();
            {
                DeltaLog log; check(log.Open(DeltaFilePath(0), engine.built_unix_), "open walk-mode WAL");
                log.QueueAdd(2, 0, 0, 0, 9, L"walk-file.txt", 0);
                check(log.Flush(), "flush walk-mode WAL");
            }
            check(engine.ReplayDeltasLocked() && engine.NameOf(2) == L"walk-file.txt" && engine.AttrAt(2).size == 9,
                "walk-mode replay does not require volume ownership metadata");
            MakeLegacy(DeltaFilePath(0));
            engine.built_unix_ = 654321;
            engine.OpenDeltasLocked();
            const auto* log = engine.DeltaFor(0);
            check(log && log->BaseBuilt() == 654321, "walk-mode recovery replaces rejected legacy WAL after rebuilding");
        }
        SetActiveIndexDirectory(L"");
        std::error_code error;
        std::filesystem::remove_all(root, error);
        check(!error, "isolated fixture cleanup");
        return ok;
    }
};
}
int main() { return pulse::index::EngineTestAccess::Run() ? 0 : 1; }
