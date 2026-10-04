#include "edit_host.h"
#include "../common/windows_compat.h"
#include "folder_picker_dialog.h"

#include "folder_picker_loader.h"
#include "folder_picker_view.h"
#include "typography.h"
#include "window_helpers.h"
#include "../common/display_path.h"
#include "../common/localization.h"

#include <commctrl.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <memory>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

namespace pulse::ui {
namespace {

constexpr wchar_t kPickerClass[] = L"PulseFolderPickerWindow";
constexpr UINT kListingMessage = WM_APP + 1;
constexpr UINT_PTR kLoadTimer = 1;
constexpr UINT kSpinnerDelayMs = 150;
constexpr UINT kSpinnerFrameMs = 33;
constexpr float kDefaultWidth = 780.0f;
constexpr float kDefaultHeight = 540.0f;
constexpr float kMinWidth = 620.0f;
constexpr float kMinHeight = 420.0f;

// Where the last pick of each mode was made, for this session.
std::wstring g_last_folder[2];

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw)) && raw)
        path = raw;
    if (raw) CoTaskMemFree(raw);
    return path;
}

std::vector<PickerPlace> BuildPlaces() {
    struct Known {
        const KNOWNFOLDERID* id;
        l10n::StringId label;
        const wchar_t* glyph;
        uint32_t rgb;
    };
    const Known known[] = {
        {&FOLDERID_Desktop, l10n::StringId::Desktop, L"\xE7F4", 0x38BDF8},
        {&FOLDERID_Documents, l10n::StringId::PickerDocuments, L"\xE8A5", 0x60A5FA},
        {&FOLDERID_Downloads, l10n::StringId::Downloads, L"\xE896", 0xC084FC},
        {&FOLDERID_Pictures, l10n::StringId::PickerPictures, L"\xEB9F", 0xF472B6},
        {&FOLDERID_Profile, l10n::StringId::PickerHome, L"\xE80F", 0x34D399},
    };
    std::vector<PickerPlace> places;
    for (const Known& k : known) {
        std::wstring path = KnownFolder(*k.id);
        if (path.empty()) continue;
        places.push_back({l10n::Get(k.label), std::move(path), k.glyph, HexColor(k.rgb), false});
    }
    places.push_back({l10n::Get(l10n::StringId::ThisPc), L"", L"\xE977", {}, true});
    return places;
}

// "\\\\server\\share" reads better whole than as just "share".
bool IsShareRootName(const std::wstring& path) {
    return path.rfind(L"\\\\", 0) == 0 && PickerParent(path).empty();
}

std::wstring ErrorText(DWORD error, const std::wstring& path) {
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
    case ERROR_DIRECTORY: {
        std::wstring text = l10n::Get(l10n::StringId::PickerNotFound);
        const size_t at = text.find(L"{path}");
        // The full path is already in the path field; the one-line message
        // names the missing folder itself.
        std::wstring name = path::FriendlyPathText(path);
        while (name.size() > 3 && name.back() == L'\\') name.pop_back();
        const size_t slash = name.find_last_of(L'\\');
        if (slash != std::wstring::npos && slash + 1 < name.size() && !IsShareRootName(name))
            name = name.substr(slash + 1);
        if (at != std::wstring::npos) text.replace(at, 6, name);
        return text;
    }
    default:
        break;
    }
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text = length && buffer ? std::wstring(buffer, length) : std::wstring();
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' ||
                             text.back() == L' ' || text.back() == L'.' ||
                             text.back() == 0x3002))
        text.pop_back();
    return text;
}

