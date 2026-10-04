// Conflict dialog: hovering a cut-off source or destination path shows it
// whole (#55). Drives the real dialog; optional argv[1] saves a screenshot
// of the expanded dialog with the source path's tooltip shown.
#include "../common/windows_compat.h"
#include "../ui/file_operation_dialog.h"

#include <windows.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <cstdio>
#include <string>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

int g_passed = 0;
int g_failed = 0;
int g_step = 0;
bool g_tip_shown = false;
std::wstring g_source;
std::wstring g_destination;
std::wstring g_shot;

void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    ok ? ++g_passed : ++g_failed;
}

HWND Dialog() { return FindWindowW(L"PulseFileConflictWindow", nullptr); }

HWND g_found_tip = nullptr;
BOOL CALLBACK FindTip(HWND hwnd, LPARAM owner) {
    wchar_t name[64]{};
    GetClassNameW(hwnd, name, ARRAYSIZE(name));
    if (_wcsicmp(name, TOOLTIPS_CLASSW) == 0 && GetWindow(hwnd, GW_OWNER) == reinterpret_cast<HWND>(owner)) {
        g_found_tip = hwnd;
        return FALSE;
    }
    return TRUE;
}

HWND Tooltip(HWND dialog) {
    g_found_tip = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), FindTip, reinterpret_cast<LPARAM>(dialog));
    return g_found_tip;
}

float Scale(HWND hwnd) { return static_cast<float>(pulse::compat::WindowDpi(hwnd)) / 96.0f; }

POINT At(HWND hwnd, float x, float y) {
    const float scale = Scale(hwnd);
    return POINT{ static_cast<LONG>(x * scale), static_cast<LONG>(y * scale) };
}

// Text of the tool under a client point, or empty when there is none.
std::wstring TipAt(HWND tip, HWND dialog, POINT pt) {
    wchar_t text[1024]{};
    TTHITTESTINFOW hit{};
    hit.hwnd = dialog;
    hit.pt = pt;
    hit.ti.cbSize = sizeof(hit.ti);
    hit.ti.lpszText = text;
    if (!SendMessageW(tip, TTM_HITTESTW, 0, reinterpret_cast<LPARAM>(&hit))) return {};
    TOOLINFOW info{ sizeof(info) };
    info.hwnd = dialog;
    info.uId = hit.ti.uId;
    info.lpszText = text;
    SendMessageW(tip, TTM_GETTEXTW, ARRAYSIZE(text), reinterpret_cast<LPARAM>(&info));
    return text;
}

void Click(HWND dialog, POINT pt) {
    const LPARAM where = MAKELPARAM(pt.x, pt.y);
    SendMessageW(dialog, WM_LBUTTONDOWN, MK_LBUTTON, where);
    SendMessageW(dialog, WM_LBUTTONUP, 0, where);
    UpdateWindow(dialog);
}

HBITMAP Capture(HWND hwnd, SIZE& size) {
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    size = SIZE{ rect.right - rect.left, rect.bottom - rect.top };
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, size.cx, size.cy);
    HGDIOBJ old = SelectObject(memory, bitmap);
    PrintWindow(hwnd, memory, PW_RENDERFULLCONTENT);
    SelectObject(memory, old);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return bitmap;
}

