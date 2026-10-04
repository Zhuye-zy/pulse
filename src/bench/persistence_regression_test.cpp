#include "../app/app_prefs.h"
#include "../app/places.h"
#include "../app/session.h"
#include "../app/app_model.h"
#include "../app/io_task_queue.h"
#include "../app/tag_ads_sync.h"
#include "../common/utf8_file.h"
#include <filesystem>
#include <cstdio>
namespace pulse::app { static std::wstring fixture; std::wstring GetPulseDataDir() { return fixture; } }
static int failures;
static void Check(bool ok, const char* label) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok; }
int main() {
    using namespace pulse; using namespace pulse::app;
    auto dir = std::filesystem::absolute(L"bench_data/persistence-regression-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(dir); fixture = dir.wstring();
    const auto config = fixture + L"\\app.json";
    AppPrefs original; original.background_image = L"original.png";
    Check(original.Save(), "fixture saved");
    std::wstring before; ReadUtf8File(config, before);
    HANDLE locked = CreateFileW(config.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    AppPrefs prefs; prefs.persist = false;
    Check(!prefs.Load() && prefs.load_failed, "PERSIST-01 existing locked configuration records failure");
    CloseHandle(locked); prefs.persist = true;
    Check(!prefs.Save(), "PERSIST-01 unlock alone cannot overwrite with defaults");
    std::wstring after; ReadUtf8File(config, after);
    Check(before==after, "PERSIST-01 original bytes retained");
    prefs.persist=false;
    Check(prefs.Load() && !prefs.load_failed && prefs.background_image==L"original.png", "PERSIST-01 successful reread restores settings");
    WriteUtf8FileAtomic(config, L"{\"background_image\":");
    Check(!prefs.Load() && prefs.load_failed, "PERSIST-01 malformed configuration rejected");
    prefs.persist=true; Check(!prefs.Save(), "PERSIST-01 malformed config protected");
    WriteUtf8FileAtomic(config,before); prefs.persist=false; prefs.Load(); prefs.persist=true;
    LayoutTabSnapshot saved; saved.layout=1; saved.focused=1; saved.target=0;
    PaneFolderSnapshot pc; PaneFolderSnapshot disk; disk.path=L"C:\\";
    saved.panes={pc,disk}; LayoutTab tab;
    RestoreLayoutTab(tab,saved,[](Tab& view,const std::wstring& path){view.current_path=path;});
    Check(tab.panes.size()==2 && tab.panes[0]->ActiveTab()->current_path.empty() && tab.panes[1]->ActiveTab()->current_path==L"C:\\" && tab.focused_index==1 && tab.target_index==0, "PERSIST-02 This PC preserves pane order focus and target");
    const std::wstring places_file=fixture+L"\\places.json";
    const std::wstring json=LR"({"workspaces":[{"name":"a}b\"c","root":"C:\\project}2026","layout":0,"panes":["C:\\project}2026"]},{"name":"next","root":"C:\\next"}],"tags":[{"id":"t","name":"tag}x","rgb":0xABCDEF,"paths":[]}],"networks":[{"name":"n}x","unc":"\\\\host\\share}"}]})";
    WriteUtf8FileAtomic(places_file,json);
    {
        PlacesCatalog places; places.persist=false;
        Check(places.Load() && places.workspaces.size()==2 && places.workspaces[0].name==L"a}b\"c" && places.tags[0].name==L"tag}x" && places.networks[0].name==L"n}x", "PERSIST-03 quoted braces escapes and following records load");
        places.persist=true; Check(places.Save(), "PERSIST-03 round trip saves"); places.persist=false;
        Check(places.Load() && places.workspaces.size()==2 && places.workspaces[0].name==L"a}b\"c", "PERSIST-03 round trip retains records");
        WriteUtf8FileAtomic(places_file,L"{broken");
        Check(!places.Load() && places.load_failed && places.workspaces.size()==2, "PERSIST-03 corrupt reload retains live catalog");
        places.persist=true; Check(!places.Save(), "PERSIST-03 corrupt file cannot be overwritten");
        places.persist=false;
    }
    IoTaskQueue queue; std::wstring order;
    queue.Push([&]{order+=L"add";},true);queue.Push([&]{order+=L"remove";},true);
    auto first=queue.Pop();Check(!queue.Ready(), "PERSIST-04 second ADS update cannot overtake active update");
    queue.Push([&]{order+=L"other";},false);auto parallel=queue.Pop();parallel.task();queue.Complete(parallel);
    first.task();queue.Complete(first);auto second=queue.Pop();second.task();queue.Complete(second);
    Check(order==L"otheraddremove", "PERSIST-04 serial FIFO keeps unrelated IO available");
    const auto file=fixture+L"\\tagged.txt";WriteUtf8FileAtomic(file,L"fixture");
    Check(WriteTagAdsV2(file,{{L"t",L"label",0xABCDEF}}), "PERSIST-04 create ADS");
    locked=CreateFileW((file+L":Pulse.Tag").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    Check(!WriteTagAdsV2(file,{}), "PERSIST-04 deletion failure reported");
    TagAdsUpdate removal; removal.path=file;
    Check(!SyncTagAdsUpdates({removal}).empty() && HasPendingTagAds(file), "PERSIST-04 failed delete retains authoritative pending intent");
    const auto fixture_saved=fixture;fixture+=L"\\other";HasPendingTagAds(file);fixture=fixture_saved;
    Check(HasPendingTagAds(file), "PERSIST-04 pending intent survives journal reload");
    { PlacesCatalog catalog;catalog.persist=false;
      catalog.MergeAdsRecords(file,{{L"old",L"stale",0}},{});
      Check(catalog.tags.empty(), "PERSIST-04 pending path rejects stale ADS reimport"); }
    CloseHandle(locked);
    Check(SyncTagAdsUpdates({}).empty() && !HasPendingTagAds(file), "PERSIST-04 retry clears pending intent after success");
    Check(WriteTagAdsV2(file,{}) && ReadTagAdsV2(file).empty(), "PERSIST-04 delete retry removes ADS");
    const auto old=fixture+L"\\wallpaper.png"; WriteUtf8FileAtomic(old,L"old-image");prefs.background_image=old;prefs.Save();
    Check(!prefs.StoreBackgroundImage(fixture+L"\\missing.png") && prefs.background_image==old && std::filesystem::exists(old), "PERSIST-05 missing source preserves previous image");
    const auto source=fixture+L"\\new.png";WriteUtf8FileAtomic(source,L"new-image");
    locked=CreateFileW(config.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr);
    Check(!prefs.StoreBackgroundImage(source) && prefs.background_image==old && std::filesystem::exists(old), "PERSIST-05 configuration commit failure preserves previous image");CloseHandle(locked);
    Check(prefs.StoreBackgroundImage(source) && std::filesystem::exists(prefs.background_image) && !std::filesystem::exists(old), "PERSIST-05 successful commit precedes old image removal");
    AppPrefs reloaded;reloaded.persist=false;Check(reloaded.Load() && reloaded.background_image==prefs.background_image, "PERSIST-05 new cache path survives reload");
    std::filesystem::remove_all(dir);
    return failures ? 1 : 0;
}