class FolderPickerWindow {
public:
    bool Show(HWND owner, const FolderPickerSpec& spec, bool dark, D2D1_COLOR_F accent,
              std::wstring& out) {
        owner_ = owner;
        dark_ = dark;
        accent_ = accent;
        mode_index_ = spec.mode == PickerMode::Image ? 1 : 0;
        visual_.mode = spec.mode;
        visual_.title = !spec.title.empty() ? spec.title
            : l10n::Get(spec.mode == PickerMode::Image
                            ? l10n::StringId::TooltipChooseBackground
                            : l10n::StringId::PickerTitleFolder);
        visual_.primary_text = l10n::Get(spec.mode == PickerMode::Image
                                             ? l10n::StringId::PickerSelect
                                             : l10n::StringId::PickerSelectFolder);
        visual_.cancel_text = l10n::Get(l10n::StringId::Cancel);
        visual_.places = BuildPlaces();
        visual_.hosted_edit = true;
        visual_.focus = kPickList;
        fallback_ = KnownFolder(spec.mode == PickerMode::Image ? FOLDERID_Pictures
                                                               : FOLDERID_Desktop);
        std::wstring start = NormalizePickerInput(spec.initial_path);
        if (start.empty()) start = g_last_folder[mode_index_];
        if (start.empty()) start = fallback_;
        scale_ = static_cast<float>(pulse::compat::WindowDpi(owner ? owner : GetDesktopWindow()))
               / 96.0f;

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.style = CS_DBLCLKS;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kPickerClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!GetClassInfoExW(wc.hInstance, kPickerClass, &wc)) RegisterClassExW(&wc);

