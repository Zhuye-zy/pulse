#include "../ui/link_pill.h"
#include <cstdio>
#include <cmath>

int main() {
    using namespace pulse::ui;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        if (!ok) ++failures;
    };
    const D2D1_RECT_F artwork{20, 12, 116, 88};
    const auto closed = LinkPillRect(artwork, 22, 140, 180, 0);
    const auto open = LinkPillRect(artwork, 22, 140, 180, 1);
    check(closed.right - closed.left == 22 && closed.bottom == artwork.bottom,
        "collapsed circle stays inside artwork and adds no line");
    check(open.right == 140 && open.top == closed.top && open.bottom == closed.bottom,
        "expansion obeys cell edge and preserves vertical footprint");
    const auto narrow = LinkPillRect(artwork, 22, 42, 180, 1);
    check(narrow.right == closed.right, "narrow cell retains circle without target overflow");
    check(IsShortcutName(L"x.LNK") && IsShortcutName(L"x.url") && !IsShortcutName(L"lnk") &&
        !IsShortcutName(L"x.lnk.txt"), "shortcut classification is suffix-specific");
    LinkPillMotion motion;
    check(motion.Update(1, 1, false, 1, 1000, false) == 0 && !motion.Active(1000),
        "idle links do not keep requesting frames");
    check(motion.Update(1, 1, true, 2, 1010, false) == 1 && !motion.Active(1010),
        "disabled animations open immediately");
    check(motion.Update(1, 1, false, 3, 1020, false) == 0 && !motion.Active(1020),
        "disabled animations close immediately");
    motion.Update(1, 1, true, 4, 1100, true);
    check(motion.Update(1, 1, true, 5, 1500, true) == 1 && !motion.Active(1500),
        "expanded target settles and stops frame pump");
    motion.Update(1, 1, false, 6, 1510, true);
    check(motion.Update(1, 1, false, 7, 1900, true) == 0 && !motion.Active(1900),
        "pointer exit closes and settles");
    motion.Update(1, 1, true, 8, 2000, true);
    check(motion.Update(2, 1, false, 9, 2010, true) == 0 && !motion.Active(2010),
        "navigation discards same-index stale target");
    return failures ? 1 : 0;
}