// Dialog plus its tooltip, composed where they sit on screen.
bool SaveShot(HWND dialog, HWND tip, const std::wstring& path) {
    RECT dialog_rect{}, tip_rect{};
    GetWindowRect(dialog, &dialog_rect);
    GetWindowRect(tip, &tip_rect);
    RECT all{};
    UnionRect(&all, &dialog_rect, &tip_rect);
    SIZE dialog_size{}, tip_size{};
    HBITMAP dialog_bits = Capture(dialog, dialog_size);
    HBITMAP tip_bits = Capture(tip, tip_size);

    Gdiplus::Bitmap canvas(all.right - all.left, all.bottom - all.top, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics graphics(&canvas);
        graphics.Clear(Gdiplus::Color(255, 96, 96, 96));
        Gdiplus::Bitmap dialog_image(dialog_bits, nullptr);
        Gdiplus::Bitmap tip_image(tip_bits, nullptr);
        graphics.DrawImage(&dialog_image, static_cast<INT>(dialog_rect.left - all.left),
                           static_cast<INT>(dialog_rect.top - all.top));
        graphics.DrawImage(&tip_image, static_cast<INT>(tip_rect.left - all.left),
                           static_cast<INT>(tip_rect.top - all.top));
    }
    DeleteObject(dialog_bits);
    DeleteObject(tip_bits);
    const CLSID png = { 0x557cf406, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
    return canvas.Save(path.c_str(), &png, nullptr) == Gdiplus::Ok;
}

void CALLBACK Step(HWND, UINT, UINT_PTR timer, DWORD) {
    HWND dialog = Dialog();
    if (!dialog) {
        if (++g_step > 40) { Check(false, "the conflict dialog opens"); KillTimer(nullptr, timer); PostQuitMessage(1); }
        return;
    }
    HWND tip = Tooltip(dialog);
    const POINT source_box = At(dialog, 32 + 99, 326 + 55 + 19);
    const POINT destination_box = At(dialog, 252 + 98, 326 + 55 + 19);
    if (!g_tip_shown && g_step < 1000) {
        Check(tip != nullptr, "the dialog owns a tooltip");
        if (!tip) { KillTimer(nullptr, timer); PostMessageW(dialog, WM_CLOSE, 0, 0); return; }
        UpdateWindow(dialog);
        Check(TipAt(tip, dialog, source_box).empty(), "no path tip while the details are folded");
        Click(dialog, At(dialog, 20 + 62, 374 + 16)); // "More details"
        Check(TipAt(tip, dialog, source_box) == g_source, "hovering the source path shows it whole");
        Check(TipAt(tip, dialog, destination_box) == g_destination,
              "hovering the destination path shows it whole");
        Check(TipAt(tip, dialog, At(dialog, 240, 150)).empty(), "the choice cards show no path tip");
        g_step = 1000;
        if (!g_shot.empty()) {
            // A real hover needs the user's mouse; pin the same tip instead.
            wchar_t text[1024]{};
            TOOLINFOW info{ sizeof(info) };
            info.hwnd = dialog;
            info.uId = 1;
            info.lpszText = text;
            SendMessageW(tip, TTM_GETTOOLINFOW, 0, reinterpret_cast<LPARAM>(&info));
            info.uFlags |= TTF_TRACK | TTF_ABSOLUTE;
            SendMessageW(tip, TTM_SETTOOLINFOW, 0, reinterpret_cast<LPARAM>(&info));
            POINT screen = source_box;
            ClientToScreen(dialog, &screen);
            SendMessageW(tip, TTM_TRACKPOSITION, 0, MAKELPARAM(screen.x, screen.y + 22));
            SendMessageW(tip, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&info));
            g_tip_shown = true;
            return; // capture on the next tick, once both windows painted
        }
    }
    if (g_tip_shown) {
        Check(IsWindowVisible(tip) != FALSE, "the pinned tip is visible");
        Check(SaveShot(dialog, tip, g_shot), "screenshot saved");
        g_tip_shown = false;
    }
    KillTimer(nullptr, timer);
    SendMessageW(dialog, WM_KEYDOWN, VK_ESCAPE, 0);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    pulse::compat::EnableDpiAwareness();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Gdiplus::GdiplusStartupInput gdiplus_input;
    ULONG_PTR gdiplus = 0;
    Gdiplus::GdiplusStartup(&gdiplus, &gdiplus_input, nullptr);
    if (argc > 1) g_shot = argv[1];

    g_source = L"C:\\Users\\Example\\AppData\\Local\\Temp\\PulseDrop\\4242-1-90210\\7zE44D7D628"
               L"\\Tools\\Diagnostics\\Release\\x64\\SFM.dll";
    g_destination = L"D:\\Projects\\Archive\\Third Party\\Long Folder Name For Wrapping\\Release"
                    L"\\x64\\SFM.dll";
    pulse::ops::ConflictItemInfo conflict;
    conflict.source = g_source;
    conflict.destination = g_destination;
    conflict.source_size = 412160;
    conflict.destination_size = 409600;
    GetSystemTimeAsFileTime(&conflict.source_modified);
    conflict.destination_modified = conflict.source_modified;
    conflict.remaining = 1;

    WNDCLASSW owner_class{};
    owner_class.lpfnWndProc = DefWindowProcW;
    owner_class.hInstance = GetModuleHandleW(nullptr);
    owner_class.lpszClassName = L"PulseConflictDialogTestOwner";
    RegisterClassW(&owner_class);
    HWND owner = CreateWindowExW(0, owner_class.lpszClassName, L"owner", WS_OVERLAPPEDWINDOW,
                                 80, 80, 900, 700, nullptr, nullptr, owner_class.hInstance, nullptr);

    SetTimer(nullptr, 0, 250, Step);
    const auto result = pulse::ui::ShowFileConflictDialog(owner, conflict, false, D2D1_COLOR_F{ 0.0f, 0.47f, 0.83f, 1.0f });
    Check(result.choice == pulse::ops::ConflictChoice::Cancel, "Escape cancels the dialog");
    DestroyWindow(owner);
    Gdiplus::GdiplusShutdown(gdiplus);
    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