        const int width = static_cast<int>(kDefaultWidth * scale_);
        const int height = static_cast<int>(kDefaultHeight * scale_);
        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kPickerClass, visual_.title.c_str(),
            WS_POPUP | WS_THICKFRAME | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, width, height,
            owner, nullptr, wc.hInstance, this);
        if (!hwnd_) return false;
        // WM_CREATE read the DPI of the monitor the window landed on, which
        // can differ from the owner's guess above.
        CenterOwnedWindow(hwnd_, owner_, static_cast<int>(kDefaultWidth * scale_),
                          static_cast<int>(kDefaultHeight * scale_));
        loader_ = std::make_unique<PickerLoader>(hwnd_, kListingMessage);
        initial_load_ = true;
        Navigate(start, false);
        if (owner_) EnableWindow(owner_, FALSE);
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        SetFocus(hwnd_);
        PresentEdit();

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            RedirectStrayModalKey(message, hwnd_);
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Close();
        if (owner_) EnableWindow(owner_, TRUE);
        if (result_.empty()) return false;
        out = result_;
        return true;
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<FolderPickerWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<FolderPickerWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        return self ? self->Handle(message, wparam, lparam)
                    : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    // ---- navigation -------------------------------------------------------

    // By value: callers pass entry paths that the reset below destroys.
    // `verified`: the folder is known to exist (it was just listed as a row, or
    // is the parent of a folder that listed), so it may be picked while it
    // loads. Typed, remembered and history paths wait for their listing.
    void Navigate(std::wstring path, bool push_history, std::wstring select_after = {},
                  bool verified = false) {
        if (push_history && !SamePickerPath(path, visual_.current))
            history_.Navigate(visual_.current);
        visual_.current = path;
        current_verified_ = verified;
        visual_.entries.clear();
        visual_.selected = -1;
        visual_.scroll = 0.0f;
        visual_.error.clear();
        visual_.waiting = true;
        visual_.loading = false;
        visual_.can_back = history_.CanGoBack();
        visual_.can_up = !path.empty();
        select_after_ = std::move(select_after);
        load_started_ = GetTickCount64();
        SetEditText(DisplayPath(path));
        UpdateChosen();
        if (loader_) loader_->Load(++generation_, path, visual_.mode);
        if (hwnd_) {
            SetTimer(hwnd_, kLoadTimer, kSpinnerDelayMs, nullptr);
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void GoUp() {
        if (visual_.current.empty()) return;
        const std::wstring child = visual_.current;
        Navigate(PickerParent(child), true, child, current_verified_);
    }

    void GoBack() {
        if (!history_.CanGoBack()) return;
        const std::wstring child = visual_.current;
        const std::wstring target = history_.Back();
        Navigate(target, false, child);
    }

    void Reload() {
        const std::wstring keep = SelectedEntry() ? SelectedEntry()->path : std::wstring();
        Navigate(visual_.current, false, keep);
    }

    void OnListing(std::unique_ptr<PickerListing> listing) {
        if (!listing || listing->generation != generation_) return;
        KillTimer(hwnd_, kLoadTimer);
        visual_.waiting = false;
        visual_.loading = false;
        current_verified_ = listing->error == ERROR_SUCCESS;
        if (listing->error != ERROR_SUCCESS) {
            if (initial_load_ && !fallback_.empty() &&
                !SamePickerPath(listing->path, fallback_)) {
                // A remembered or suggested folder that is gone: start from
                // the default place instead of an error page.
                Navigate(fallback_, false);
                return;
            }
            visual_.error = ErrorText(listing->error, listing->path);
            if (visual_.error.empty())
                visual_.error = l10n::Get(l10n::StringId::PickerOpenFailed);
        }
        initial_load_ = false;
        visual_.entries = std::move(listing->entries);
        if (!select_after_.empty()) {
            for (size_t i = 0; i < visual_.entries.size(); ++i) {
                if (SamePickerPath(visual_.entries[i].path, select_after_)) {
                    Select(static_cast<int>(i));
                    break;
                }
            }
            select_after_.clear();
        }
        UpdateChosen();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    const PickerEntry* SelectedEntry() const {
        if (visual_.selected < 0 || visual_.selected >= static_cast<int>(visual_.entries.size()))
            return nullptr;
        return &visual_.entries[static_cast<size_t>(visual_.selected)];
    }

    void UpdateChosen() {
        visual_.chosen = visual_.error.empty() && !visual_.waiting
            ? PickerChosenPath(visual_.mode, visual_.current, SelectedEntry())
            : std::wstring();
        // While a verified folder loads, folder mode can still pick the folder
        // itself; an unverified path is not choosable until its listing succeeds.
        if (visual_.chosen.empty() && visual_.waiting && current_verified_ &&
            visual_.mode == PickerMode::Folder)
            visual_.chosen = visual_.current;
    }

    void Select(int index) {
        const int count = static_cast<int>(visual_.entries.size());
        if (count == 0) { visual_.selected = -1; UpdateChosen(); return; }
        visual_.selected = std::clamp(index, 0, count - 1);
        visual_.scroll = ScrollPickerRowIntoView(layout_, visual_.entries.size(), visual_.scroll,
                                                 visual_.selected);
        UpdateChosen();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // Double click or Enter on an entry.
    void OpenEntry(int index) {
        if (index < 0 || index >= static_cast<int>(visual_.entries.size())) return;
        const PickerEntry& entry = visual_.entries[static_cast<size_t>(index)];
        if (entry.kind == PickerEntryKind::Image) {
            Finish(entry.path);
            return;
        }
        Navigate(entry.path, true, {}, true);
    }

    void Finish(const std::wstring& path) {
        if (path.empty()) return;
        result_ = path;
        // Next time start in the folder this pick was made from.
        g_last_folder[mode_index_] = visual_.current.empty() ? path : visual_.current;
        done_ = true;
        PostMessageW(hwnd_, WM_NULL, 0, 0);
    }

    void Cancel() {
        result_.clear();
        done_ = true;
        PostMessageW(hwnd_, WM_NULL, 0, 0);
    }

    void Close() {
        if (!hwnd_) return;
        KillTimer(hwnd_, kLoadTimer);
        // Stop the loader first: after this no listing can be posted, so the
        // ones already queued are all that must be freed.
        loader_.reset();
        MSG pending{};
        while (PeekMessageW(&pending, hwnd_, kListingMessage, kListingMessage, PM_REMOVE))
            PickerLoader::Take(pending.lParam);
        if (IsWindow(hwnd_)) {
            HideComposedDialog(hwnd_, owner_);
            DestroyWindow(hwnd_);
        }
        hwnd_ = nullptr;
    }

    std::wstring DisplayPath(const std::wstring& path) const {
        return path.empty() ? l10n::Get(l10n::StringId::ThisPc) : path::FriendlyPathText(path);
    }

    // ---- path field --------------------------------------------------------

    D2D1_COLOR_F EditForeground() const { return ColorFromRef(EditTextColor(dark_)); }
    D2D1_COLOR_F EditBackground() const { return ColorFromRef(EditBackColor(dark_)); }

    HBRUSH EditBrush() const { return EditBackBrush(edit_brush_); }

    void CreateFonts() {
        if (font_) { DeleteObject(font_); font_ = nullptr; }
        const int height = -std::max(1, static_cast<int>(std::lround(14.0f * scale_)));
        font_ = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, typography::PreferredTextFamily());
        if (!font_) {
            font_ = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        }
        if (edit_ && font_) SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }

    void CreateEdit() {
        edit_ = CreateChildEdit(hwnd_, L"");
        if (!edit_) return;
        SetWindowTheme(edit_, L"", L"");
        if (!compositor_.LumaTextEnabled()) SetLayeredWindowAttributes(edit_, 0, 255, LWA_ALPHA);
        if (font_) SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        SetWindowSubclass(edit_, EditProc, 1, reinterpret_cast<DWORD_PTR>(this));
    }

    void SetEditText(const std::wstring& text) {
        edit_text_ = text;
        if (!edit_) return;
        SetWindowTextW(edit_, text.c_str());
        PresentEdit();
    }

    void PresentEdit() {
        if (edit_ && IsWindowVisible(edit_))
            PresentChildEdit(compositor_, compositor_.TextFormat(), EditForeground(),
                             EditBackground(), edit_);
    }

    void PlaceEdit() {
        if (!edit_ || !hwnd_) return;
        const D2D1_RECT_F& cell = layout_.path;
        const int x = static_cast<int>(std::lround(cell.left + 10.0f * scale_));
        const int w = std::max(40, static_cast<int>(std::lround(cell.right - cell.left -
                                                                20.0f * scale_)));
        const int cell_h = std::max(18, static_cast<int>(std::lround(cell.bottom - cell.top)));
        int line_h = cell_h;
        if (font_) {
            HDC hdc = GetDC(edit_);
            HFONT old = static_cast<HFONT>(SelectObject(hdc, font_));
            TEXTMETRICW tm{};
            GetTextMetricsW(hdc, &tm);
            SelectObject(hdc, old);
            ReleaseDC(edit_, hdc);
            line_h = std::max(1, static_cast<int>(tm.tmHeight));
        }
        line_h = std::min(line_h, cell_h);
        const int y = static_cast<int>(std::lround(cell.top)) + std::max(0, (cell_h - line_h) / 2);
        SetWindowPos(edit_, HWND_TOP, x, y, w, line_h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    void FocusPath() {
        if (!edit_) return;
        SetFocus(edit_);
        SendMessageW(edit_, EM_SETSEL, 0, -1);
    }

    void FocusList() {
        visual_.focus = kPickList;
        SetFocus(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void SubmitPath() {
        const int length = GetWindowTextLengthW(edit_);
        std::wstring text(static_cast<size_t>(std::max(0, length)) + 1, L'\0');
        GetWindowTextW(edit_, text.data(), length + 1);
        text.resize(static_cast<size_t>(std::max(0, length)));
        std::wstring target = text == l10n::Get(l10n::StringId::ThisPc)
            ? std::wstring() : NormalizePickerInput(text);
        if (target.empty() && !text.empty() && text != l10n::Get(l10n::StringId::ThisPc)) {
            SetEditText(edit_text_);
            return;
        }
        // A typed picture path selects it in its folder.
        if (visual_.mode == PickerMode::Image && IsPickerImageName(target)) {
            Navigate(PickerParent(target), true, target);
        } else {
            Navigate(target, true);
        }
        FocusList();
    }

    static LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
                                     UINT_PTR, DWORD_PTR ref) {
        auto* self = reinterpret_cast<FolderPickerWindow*>(ref);
        if (!self) return DefSubclassProc(hwnd, msg, wparam, lparam);
        switch (msg) {
        case WM_KEYDOWN:
            if (wparam == VK_RETURN) { self->SubmitPath(); return 0; }
            if (wparam == VK_ESCAPE) {
                self->SetEditText(self->edit_text_);
                self->FocusList();
                return 0;
            }
            if (wparam == VK_TAB) {
                if (GetKeyState(VK_SHIFT) & 0x8000) {
                    self->visual_.focus = self->visual_.chosen.empty() ? kPickCancel
                                                                       : kPickPrimary;
                    self->visual_.show_focus = true;
                    SetFocus(self->hwnd_);
                    InvalidateRect(self->hwnd_, nullptr, FALSE);
                } else {
                    self->visual_.show_focus = true;
                    self->FocusList();
                }
                return 0;
            }
            break;
        case WM_CHAR:
            if (wparam == VK_RETURN || wparam == VK_ESCAPE || wparam == VK_TAB) return 0;
            break;
        case WM_SETFOCUS:
            self->visual_.path_focused = true;
            self->visual_.focus = kPickPath;
            break;
        case WM_KILLFOCUS:
            self->visual_.path_focused = false;
            break;
        }
        const bool repaint = msg == WM_SETFOCUS || msg == WM_KILLFOCUS ||
                             msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK;
        LRESULT result = 0;
        if (!HandleChildEditMessage(self->compositor_, self->compositor_.TextFormat(),
                                    self->EditForeground(), self->EditBackground(),
                                    self->EditBrush(), hwnd, msg, wparam, lparam, result)) {
            result = DefPresentedChildEditProc(self->compositor_, self->compositor_.TextFormat(),
                                               self->EditForeground(), self->EditBackground(),
                                               hwnd, msg, wparam, lparam);
        }
        if (repaint && self->hwnd_) InvalidateRect(self->hwnd_, nullptr, FALSE);
        return result;
    }

    // ---- layout and painting ----------------------------------------------

    void Relayout() {
        RECT client{};
        GetClientRect(hwnd_, &client);
        layout_ = LayoutFolderPicker(static_cast<float>(client.right),
                                     static_cast<float>(client.bottom), visual_.places, painter_,
                                     visual_.primary_text, visual_.cancel_text, scale_);
        visual_.scroll = ClampPickerScroll(layout_, visual_.entries.size(), visual_.scroll);
        PlaceEdit();
    }

    void Render() {
        if (compositor_.NeedsRecovery()) {
            if (!compositor_.Recover()) return;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
        }
        if (!compositor_.Dc()) return;
        const bool high_contrast = IsHighContrast();
        const Theme theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
        BeginSurface(compositor_, painter_, theme, dark_, high_contrast, backdrop_enabled_,
                     scale_);
        DrawFolderPicker(compositor_, painter_, theme, visual_, layout_, dark_, high_contrast);
        EndSurface(compositor_);
        PresentEdit();
    }

    int Hit(LPARAM lparam) const {
        return HitTestFolderPicker(layout_, visual_, static_cast<float>(GET_X_LPARAM(lparam)),
                                   static_cast<float>(GET_Y_LPARAM(lparam)));
    }

    void ScrollBy(float delta) {
        visual_.scroll = ClampPickerScroll(layout_, visual_.entries.size(),
                                           visual_.scroll + delta);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // Thumb dragging maps the pointer linearly onto the scroll range.
    void DragScrollbar(float y) {
        const float viewport = layout_.rows.bottom - layout_.rows.top;
        const float content = PickerContentHeight(layout_, visual_.entries.size());
        if (viewport <= 0.0f || content <= viewport) return;
        const float ratio = std::clamp((y - layout_.rows.top) / viewport, 0.0f, 1.0f);
        visual_.scroll = ClampPickerScroll(layout_, visual_.entries.size(),
                                           ratio * content - viewport * 0.5f);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    int PageRows() const {
        return std::max(1, static_cast<int>((layout_.rows.bottom - layout_.rows.top) /
                                            layout_.row_h) - 1);
    }

    std::vector<int> FocusOrder() const {
        std::vector<int> order{kPickPath, kPickList, kPickCancel};
        if (!visual_.chosen.empty()) order.push_back(kPickPrimary);
        return order;
    }

    void MoveFocus(int direction) {
        const std::vector<int> order = FocusOrder();
        auto it = std::find(order.begin(), order.end(), visual_.focus);
        int index = it == order.end() ? 1 : static_cast<int>(it - order.begin());
        const int count = static_cast<int>(order.size());
        index = (index + direction + count) % count;
        visual_.focus = order[static_cast<size_t>(index)];
        visual_.show_focus = true;
        if (visual_.focus == kPickPath) FocusPath();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void Activate(int id) {
        if (id == kPickClose || id == kPickCancel) Cancel();
        else if (id == kPickPrimary) Finish(visual_.chosen);
        else if (id == kPickBack) GoBack();
        else if (id == kPickUp) GoUp();
        else if (id >= kPickPlace && id < kPickRow) {
            const size_t index = static_cast<size_t>(id - kPickPlace);
            if (index < visual_.places.size()) Navigate(visual_.places[index].path, true);
        }
    }

    void EnterOnList() {
        if (SelectedEntry()) OpenEntry(visual_.selected);
        else if (!visual_.chosen.empty()) Finish(visual_.chosen);
    }

    bool HandleKey(WPARAM key) {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const int count = static_cast<int>(visual_.entries.size());
        auto list_move = [&](int index) {
            visual_.focus = kPickList;
            visual_.show_focus = true;
            if (count > 0) Select(index);
        };
        switch (key) {
        case VK_ESCAPE: Cancel(); return true;
        case VK_TAB: MoveFocus((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1); return true;
        case VK_RETURN:
            if (visual_.focus == kPickCancel || visual_.focus == kPickPrimary)
                Activate(visual_.focus);
            else EnterOnList();
            return true;
        case VK_SPACE:
            if (visual_.focus == kPickCancel || visual_.focus == kPickPrimary)
                Activate(visual_.focus);
            return true;
        case VK_UP: list_move(visual_.selected < 0 ? count - 1 : visual_.selected - 1);
            return true;
        case VK_DOWN: list_move(visual_.selected + 1); return true;
        case VK_HOME: list_move(0); return true;
        case VK_END: list_move(count - 1); return true;
        case VK_PRIOR: list_move(std::max(0, visual_.selected) - PageRows()); return true;
        case VK_NEXT: list_move(std::max(0, visual_.selected) + PageRows()); return true;
        case VK_BACK: GoUp(); return true;
        case VK_F5: Reload(); return true;
        case VK_F4: FocusPath(); return true;
        case 'L':
            if (ctrl) { FocusPath(); return true; }
            break;
        }
        return false;
    }

    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            scale_ = static_cast<float>(pulse::compat::WindowDpi(hwnd_)) / 96.0f;
            backdrop_enabled_ = ApplyBackdrop(hwnd_, dark_);
            if (!compositor_.Init(hwnd_)) return -1;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            CreateFonts();
            edit_brush_ = CreateSolidBrush(dark_ ? RGB(30, 30, 30) : RGB(255, 255, 255));
            CreateEdit();
            Relayout();
            return 0;
        case WM_NCCALCSIZE:
            return 0;
        case WM_NCHITTEST:
            return BorderlessHitTest(hwnd_, lparam, layout_.title_bar, layout_.close);
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize.x = static_cast<LONG>(kMinWidth * scale_);
            info->ptMinTrackSize.y = static_cast<LONG>(kMinHeight * scale_);
            return 0;
        }
        case WM_SIZE:
            if (compositor_.Dc()) compositor_.Resize(LOWORD(lparam), HIWORD(lparam));
            Relayout();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_MOVE:
            compositor_.UpdateTextRenderingParams(
                MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
            PlaceEdit();
            return 0;
        case WM_DPICHANGED: {
            scale_ = HIWORD(wparam) / 96.0f;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetScale(scale_);
            CreateFonts();
            if (const auto* suggested = reinterpret_cast<RECT*>(lparam)) {
                SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
            }
            Relayout();
            return 0;
        }
        case WM_CTLCOLOREDIT: {
            const HDC hdc = reinterpret_cast<HDC>(wparam);
            SetTextColor(hdc, EditTextColor(dark_));
            SetBkColor(hdc, EditBackColor(dark_));
            return reinterpret_cast<LRESULT>(EditBrush());
        }
        case WM_TIMER:
            if (wparam == kLoadTimer) {
                if (!visual_.waiting) { KillTimer(hwnd_, kLoadTimer); return 0; }
                if (!visual_.loading) {
                    visual_.loading = true;
                    SetTimer(hwnd_, kLoadTimer, kSpinnerFrameMs, nullptr);
                }
                const ULONGLONG elapsed = GetTickCount64() - load_started_;
                visual_.spinner = static_cast<float>(elapsed % 1000ULL) / 1000.0f;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case kListingMessage:
            OnListing(PickerLoader::Take(lparam));
            return 0;
        case WM_MOUSEMOVE: {
            if (dragging_scrollbar_) {
                DragScrollbar(static_cast<float>(GET_Y_LPARAM(lparam)));
                return 0;
            }
            const int next = Hit(lparam);
            if (next != visual_.hover) {
                visual_.hover = next;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            visual_.hover = kPickNone;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_MOUSEWHEEL:
            ScrollBy(-static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA *
                     layout_.row_h * 3.0f);
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            const int hit = Hit(lparam);
            visual_.show_focus = false;
            if (hit >= kPickRow) {
                const int index = hit - kPickRow;
                visual_.focus = kPickList;
                SetFocus(hwnd_);
                Select(index);
                if (message == WM_LBUTTONDBLCLK) OpenEntry(index);
                return 0;
            }
            if (hit == kPickList) {
                FocusList();
                visual_.selected = -1;
                UpdateChosen();
                return 0;
            }
            if (hit == kPickScrollbar) {
                dragging_scrollbar_ = true;
                SetCapture(hwnd_);
                DragScrollbar(static_cast<float>(GET_Y_LPARAM(lparam)));
                return 0;
            }
            if (hit == kPickPath) { FocusPath(); return 0; }
            visual_.pressed = hit;
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            if (dragging_scrollbar_) {
                dragging_scrollbar_ = false;
                ReleaseCapture();
                return 0;
            }
            const int hit = Hit(lparam);
            const int pressed = visual_.pressed;
            visual_.pressed = kPickNone;
            ReleaseCapture();
            if (pressed != kPickNone && hit == pressed) Activate(hit);
            if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_CAPTURECHANGED:
            dragging_scrollbar_ = false;
            return 0;
        case WM_XBUTTONUP:
            if (GET_XBUTTON_WPARAM(wparam) == XBUTTON1) GoBack();
            return TRUE;
        case WM_KEYDOWN:
            if (HandleKey(wparam)) return 0;
            break;
        case WM_SYSKEYDOWN:
            if (wparam == VK_UP) { GoUp(); return 0; }
            if (wparam == VK_LEFT) { GoBack(); return 0; }
            if (wparam == 'D') { FocusPath(); return 0; }
            break;
        case WM_CHAR:
            if (wparam >= 0x20 && wparam != 0x7F && visual_.focus == kPickList) {
                const int next = PickerTypeAhead(visual_.entries, visual_.selected,
                                                 static_cast<wchar_t>(wparam));
                if (next >= 0) Select(next);
                return 0;
            }
            break;
        case WM_SETFOCUS:
            if (visual_.focus == kPickPath) visual_.focus = kPickList;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_CLOSE:
            Cancel();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd_, &paint);
            Render();
            EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            if (edit_) {
                RemoveWindowSubclass(edit_, EditProc, 1);
                DestroyWindow(edit_);
                edit_ = nullptr;
            }
            if (font_) { DeleteObject(font_); font_ = nullptr; }
            if (edit_brush_) { DeleteObject(edit_brush_); edit_brush_ = nullptr; }
            compositor_.Shutdown();
            done_ = true;
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HWND edit_ = nullptr;
    HFONT font_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_{&compositor_};
    FolderPickerLayout layout_;
    FolderPickerVisual visual_;
    PickerHistory history_;
    std::unique_ptr<PickerLoader> loader_;
    std::wstring fallback_;
    std::wstring select_after_;
    std::wstring edit_text_;
    std::wstring result_;
    D2D1_COLOR_F accent_ = HexColor(0x0078D4);
    uint64_t generation_ = 0;
    ULONGLONG load_started_ = 0;
    size_t mode_index_ = 0;
    float scale_ = 1.0f;
    bool dark_ = false;
    bool backdrop_enabled_ = false;
    bool done_ = false;
    bool initial_load_ = false;
    bool current_verified_ = false;
    bool dragging_scrollbar_ = false;
};

} // namespace

bool ShowFolderPicker(HWND owner, const FolderPickerSpec& spec, bool dark,
                      D2D1_COLOR_F accent, std::wstring& path) {
    FolderPickerWindow window;
    return window.Show(owner, spec, dark, accent, path);
}

} // namespace pulse::ui
