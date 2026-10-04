#include "../app/app_prefs.h"
#include "../app/session_save.h"
#include "../app/startup_location.h"

#include <cstdio>

int main() {
    int failures = 0;
    auto check = [&failures](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failures;
    };

    pulse::app::AppPrefs prefs;
    prefs.persist = false;
    prefs.startup_open = 1;
    check(!pulse::app::RestoresLastTabs(prefs), "normal launch honors default-location preference");
    check(pulse::app::RestoresLastTabs(prefs, true), "update restart restores tabs despite preference");
    check(prefs.startup_open == 1 && !pulse::app::RestoresLastTabs(prefs),
          "update override does not change later normal launches");
    prefs.startup_open = 0;
    check(pulse::app::RestoresLastTabs(prefs), "normal restore preference remains supported");

    std::wstring saved = L"old session";
    int writes = 0;
    bool writable = false;
    auto writer = [&writes, &writable](const std::wstring&) {
        ++writes;
        return writable;
    };
    check(!pulse::app::SaveChangedSession(L"new session", saved, false, writer) &&
          saved == L"old session" && writes == 1,
          "failed save refuses update and leaves previous cache intact");
    writable = true;
    check(pulse::app::SaveChangedSession(L"new session", saved, false, writer) &&
          saved == L"new session" && writes == 2,
          "failed save can be retried successfully");
    check(pulse::app::SaveChangedSession(L"new session", saved, false, writer) && writes == 2,
          "unchanged update session does not write again");
    check(pulse::app::SaveChangedSession(L"new session", saved, true, writer) && writes == 3,
          "normal forced save behavior remains supported");
    return failures == 0 ? 0 : 1;
}
