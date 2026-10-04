#include "text_diff_window.h"

#include "../common/localization.h"
#include "../common/windows_compat.h"
#include "typography.h"

#include <dwmapi.h>
#include <imm.h>
#include <windowsx.h>

#pragma comment(lib, "imm32.lib")

#include <algorithm>
#include <cmath>

namespace pulse::ui {
namespace {

constexpr wchar_t kClassName[] = L"PulseTextDiffWindow";
constexpr UINT kMsgJobDone = WM_APP + 41;
constexpr UINT_PTR kTooltipTimer = 1;
constexpr float kToolbarH = 48.0f;
constexpr float kHeaderH = 46.0f;
constexpr float kStatusH = 26.0f;
constexpr float kMinimapW = 14.0f;
constexpr int kContextLines = 2;
constexpr int kMinFoldLines = 3;
constexpr size_t kMaxDrawChars = 4000;

using Sid = pulse::l10n::StringId;

std::wstring Str(Sid id) { return std::wstring(pulse::l10n::Get(id)); }

std::wstring Fill(std::wstring text, std::wstring_view key, const std::wstring& value) {
    const size_t at = text.find(key);
    if (at != std::wstring::npos) text.replace(at, key.size(), value);
    return text;
}

bool DiffHighContrast() {
    HIGHCONTRASTW value{sizeof(value)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(value), &value, 0) &&
           (value.dwFlags & HCF_HIGHCONTRASTON);
}

struct Palette {
    D2D1_COLOR_F panel, body, add_bg, add_hi, del_bg, del_hi, mod_bg, add_fg, del_fg, mod_fg,
        bar, fold, fold_hover, select;
};

Palette MakePalette(bool dark, const D2D1_COLOR_F& accent) {
    Palette p{};
    if (dark) {
        p.panel = D2D1::ColorF(0x202020);
        p.body = D2D1::ColorF(0x191919);
        p.add_bg = D2D1::ColorF(0x2EA043, 0.16f);
        p.add_hi = D2D1::ColorF(0x2EA043, 0.46f);
        p.del_bg = D2D1::ColorF(0xF85149, 0.15f);
        p.del_hi = D2D1::ColorF(0xF85149, 0.44f);
        p.mod_bg = D2D1::ColorF(0xD29922, 0.14f);
        p.add_fg = D2D1::ColorF(0x3FB950);
        p.del_fg = D2D1::ColorF(0xF85149);
        p.mod_fg = D2D1::ColorF(0xD29922);
        p.bar = D2D1::ColorF(0xE3A008);
        p.fold = D2D1::ColorF(0xFFFFFF, 0.04f);
        p.fold_hover = D2D1::ColorF(0xFFFFFF, 0.09f);
    } else {
        p.panel = D2D1::ColorF(0xF3F3F3);
        p.body = D2D1::ColorF(0xFFFFFF);
        p.add_bg = D2D1::ColorF(0x2DA44E, 0.13f);
        p.add_hi = D2D1::ColorF(0x2DA44E, 0.36f);
        p.del_bg = D2D1::ColorF(0xCF222E, 0.10f);
        p.del_hi = D2D1::ColorF(0xCF222E, 0.30f);
        p.mod_bg = D2D1::ColorF(0xBF8700, 0.12f);
        p.add_fg = D2D1::ColorF(0x1A7F37);
        p.del_fg = D2D1::ColorF(0xCF222E);
        p.mod_fg = D2D1::ColorF(0x9A6700);
        p.bar = D2D1::ColorF(0xBF8700);
        p.fold = D2D1::ColorF(0x000000, 0.035f);
        p.fold_hover = D2D1::ColorF(0x000000, 0.07f);
    }
    p.select = D2D1::ColorF(accent.r, accent.g, accent.b, dark ? 0.26f : 0.18f);
    return p;
}

std::wstring FormatFileTime(uint64_t value) {
    if (!value) return {};
    FILETIME ft{static_cast<DWORD>(value & 0xFFFFFFFFu), static_cast<DWORD>(value >> 32)};
    SYSTEMTIME utc{}, local{};
    if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
        return {};
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u", local.wYear, local.wMonth, local.wDay,
               local.wHour, local.wMinute);
    return buffer;
}

int DisplayColumns(const std::wstring& line) {
    int cols = 0;
    const size_t n = (std::min)(line.size(), kMaxDrawChars);
    for (size_t i = 0; i < n; ++i) cols += line[i] >= 0x1100 ? 2 : 1;
    return cols;
}

bool Inside(const D2D1_RECT_F& rc, float x, float y) {
    return x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom;
}

} // namespace

TextDiffWindow::~TextDiffWindow() {
    StopJob();
    if (hwnd_ && IsWindow(hwnd_)) {
        compositor_.Shutdown();
        DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;
}

bool TextDiffWindow::visible() const noexcept {
    return hwnd_ && IsWindowVisible(hwnd_);
}

Theme TextDiffWindow::CurrentTheme() const {
    return DiffHighContrast() ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
}

bool TextDiffWindow::EnsureWindow(HWND owner) {
    if (hwnd_) return true;
    static bool registered = false;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &TextDiffWindow::WndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        registered = true;
    }
    const UINT dpi = owner ? pulse::compat::WindowDpi(owner) : 96u;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT work = mi.rcWork;
    const int work_w = work.right - work.left;
    const int work_h = work.bottom - work.top;
    const int width = (std::min)(MulDiv(1180, static_cast<int>(dpi), 96), work_w * 9 / 10);
    const int height = (std::min)(MulDiv(760, static_cast<int>(dpi), 96), work_h * 9 / 10);
    const int x = work.left + (work_w - width) / 2;
    const int y = work.top + (work_h - height) / 2;
    CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kClassName, Str(Sid::DiffTitle).c_str(),
                    WS_OVERLAPPEDWINDOW, x, y, width, height, nullptr, nullptr, instance, this);
    if (!hwnd_) return false;
    if (owner) {
        if (const auto big = reinterpret_cast<HICON>(GetClassLongPtrW(owner, GCLP_HICON)))
            SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
        if (const auto icon_sm = reinterpret_cast<HICON>(GetClassLongPtrW(owner, GCLP_HICONSM)))
            SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon_sm));
    }
    return true;
}

void TextDiffWindow::Show(HWND owner, const std::wstring& left, const std::wstring& right,
                          bool dark, D2D1_COLOR_F accent) {
    dark_ = dark;
    accent_ = accent;
    if (!EnsureWindow(owner)) return;
    const BOOL immersive_dark = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &immersive_dark,
                          sizeof(immersive_dark));
    path_[0] = left;
    path_[1] = right;
    for (int i = 0; i < 2; ++i) {
        FileInfo& fi = info_[i];
        fi = FileInfo{};
        std::wstring shown = path_[i];
        if (shown.rfind(L"\\\\?\\UNC\\", 0) == 0) shown = L"\\\\" + shown.substr(8);
        else if (shown.rfind(L"\\\\?\\", 0) == 0) shown = shown.substr(4);
        const size_t slash = shown.find_last_of(L"\\/");
        fi.name = slash == std::wstring::npos ? shown : shown.substr(slash + 1);
        fi.folder = slash == std::wstring::npos ? std::wstring() : shown.substr(0, slash);
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (GetFileAttributesExW(path_[i].c_str(), GetFileExInfoStandard, &data)) {
            fi.mtime = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                       data.ftLastWriteTime.dwLowDateTime;
            fi.time = FormatFileTime(fi.mtime);
        }
    }
    left_.reset();
    right_.reset();
    result_ = diff::DiffResult{};
    view_.clear();
    row_to_view_.clear();
    expanded_folds_.clear();
    spans_.clear();
    scroll_x_ = scroll_y_ = 0.0f;
    current_hunk_ = -1;
    sel_anchor_ = sel_end_ = -1;
    first_result_ = true;
    UpdateTitle();
    StartJob(true);
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void TextDiffWindow::Close() {
    StopJob();
    computing_ = false;
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
    left_.reset();
    right_.reset();
    result_ = diff::DiffResult{};
    view_.clear();
    row_to_view_.clear();
    spans_.clear();
}

void TextDiffWindow::UpdateTitle() {
    if (!hwnd_) return;
    const std::wstring title = Str(Sid::DiffTitle) + L" \x2014 " + info_[0].name + L" \x2194 " +
                               info_[1].name;
    SetWindowTextW(hwnd_, title.c_str());
}

void TextDiffWindow::StopJob() {
    cancel_.store(true);
    if (worker_.joinable()) worker_.join();
    cancel_.store(false);
    std::lock_guard<std::mutex> lock(result_mutex_);
    pending_.reset();
}

void TextDiffWindow::StartJob(bool reload) {
    StopJob();
    const uint64_t generation = ++generation_;
    computing_ = true;
    spans_.clear();
    const HWND target_hwnd = hwnd_;
    const std::wstring left_path = path_[0];
    const std::wstring right_path = path_[1];
    const diff::DiffOptions options = options_;
    std::shared_ptr<const diff::DiffSide> left = reload ? nullptr : left_;
    std::shared_ptr<const diff::DiffSide> right = reload ? nullptr : right_;
    worker_ = std::thread([this, target_hwnd, generation, left_path, right_path, options, left,
                           right]() mutable {
        auto job = std::make_unique<JobResult>();
        job->generation = generation;
        if (!left) {
            auto side = std::make_shared<diff::DiffSide>();
            diff::LoadDiffSide(left_path, *side);
            left = std::move(side);
        }
        if (cancel_.load()) return;
        if (!right) {
            auto side = std::make_shared<diff::DiffSide>();
            diff::LoadDiffSide(right_path, *side);
            right = std::move(side);
        }
        if (cancel_.load()) return;
        if (left->status == diff::LoadStatus::Ok && right->status == diff::LoadStatus::Ok) {
            if (!diff::ComputeLineDiff(left->lines, right->lines, options, job->result, &cancel_))
                return;
        }
        job->left = std::move(left);
        job->right = std::move(right);
        {
            std::lock_guard<std::mutex> lock(result_mutex_);
            pending_ = std::move(job);
        }
        PostMessageW(target_hwnd, kMsgJobDone, 0, 0);
    });
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void TextDiffWindow::AdoptResult() {
    std::unique_ptr<JobResult> job;
    {
        std::lock_guard<std::mutex> lock(result_mutex_);
        job = std::move(pending_);
    }
    if (!job || job->generation != generation_) return;
    if (worker_.joinable()) worker_.join();
    {
        RECT client{};
        GetClientRect(hwnd_, &client);
        compositor_.Resize((std::max)(1L, client.right), (std::max)(1L, client.bottom));
        Layout();
    }
    computing_ = false;
    left_ = std::move(job->left);
    right_ = std::move(job->right);
    result_ = std::move(job->result);
    spans_.clear();
    max_cols_ = 0;
    size_t most_lines = 1;
    for (const auto* side : {left_.get(), right_.get()}) {
        if (!side) continue;
        most_lines = (std::max)(most_lines, side->lines.size());
        for (const auto& line : side->lines) max_cols_ = (std::max)(max_cols_, DisplayColumns(line));
    }
    number_digits_ = 1;
    for (size_t v = most_lines; v >= 10; v /= 10) ++number_digits_;
    const int hunks = static_cast<int>(result_.hunks.size());
    RebuildView();
    if (first_result_) {
        first_result_ = false;
        scroll_y_ = 0.0f;
        current_hunk_ = -1;
        if (hunks > 0) GoHunk(0);
    } else {
        current_hunk_ = hunks == 0 ? -1 : std::clamp(current_hunk_, 0, hunks - 1);
        ScrollTo(scroll_y_);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void TextDiffWindow::RebuildView() {
    // Keep the top visible DiffRow in place across mode / fold changes.
    int anchor_row = -1;
    if (!view_.empty()) {
        const int top = std::clamp(static_cast<int>(scroll_y_ / RowHeight()), 0,
                                   static_cast<int>(view_.size()) - 1);
        const ViewRow& vr = view_[static_cast<size_t>(top)];
        anchor_row = vr.row >= 0 ? vr.row : vr.fold_first;
    }
    view_.clear();
    const auto& rows = result_.rows;
    const int n = static_cast<int>(rows.size());
    row_to_view_.assign(static_cast<size_t>(n), -1);
    if (n == 0) {
        scroll_y_ = 0.0f;
        return;
    }
    std::vector<uint8_t> keep(static_cast<size_t>(n), only_changes_ ? 0 : 1);
    if (only_changes_) {
        for (const auto& h : result_.hunks) {
            for (int r = (std::max)(0, h.first - kContextLines);
                 r <= (std::min)(n - 1, h.last + kContextLines); ++r)
                keep[static_cast<size_t>(r)] = 1;
        }
    }
    auto map_row = [&](int r) {
        if (row_to_view_[static_cast<size_t>(r)] < 0)
            row_to_view_[static_cast<size_t>(r)] = static_cast<int>(view_.size()) - 1;
    };
    int r = 0;
    while (r < n) {
        const diff::DiffRow& row = rows[static_cast<size_t>(r)];
        if (row.kind == diff::RowKind::Same) {
            if (keep[static_cast<size_t>(r)]) {
                view_.push_back({r, static_cast<int8_t>(unified_ ? 2 : -1), -1, 0});
                map_row(r);
                ++r;
                continue;
            }
            int end = r;
            while (end < n && rows[static_cast<size_t>(end)].kind == diff::RowKind::Same &&
                   !keep[static_cast<size_t>(end)])
                ++end;
            const int count = end - r;
            if (count >= kMinFoldLines && !expanded_folds_.count(r)) {
                view_.push_back({-1, -1, r, count});
                for (int q = r; q < end; ++q) map_row(q);
            } else {
                for (int q = r; q < end; ++q) {
                    view_.push_back({q, static_cast<int8_t>(unified_ ? 2 : -1), -1, 0});
                    map_row(q);
                }
            }
            r = end;
            continue;
        }
        int end = r;
        while (end < n && rows[static_cast<size_t>(end)].kind != diff::RowKind::Same) ++end;
        if (unified_) {
            for (int q = r; q < end; ++q) {
                if (rows[static_cast<size_t>(q)].left < 0) continue;
                view_.push_back({q, 0, -1, 0});
                map_row(q);
            }
            for (int q = r; q < end; ++q) {
                if (rows[static_cast<size_t>(q)].right < 0) continue;
                view_.push_back({q, 1, -1, 0});
                map_row(q);
            }
        } else {
            for (int q = r; q < end; ++q) {
                view_.push_back({q, -1, -1, 0});
                map_row(q);
            }
        }
        r = end;
    }
    if (anchor_row >= 0 && anchor_row < n)
        scroll_y_ = static_cast<float>(row_to_view_[static_cast<size_t>(anchor_row)]) * RowHeight();
    sel_anchor_ = sel_end_ = -1;
    ScrollTo(scroll_y_);
}

void TextDiffWindow::RecreateFormats() {
    auto* factory = compositor_.DwriteFactory();
    if (!factory) return;
    mono_.reset();
    gutter_.reset();
    ui_.reset();
    ui_bold_.reset();
    small_.reset();
    hatch_.reset();
    const wchar_t* mono_family = L"Consolas";
    {
        ComPtr<IDWriteFontCollection> fonts;
        if (SUCCEEDED(factory->GetSystemFontCollection(&fonts, FALSE)) && fonts.get()) {
            UINT32 index = 0;
            BOOL exists = FALSE;
            if (SUCCEEDED(fonts->FindFamilyName(L"Cascadia Mono", &index, &exists)) && exists)
                mono_family = L"Cascadia Mono";
        }
    }
    std::wstring ui_family = L"Segoe UI";
    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = L"";
    if (IDWriteTextFormat* base = compositor_.TextFormat()) {
        const UINT32 len = base->GetFontFamilyNameLength();
        std::wstring family(len + 1, L'\0');
        if (SUCCEEDED(base->GetFontFamilyName(family.data(), len + 1))) {
            family.resize(len);
            if (!family.empty()) ui_family = family;
        }
        base->GetLocaleName(locale, LOCALE_NAME_MAX_LENGTH);
    }
    auto make = [&](const wchar_t* family, DWRITE_FONT_WEIGHT weight, float dip,
                    DWRITE_TEXT_ALIGNMENT align, ComPtr<IDWriteTextFormat>& out) {
        if (FAILED(factory->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                             DWRITE_FONT_STRETCH_NORMAL,
                                             dip * scale_ * typography::UiFontScale(), locale,
                                             &out)) || !out.get())
            return;
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        out->SetTextAlignment(align);
    };
    make(mono_family, DWRITE_FONT_WEIGHT_NORMAL, 13.0f, DWRITE_TEXT_ALIGNMENT_LEADING, mono_);
    make(mono_family, DWRITE_FONT_WEIGHT_NORMAL, 11.5f, DWRITE_TEXT_ALIGNMENT_TRAILING, gutter_);
    make(ui_family.c_str(), DWRITE_FONT_WEIGHT_NORMAL, 13.0f, DWRITE_TEXT_ALIGNMENT_LEADING, ui_);
    make(ui_family.c_str(), DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.5f, DWRITE_TEXT_ALIGNMENT_LEADING,
         ui_bold_);
    make(ui_family.c_str(), DWRITE_FONT_WEIGHT_NORMAL, 11.5f, DWRITE_TEXT_ALIGNMENT_LEADING,
         small_);
    char_w_ = mono_.get() ? Measure(L"0000000000", mono_.get()) / 10.0f : 8.0f * scale_;
    if (char_w_ <= 0.0f) char_w_ = 8.0f * scale_;
}

float TextDiffWindow::Measure(std::wstring_view text, IDWriteTextFormat* format) const {
    auto* factory = compositor_.DwriteFactory();
    if (!factory || !format || text.empty()) return 0.0f;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format,
                                         100000.0f, 1000.0f, &layout)) || !layout.get())
        return 0.0f;
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return metrics.widthIncludingTrailingWhitespace;
}

void TextDiffWindow::EnsureHatch() {
    if (hatch_.get() && hatch_dark_ == dark_) return;
    hatch_.reset();
    auto* dc = compositor_.Dc();
    if (!dc) return;
    const float size = std::round(8.0f * scale_);
    ComPtr<ID2D1BitmapRenderTarget> target;
    if (FAILED(dc->CreateCompatibleRenderTarget(D2D1::SizeF(size, size), &target)) ||
        !target.get())
        return;
    ComPtr<ID2D1SolidColorBrush> ink;
    target->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0xFFFFFF, 0.10f)
                                        : D2D1::ColorF(0x000000, 0.09f), &ink);
    target->BeginDraw();
    target->Clear(D2D1::ColorF(0, 0.0f));
    if (ink.get()) {
        const float w = (std::max)(1.0f, scale_);
        target->DrawLine(D2D1::Point2F(-1.0f, size + 1.0f), D2D1::Point2F(size + 1.0f, -1.0f),
                         ink.get(), w);
        target->DrawLine(D2D1::Point2F(-1.0f, 1.0f), D2D1::Point2F(1.0f, -1.0f), ink.get(), w);
        target->DrawLine(D2D1::Point2F(size - 1.0f, size + 1.0f),
                         D2D1::Point2F(size + 1.0f, size - 1.0f), ink.get(), w);
    }
    if (FAILED(target->EndDraw())) return;
    ComPtr<ID2D1Bitmap> bitmap;
    if (FAILED(target->GetBitmap(&bitmap)) || !bitmap.get()) return;
    dc->CreateBitmapBrush(bitmap.get(),
                          D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP),
                          &hatch_);
    hatch_dark_ = dark_;
}

void TextDiffWindow::Layout() {
    const float s = scale_;
    const float w = static_cast<float>(compositor_.Width());
    const float h = static_cast<float>(compositor_.Height());
    toolbar_ = D2D1::RectF(0, 0, w, kToolbarH * s);
    header_ = D2D1::RectF(0, toolbar_.bottom, w, toolbar_.bottom + kHeaderH * s);
    status_ = D2D1::RectF(0, (std::max)(header_.bottom, h - kStatusH * s), w, h);
    body_ = D2D1::RectF(0, header_.bottom, (std::max)(0.0f, w - kMinimapW * s), status_.top);
    minimap_ = D2D1::RectF(body_.right, header_.bottom, w, status_.top);

    const float top = 9.0f * s;
    const float bottom = top + 30.0f * s;
    float x = 12.0f * s;
    const float seg_pad = 26.0f * s;
    const float w_split = Measure(Str(Sid::DiffSideBySide), ui_.get()) + seg_pad;
    const float w_unified = Measure(Str(Sid::DiffUnified), ui_.get()) + seg_pad;
    mode_split_ = D2D1::RectF(x, top, x + w_split, bottom);
    x += w_split;
    mode_unified_ = D2D1::RectF(x, top, x + w_unified, bottom);
    x += w_unified + 18.0f * s;
    auto check_rect = [&](Sid id) {
        const float cw = Measure(Str(id), ui_.get()) + 34.0f * s;
        const D2D1_RECT_F rc = D2D1::RectF(x, top, x + cw, bottom);
        x += cw + 8.0f * s;
        return rc;
    };
    only_changes_rc_ = check_rect(Sid::DiffOnlyChanges);
    ignore_ws_rc_ = check_rect(Sid::DiffIgnoreWs);
    ignore_case_rc_ = check_rect(Sid::DiffIgnoreCase);
    x += 4.0f * s;
    const float sw = Measure(Str(Sid::DiffSwap), ui_.get()) + 50.0f * s;
    swap_rc_ = D2D1::RectF(x, top, x + sw, bottom);

    float right = w - 12.0f * s;
    next_rc_ = D2D1::RectF(right - 32.0f * s, top, right, bottom);
    right = next_rc_.left - 4.0f * s;
    prev_rc_ = D2D1::RectF(right - 32.0f * s, top, right, bottom);
    right = prev_rc_.left - 6.0f * s;
    const float pw = Measure(Fill(Fill(Str(Sid::DiffHunkPos), L"{i}", L"888"), L"{n}", L"888"),
                             ui_.get()) + 12.0f * s;
    position_rc_ = D2D1::RectF(right - pw, top, right, bottom);
    right = position_rc_.left - 8.0f * s;
    summary_rc_ = D2D1::RectF((std::max)(swap_rc_.right + 8.0f * s, right - 190.0f * s), top,
                              right, bottom);
}

TextDiffWindow::Hit TextDiffWindow::HitTest(float x, float y, int* view_row) const {
    if (view_row) *view_row = -1;
    if (Inside(toolbar_, x, y)) {
        if (Inside(mode_split_, x, y)) return Hit::ModeSplit;
        if (Inside(mode_unified_, x, y)) return Hit::ModeUnified;
        if (Inside(only_changes_rc_, x, y)) return Hit::OnlyChanges;
        if (Inside(ignore_ws_rc_, x, y)) return Hit::IgnoreWs;
        if (Inside(ignore_case_rc_, x, y)) return Hit::IgnoreCase;
        if (Inside(swap_rc_, x, y)) return Hit::Swap;
        if (Inside(prev_rc_, x, y)) return Hit::Prev;
        if (Inside(next_rc_, x, y)) return Hit::Next;
        return Hit::None;
    }
    if (!Ready()) return Hit::None;
    if (Inside(minimap_, x, y)) return Hit::Minimap;
    if (Inside(body_, x, y)) {
        const int row = static_cast<int>((y - body_.top + scroll_y_) / RowHeight());
        if (row >= 0 && row < static_cast<int>(view_.size())) {
            if (view_row) *view_row = row;
            return Hit::Body;
        }
    }
    return Hit::None;
}

void TextDiffWindow::ScrollTo(float y) {
    const float body_h = body_.bottom - body_.top;
    const float max_y = (std::max)(0.0f, ContentHeight() - body_h + RowHeight());
    scroll_y_ = std::clamp(y, 0.0f, max_y);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void TextDiffWindow::ScrollX(float dx) {
    const float body_w = body_.right - body_.left;
    const float gutter = static_cast<float>(number_digits_) * char_w_ + 20.0f * scale_;
    const float text_w = unified_ ? body_w - gutter * 2.0f - 24.0f * scale_
                                  : body_w / 2.0f - gutter - 16.0f * scale_;
    const float max_x = (std::max)(0.0f, static_cast<float>(max_cols_) * char_w_ -
                                             text_w + 24.0f * scale_);
    scroll_x_ = std::clamp(scroll_x_ + dx, 0.0f, max_x);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void TextDiffWindow::GoHunk(int index) {
    const int count = static_cast<int>(result_.hunks.size());
    if (count == 0 || view_.empty()) return;
    current_hunk_ = std::clamp(index, 0, count - 1);
    const int first = result_.hunks[static_cast<size_t>(current_hunk_)].first;
    const int v = row_to_view_[static_cast<size_t>(first)];
    const float body_h = body_.bottom - body_.top;
    ScrollTo(static_cast<float>(v) * RowHeight() - body_h / 3.0f);
}

void TextDiffWindow::StepHunk(int delta) {
    const int count = static_cast<int>(result_.hunks.size());
    if (count == 0) return;
    if (current_hunk_ < 0) {
        GoHunk(delta > 0 ? 0 : count - 1);
        return;
    }
    const int next = current_hunk_ + delta;
    if (next < 0 || next >= count) {
        MessageBeep(MB_OK);
        return;
    }
    GoHunk(next);
}

const TextDiffWindow::CharSpans& TextDiffWindow::SpansFor(int row) {
    auto it = spans_.find(row);
    if (it != spans_.end()) return it->second;
    CharSpans spans;
    const diff::DiffRow& r = result_.rows[static_cast<size_t>(row)];
    if (r.kind == diff::RowKind::Mod && left_ && right_) {
        diff::ComputeCharDiff(left_->lines[static_cast<size_t>(r.left)],
                              right_->lines[static_cast<size_t>(r.right)], options_, spans.left,
                              spans.right);
    }
    return spans_.emplace(row, std::move(spans)).first->second;
}

void TextDiffWindow::OnClick(float x, float y, bool shift) {
    int view_row = -1;
    const Hit hit = HitTest(x, y, &view_row);
    switch (hit) {
    case Hit::ModeSplit:
    case Hit::ModeUnified: {
        const bool unified = hit == Hit::ModeUnified;
        if (unified != unified_) {
            unified_ = unified;
            scroll_x_ = 0.0f;
            RebuildView();
        }
        break;
    }
    case Hit::OnlyChanges:
        only_changes_ = !only_changes_;
        expanded_folds_.clear();
        RebuildView();
        break;
    case Hit::IgnoreWs:
        options_.ignore_whitespace = !options_.ignore_whitespace;
        if (left_ && right_) StartJob(false);
        break;
    case Hit::IgnoreCase:
        options_.ignore_case = !options_.ignore_case;
        if (left_ && right_) StartJob(false);
        break;
    case Hit::Swap:
        std::swap(path_[0], path_[1]);
        std::swap(info_[0], info_[1]);
        std::swap(left_, right_);
        UpdateTitle();
        if (left_ && right_) StartJob(false);
        break;
    case Hit::Prev: StepHunk(-1); break;
    case Hit::Next: StepHunk(1); break;
    case Hit::Minimap: {
        minimap_drag_ = true;
        SetCapture(hwnd_);
        const float h = minimap_.bottom - minimap_.top;
        const float frac = h > 0 ? (y - minimap_.top) / h : 0.0f;
        ScrollTo(frac * ContentHeight() - (body_.bottom - body_.top) / 2.0f);
        break;
    }
    case Hit::Body: {
        const ViewRow& vr = view_[static_cast<size_t>(view_row)];
        if (vr.row < 0) {
            expanded_folds_.insert(vr.fold_first);
            RebuildView();
            break;
        }
        if (shift && sel_anchor_ >= 0) {
            sel_end_ = view_row;
        } else {
            sel_anchor_ = sel_end_ = view_row;
        }
        for (int i = 0; i < static_cast<int>(result_.hunks.size()); ++i) {
            const auto& hk = result_.hunks[static_cast<size_t>(i)];
            if (vr.row >= hk.first && vr.row <= hk.last) current_hunk_ = i;
        }
        break;
    }
    default: break;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void TextDiffWindow::CopySelection() {
    if (sel_anchor_ < 0 || !left_ || !right_) return;
    const int a = (std::min)(sel_anchor_, sel_end_);
    const int b = (std::max)(sel_anchor_, sel_end_);
    std::wstring text;
    for (int i = a; i <= b && i < static_cast<int>(view_.size()); ++i) {
        const ViewRow& vr = view_[static_cast<size_t>(i)];
        if (vr.row < 0) continue;
        const diff::DiffRow& r = result_.rows[static_cast<size_t>(vr.row)];
        auto left_line = [&] { return left_->lines[static_cast<size_t>(r.left)]; };
        auto right_line = [&] { return right_->lines[static_cast<size_t>(r.right)]; };
        if (r.kind == diff::RowKind::Same) {
            text += L"  " + left_line() + L"\r\n";
        } else if (unified_) {
            text += vr.side == 0 ? L"- " + left_line() : L"+ " + right_line();
            text += L"\r\n";
        } else {
            if (r.left >= 0) text += L"- " + left_line() + L"\r\n";
            if (r.right >= 0) text += L"+ " + right_line() + L"\r\n";
        }
    }
    if (text.empty() || !OpenClipboard(hwnd_)) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* dst = GlobalLock(mem)) {
            memcpy(dst, text.c_str(), bytes);
            GlobalUnlock(mem);
            if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
}

void TextDiffWindow::Render() {
    if (!compositor_.Dc()) return;
    if (compositor_.NeedsRecovery()) {
        if (!compositor_.Recover()) return;
        compositor_.RecreateTextFormats(scale_);
        brush_.reset();
        hatch_.reset();
    }
    RECT client{};
    GetClientRect(hwnd_, &client);
    compositor_.Resize((std::max)(1L, client.right), (std::max)(1L, client.bottom));
    Layout();
    auto* dc = compositor_.Dc();
    auto* factory = compositor_.DwriteFactory();
    const bool hc = DiffHighContrast();
    const Theme theme = CurrentTheme();
    Palette pal = MakePalette(dark_, accent_);
    if (hc) {
        pal.panel = pal.body = theme.bg;
    }
    const float s = scale_;
    const float w = static_cast<float>(compositor_.Width());
    dc->BeginDraw();
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    dc->Clear(pal.body);
    if (!brush_.get()) dc->CreateSolidColorBrush(theme.text, &brush_);
    EnsureHatch();
    painter_.SetCompositor(&compositor_);
    painter_.SetScale(s);
    const bool painter_ok = painter_.BeginFrame(theme, hc);
    auto fill = [&](const D2D1_RECT_F& rc, const D2D1_COLOR_F& color) {
        brush_->SetColor(color);
        dc->FillRectangle(rc, brush_.get());
    };
    auto text_at = [&](std::wstring_view text, const D2D1_RECT_F& rc, IDWriteTextFormat* format,
                       const D2D1_COLOR_F& color) {
        if (!format || text.empty()) return;
        brush_->SetColor(color);
        dc->DrawTextW(text.data(), static_cast<UINT32>(text.size()), format, rc, brush_.get(),
                      D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
    };
    auto hline = [&](float y, float x0, float x1) {
        fill(D2D1::RectF(x0, y, x1, y + (std::max)(1.0f, std::floor(s))), theme.stroke_divider);
    };

    // ---- Toolbar ----
    fill(toolbar_, pal.panel);
    fill(header_, pal.panel);
    fill(status_, pal.panel);
    if (painter_ok) {
        const D2D1_RECT_F track = D2D1::RectF(mode_split_.left, mode_split_.top,
                                              mode_unified_.right, mode_unified_.bottom);
        painter_.DrawSegmentedTrack(track);
        fluent::SegmentedItemSpec seg{};
        const std::wstring split_text = Str(Sid::DiffSideBySide);
        const std::wstring unified_text = Str(Sid::DiffUnified);
        seg.bounds = mode_split_;
        seg.text = split_text;
        seg.shared_track = true;
        seg.position = fluent::SegmentPosition::First;
        seg.state.selected = seg.state.checked = !unified_;
        seg.state.hovered = hover_ == Hit::ModeSplit;
        painter_.DrawSegmentedItem(seg);
        seg.bounds = mode_unified_;
        seg.text = unified_text;
        seg.position = fluent::SegmentPosition::Last;
        seg.state.selected = seg.state.checked = unified_;
        seg.state.hovered = hover_ == Hit::ModeUnified;
        painter_.DrawSegmentedItem(seg);
        auto check = [&](const D2D1_RECT_F& rc, Sid id, bool on, Hit hit) {
            fluent::ControlState cs{};
            cs.checked = on;
            cs.hovered = hover_ == hit;
            cs.pressed = pressed_ == hit;
            painter_.DrawCheckBox(rc, Str(id), cs);
        };
        check(only_changes_rc_, Sid::DiffOnlyChanges, only_changes_, Hit::OnlyChanges);
        check(ignore_ws_rc_, Sid::DiffIgnoreWs, options_.ignore_whitespace, Hit::IgnoreWs);
        check(ignore_case_rc_, Sid::DiffIgnoreCase, options_.ignore_case, Hit::IgnoreCase);
        const std::wstring swap_text = Str(Sid::DiffSwap);
        fluent::ButtonSpec button{};
        button.bounds = swap_rc_;
        button.text = swap_text;
        button.glyph = L"\xE8AB";
        button.state.hovered = hover_ == Hit::Swap;
        button.state.pressed = pressed_ == Hit::Swap;
        painter_.DrawButton(button);
        const bool has_hunks = Ready() && !result_.hunks.empty();
        fluent::ButtonSpec nav{};
        nav.icon_only = true;
        nav.bounds = prev_rc_;
        nav.glyph = L"\xE70E";
        nav.state.enabled = has_hunks;
        nav.state.hovered = hover_ == Hit::Prev;
        nav.state.pressed = pressed_ == Hit::Prev;
        painter_.DrawButton(nav);
        nav.bounds = next_rc_;
        nav.glyph = L"\xE70D";
        nav.state.hovered = hover_ == Hit::Next;
        nav.state.pressed = pressed_ == Hit::Next;
        painter_.DrawButton(nav);
    }
    if (Ready()) {
        std::wstring position;
        if (result_.hunks.empty()) {
            position = Str(options_.ignore_whitespace || options_.ignore_case
                               ? Sid::DiffIdenticalOptions : Sid::DiffIdentical);
        } else {
            position = Fill(Fill(Str(Sid::DiffHunkPos), L"{i}",
                                 current_hunk_ >= 0 ? std::to_wstring(current_hunk_ + 1)
                                                    : std::wstring(L"-")),
                            L"{n}", std::to_wstring(result_.hunks.size()));
        }
        const float pw = Measure(position, ui_.get());
        D2D1_RECT_F prc = position_rc_;
        if (pw > prc.right - prc.left) prc.left = prc.right - pw;
        prc.left = (std::max)(prc.left, swap_rc_.right + 8.0f * s);
        text_at(position, D2D1::RectF((std::max)(prc.left, prc.right - pw), prc.top, prc.right,
                                      prc.bottom), ui_.get(),
                result_.hunks.empty() ? theme.text_secondary : theme.text);
        // Summary: +added  -deleted  ~modified (right aligned before the position text).
        const std::wstring parts[3] = {L"+" + std::to_wstring(result_.added),
                                       L"\x2212" + std::to_wstring(result_.deleted),
                                       L"~" + std::to_wstring(result_.modified)};
        const D2D1_COLOR_F colors[3] = {pal.add_fg, pal.del_fg, pal.mod_fg};
        float x = (std::max)(prc.left, prc.right - pw) - 14.0f * s;
        for (int i = 2; i >= 0; --i) {
            const float pw_i = Measure(parts[i], ui_bold_.get());
            x -= pw_i;
            if (x < summary_rc_.left) break;
            text_at(parts[i], D2D1::RectF(x, summary_rc_.top, x + pw_i + 2.0f * s,
                                          summary_rc_.bottom), ui_bold_.get(), colors[i]);
            x -= 10.0f * s;
        }
    }
    hline(toolbar_.bottom - 1.0f, 0, w);

    // ---- Header: name, newer tag, folder · time for each side ----
    const float half = (body_.right - body_.left) / 2.0f;
    const int newer = info_[0].mtime > info_[1].mtime + 20000000ull   ? 0
                      : info_[1].mtime > info_[0].mtime + 20000000ull ? 1
                                                                      : -1;
    for (int side = 0; side < 2; ++side) {
        const float x0 = body_.left + half * static_cast<float>(side) + 14.0f * s;
        const float x1 = body_.left + half * static_cast<float>(side + 1) - 10.0f * s;
        const FileInfo& fi = info_[side];
        const std::wstring sign = side == 0 ? L"\x2212 " : L"+ ";
        const float sign_w = Measure(sign, ui_bold_.get());
        text_at(sign, D2D1::RectF(x0, header_.top + 4.0f * s, x0 + sign_w + 2.0f, header_.top + 24.0f * s),
                ui_bold_.get(), side == 0 ? pal.del_fg : pal.add_fg);
        const float name_w = (std::min)(Measure(fi.name, ui_bold_.get()),
                                        (std::max)(0.0f, x1 - x0 - sign_w - 60.0f * s));
        const float nx = x0 + sign_w;
        text_at(fi.name, D2D1::RectF(nx, header_.top + 4.0f * s, nx + name_w, header_.top + 24.0f * s),
                ui_bold_.get(), theme.text);
        if (newer == side) {
            const std::wstring tag = Str(Sid::DiffNewer);
            const float tw = Measure(tag, small_.get()) + 12.0f * s;
            const D2D1_RECT_F pill = D2D1::RectF(nx + name_w + 8.0f * s, header_.top + 6.0f * s,
                                                 nx + name_w + 8.0f * s + tw,
                                                 header_.top + 22.0f * s);
            brush_->SetColor(D2D1::ColorF(accent_.r, accent_.g, accent_.b, 0.18f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(pill, 8.0f * s, 8.0f * s), brush_.get());
            text_at(tag, D2D1::RectF(pill.left + 6.0f * s, pill.top, pill.right, pill.bottom),
                    small_.get(), theme.accent_text);
        }
        std::wstring sub = fi.folder;
        if (!fi.time.empty()) sub += (sub.empty() ? L"" : L"  \x00B7  ") + fi.time;
        text_at(sub, D2D1::RectF(x0, header_.top + 24.0f * s, x1, header_.top + 42.0f * s),
                small_.get(), theme.text_secondary);
    }
    if (!unified_) fill(D2D1::RectF(body_.left + half, header_.top + 8.0f * s,
                                    body_.left + half + (std::max)(1.0f, std::floor(s)),
                                    header_.bottom - 8.0f * s), theme.stroke_divider);
    hline(header_.bottom - 1.0f, 0, w);

    // ---- Body ----
    auto centered = [&](const std::wstring& message, const D2D1_COLOR_F& color) {
        const float mw = Measure(message, ui_.get());
        const float cx = (body_.left + body_.right) / 2.0f;
        const float cy = (body_.top + body_.bottom) / 2.0f;
        text_at(message, D2D1::RectF(cx - mw / 2.0f, cy - 14.0f * s, cx + mw / 2.0f + 2.0f,
                                     cy + 14.0f * s), ui_.get(), color);
    };
    const float row_h = RowHeight();
    const float gutter = static_cast<float>(number_digits_) * char_w_ + 20.0f * s;
    if (computing_) {
        centered(Str(Sid::DiffComputing), theme.text_secondary);
    } else if (left_ && right_ && !Ready()) {
        const diff::DiffSide* bad = left_->status != diff::LoadStatus::Ok ? left_.get() : right_.get();
        const std::wstring& name = bad == left_.get() ? info_[0].name : info_[1].name;
        const Sid id = bad->status == diff::LoadStatus::Binary     ? Sid::DiffBinary
                       : bad->status == diff::LoadStatus::TooLarge ? Sid::DiffTooLarge
                                                                   : Sid::DiffReadFailed;
        centered(Fill(Str(id), L"{name}", name), theme.text_secondary);
    } else if (Ready()) {
        dc->PushAxisAlignedClip(body_, D2D1_ANTIALIAS_MODE_ALIASED);
        const int count = static_cast<int>(view_.size());
        const int first = (std::max)(0, static_cast<int>(scroll_y_ / row_h));
        const int page_rows = static_cast<int>((body_.bottom - body_.top) / row_h) + 2;
        const int last = (std::min)(count, first + page_rows);
        const int sel_a = sel_anchor_ < 0 ? -1 : (std::min)(sel_anchor_, sel_end_);
        const int sel_b = sel_anchor_ < 0 ? -1 : (std::max)(sel_anchor_, sel_end_);
        const diff::DiffHunk* hunk = current_hunk_ >= 0 && current_hunk_ <
            static_cast<int>(result_.hunks.size()) ? &result_.hunks[static_cast<size_t>(current_hunk_)] : nullptr;

        auto draw_text = [&](const std::wstring& line, float text_left, float clip_left,
                             float clip_right, float y, const std::vector<diff::CharSpan>* spans,
                             const D2D1_COLOR_F& hi) {
            if (!factory || !mono_.get() || clip_right <= clip_left) return;
            const UINT32 len = static_cast<UINT32>((std::min)(line.size(), kMaxDrawChars));
            if (len == 0) return;
            dc->PushAxisAlignedClip(D2D1::RectF(clip_left, y, clip_right, y + row_h),
                                    D2D1_ANTIALIAS_MODE_ALIASED);
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(factory->CreateTextLayout(line.data(), len, mono_.get(), 1.0e6f, row_h,
                                                    &layout)) && layout.get()) {
                const float ox = text_left - scroll_x_;
                if (spans) {
                    for (const auto& span : *spans) {
                        const int b = (std::min)(span.begin, static_cast<int>(len));
                        const int e = (std::min)(span.end, static_cast<int>(len));
                        if (e <= b) continue;
                        UINT32 hits = 0;
                        layout->HitTestTextRange(static_cast<UINT32>(b), static_cast<UINT32>(e - b),
                                                 ox, y, nullptr, 0, &hits);
                        if (hits == 0) continue;
                        std::vector<DWRITE_HIT_TEST_METRICS> metrics(hits);
                        if (FAILED(layout->HitTestTextRange(static_cast<UINT32>(b),
                                                            static_cast<UINT32>(e - b), ox, y,
                                                            metrics.data(), hits, &hits)))
                            continue;
                        brush_->SetColor(hi);
                        for (UINT32 m = 0; m < hits; ++m) {
                            const auto& hm = metrics[m];
                            const D2D1_RECT_F rc = D2D1::RectF(
                                hm.left, y + 2.0f * s,
                                hm.left + (std::max)(hm.width, 2.0f * s), y + row_h - 2.0f * s);
                            dc->FillRoundedRectangle(D2D1::RoundedRect(rc, 2.0f * s, 2.0f * s),
                                                     brush_.get());
                        }
                    }
                }
                brush_->SetColor(theme.text);
                dc->DrawTextLayout(D2D1::Point2F(ox, y), layout.get(), brush_.get(),
                                   D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
            dc->PopAxisAlignedClip();
        };
        auto number = [&](int line, float x0, float x1, float y) {
            if (line < 0) return;
            const std::wstring text = std::to_wstring(line + 1);
            text_at(text, D2D1::RectF(x0, y, x1, y + row_h), gutter_.get(), theme.text_secondary);
        };
        auto hatch = [&](const D2D1_RECT_F& rc) {
            if (hatch_.get()) dc->FillRectangle(rc, hatch_.get());
        };

        for (int i = first; i < last; ++i) {
            const ViewRow& vr = view_[static_cast<size_t>(i)];
            const float y = body_.top + static_cast<float>(i) * row_h - scroll_y_;
            const D2D1_RECT_F row_rc = D2D1::RectF(body_.left, y, body_.right, y + row_h);
            if (vr.row < 0) {
                fill(row_rc, hover_ == Hit::Body && hover_row_ == i ? pal.fold_hover : pal.fold);
                const std::wstring label = Fill(Str(Sid::DiffFolded), L"{n}",
                                                std::to_wstring(vr.fold_count));
                text_at(label, D2D1::RectF(body_.left + gutter + 10.0f * s, y, body_.right, y + row_h),
                        small_.get(), theme.text_secondary);
                continue;
            }
            const diff::DiffRow& r = result_.rows[static_cast<size_t>(vr.row)];
            const bool in_hunk = hunk && vr.row >= hunk->first && vr.row <= hunk->last;
            if (!unified_) {
                for (int side = 0; side < 2; ++side) {
                    const float x0 = body_.left + half * static_cast<float>(side);
                    const float x1 = x0 + half;
                    const D2D1_RECT_F cell = D2D1::RectF(x0, y, x1, y + row_h);
                    const int line = side == 0 ? r.left : r.right;
                    switch (r.kind) {
                    case diff::RowKind::Same: break;
                    case diff::RowKind::Mod: fill(cell, pal.mod_bg); break;
                    case diff::RowKind::Del:
                        if (side == 0) fill(cell, pal.del_bg); else hatch(cell);
                        break;
                    case diff::RowKind::Add:
                        if (side == 1) fill(cell, pal.add_bg); else hatch(cell);
                        break;
                    }
                    number(line, x0, x0 + gutter - 8.0f * s, y);
                    if (line < 0) continue;
                    const diff::DiffSide& ds = side == 0 ? *left_ : *right_;
                    const std::vector<diff::CharSpan>* spans = nullptr;
                    if (r.kind == diff::RowKind::Mod) {
                        const CharSpans& cs = SpansFor(vr.row);
                        spans = side == 0 ? &cs.left : &cs.right;
                    }
                    draw_text(ds.lines[static_cast<size_t>(line)], x0 + gutter + 6.0f * s,
                              x0 + gutter, x1 - 2.0f * s, y, spans,
                              side == 0 ? pal.del_hi : pal.add_hi);
                }
            } else {
                const bool left_part = vr.side == 0;
                const bool same = vr.side == 2;
                if (!same) fill(row_rc, left_part ? pal.del_bg : pal.add_bg);
                number(same || left_part ? r.left : -1, body_.left, body_.left + gutter - 8.0f * s, y);
                number(same || !left_part ? r.right : -1, body_.left + gutter,
                       body_.left + gutter * 2.0f - 8.0f * s, y);
                const float sign_x = body_.left + gutter * 2.0f + 2.0f * s;
                if (!same)
                    text_at(left_part ? L"\x2212" : L"+",
                            D2D1::RectF(sign_x, y, sign_x + 14.0f * s, y + row_h), mono_.get(),
                            left_part ? pal.del_fg : pal.add_fg);
                const int line = same || left_part ? r.left : r.right;
                const diff::DiffSide& ds = same || left_part ? *left_ : *right_;
                const std::vector<diff::CharSpan>* spans = nullptr;
                if (r.kind == diff::RowKind::Mod) {
                    const CharSpans& cs = SpansFor(vr.row);
                    spans = left_part ? &cs.left : &cs.right;
                }
                const float text_left = sign_x + 18.0f * s;
                draw_text(ds.lines[static_cast<size_t>(line)], text_left, text_left - 4.0f * s,
                          body_.right - 2.0f * s, y, spans, left_part ? pal.del_hi : pal.add_hi);
            }
            if (i >= sel_a && i <= sel_b) fill(row_rc, pal.select);
            if (in_hunk) fill(D2D1::RectF(body_.left, y, body_.left + 3.0f * s, y + row_h), pal.bar);
        }
        // Gutter separators and the centre divider.
        const float line_w = (std::max)(1.0f, std::floor(s));
        if (!unified_) {
            fill(D2D1::RectF(body_.left + half, body_.top, body_.left + half + line_w, body_.bottom),
                 theme.stroke_divider);
        }
        dc->PopAxisAlignedClip();
        if (view_.empty()) centered(Str(Sid::DiffIdentical), theme.text_secondary);

        // ---- Minimap ----
        fill(minimap_, pal.panel);
        fill(D2D1::RectF(minimap_.left, minimap_.top, minimap_.left + line_w, minimap_.bottom),
             theme.stroke_divider);
        const float mh = minimap_.bottom - minimap_.top;
        if (count > 0 && mh > 0) {
            const float per = mh / static_cast<float>(count);
            const float tick = (std::max)(2.0f * s, per);
            int last_px = -1000;
            int last_kind = -1;
            for (int i = 0; i < count; ++i) {
                const ViewRow& vr = view_[static_cast<size_t>(i)];
                if (vr.row < 0) continue;
                const diff::DiffRow& r = result_.rows[static_cast<size_t>(vr.row)];
                if (r.kind == diff::RowKind::Same) continue;
                int kind = static_cast<int>(r.kind);
                if (unified_) kind = vr.side == 0 ? static_cast<int>(diff::RowKind::Del)
                                                  : static_cast<int>(diff::RowKind::Add);
                const float y = minimap_.top + static_cast<float>(i) * per;
                const int px = static_cast<int>(y);
                if (px == last_px && kind == last_kind) continue;
                last_px = px;
                last_kind = kind;
                const D2D1_COLOR_F c = kind == static_cast<int>(diff::RowKind::Add)   ? pal.add_fg
                                       : kind == static_cast<int>(diff::RowKind::Del) ? pal.del_fg
                                                                                      : pal.mod_fg;
                fill(D2D1::RectF(minimap_.left + 3.0f * s, y, minimap_.right - 3.0f * s, y + tick), c);
            }
            const float content = ContentHeight();
            if (content > 0) {
                const float vy = minimap_.top + scroll_y_ / content * mh;
                const float vh = (std::min)(mh, (body_.bottom - body_.top) / content * mh);
                const D2D1_RECT_F viewport = D2D1::RectF(minimap_.left + line_w, vy, minimap_.right,
                                                         (std::min)(minimap_.bottom, vy + (std::max)(vh, 6.0f * s)));
                fill(viewport, dark_ ? D2D1::ColorF(0xFFFFFF, 0.16f) : D2D1::ColorF(0x000000, 0.12f));
                brush_->SetColor(dark_ ? D2D1::ColorF(0xFFFFFF, 0.45f) : D2D1::ColorF(0x000000, 0.35f));
                dc->DrawRectangle(D2D1::RectF(viewport.left + 0.5f, viewport.top + 0.5f,
                                              viewport.right - 0.5f, viewport.bottom - 0.5f),
                                  brush_.get(), (std::max)(1.0f, std::floor(s)));
            }
        }
    }
    hline(status_.top, 0, w);

    // ---- Status bar ----
    for (int side = 0; side < 2; ++side) {
        const diff::DiffSide* ds = side == 0 ? left_.get() : right_.get();
        if (!ds || ds->status != diff::LoadStatus::Ok) continue;
        std::wstring text = Fill(Str(Sid::DiffLines), L"{n}", std::to_wstring(ds->lines.size()));
        text += L"  \x00B7  " + ds->encoding;
        if (!ds->eol.empty()) text += L"  \x00B7  " + ds->eol;
        const float x0 = body_.left + half * static_cast<float>(side) + 14.0f * s;
        text_at(text, D2D1::RectF(x0, status_.top, x0 + half - 20.0f * s, status_.bottom),
                small_.get(), theme.text_secondary);
    }
    if (Ready() && result_.capped) {
        const std::wstring note = Str(Sid::DiffCapped);
        const float nw = Measure(note, small_.get());
        text_at(note, D2D1::RectF(w - 14.0f * s - nw, status_.top, w - 12.0f * s, status_.bottom),
                small_.get(), pal.mod_fg);
    }

    // ---- Tooltip for the hunk navigation buttons ----
    if (painter_ok && tooltip_visible_ && (hover_ == Hit::Prev || hover_ == Hit::Next)) {
        const std::wstring tip = Str(hover_ == Hit::Prev ? Sid::DiffPrevHunk : Sid::DiffNextHunk);
        const float tw = Measure(tip, small_.get()) + 24.0f * s;
        const D2D1_RECT_F anchor = hover_ == Hit::Prev ? prev_rc_ : next_rc_;
        float tx = (std::min)(anchor.right, w - 8.0f * s) - tw;
        tx = (std::max)(8.0f * s, tx);
        const D2D1_RECT_F tip_rc = D2D1::RectF(tx, anchor.bottom + 6.0f * s, tx + tw,
                                               anchor.bottom + 34.0f * s);
        painter_.DrawTooltip(tip_rc, tip);
    }

    const HRESULT hr = dc->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED ||
        hr == DXGI_ERROR_DEVICE_RESET)
        compositor_.NotifyDeviceLost(hr);
    else
        compositor_.Present();
}

LRESULT CALLBACK TextDiffWindow::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    TextDiffWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<TextDiffWindow*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<TextDiffWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->HandleMessage(message, wparam, lparam);
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT TextDiffWindow::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        scale_ = static_cast<float>(pulse::compat::WindowDpi(hwnd_)) / 96.0f;
        if (!compositor_.Init(hwnd_)) return -1;
        // No text input here: without this the IME turns Esc / Ctrl+C into VK_PROCESSKEY.
        ImmAssociateContext(hwnd_, nullptr);
        compositor_.RecreateTextFormats(scale_);
        RecreateFormats();
        return 0;
    case WM_SIZE:
        ScrollTo(scroll_y_);
        ScrollX(0.0f);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_MOVE:
        compositor_.UpdateTextRenderingParams(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
        return 0;
    case WM_DPICHANGED: {
        scale_ = static_cast<float>(HIWORD(wparam)) / 96.0f;
        compositor_.RecreateTextFormats(scale_);
        RecreateFormats();
        const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize.x = static_cast<LONG>(860.0f * scale_);
        info->ptMinTrackSize.y = static_cast<LONG>(420.0f * scale_);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd_, &paint);
        Render();
        EndPaint(hwnd_, &paint);
        return 0;
    }
    case kMsgJobDone:
        AdoptResult();
        return 0;
    case WM_MOUSEMOVE: {
        const float x = static_cast<float>(GET_X_LPARAM(lparam));
        const float y = static_cast<float>(GET_Y_LPARAM(lparam));
        if (minimap_drag_) {
            const float h = minimap_.bottom - minimap_.top;
            const float frac = h > 0 ? (y - minimap_.top) / h : 0.0f;
            ScrollTo(frac * ContentHeight() - (body_.bottom - body_.top) / 2.0f);
            return 0;
        }
        if (!tracking_leave_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
            tracking_leave_ = TrackMouseEvent(&tme) != FALSE;
        }
        int row = -1;
        const Hit hit = HitTest(x, y, &row);
        const bool fold_hover = hit == Hit::Body && row >= 0 &&
                                view_[static_cast<size_t>(row)].row < 0;
        const int hover_row = fold_hover ? row : -1;
        if (hit != hover_ || hover_row != hover_row_) {
            hover_ = hit;
            hover_row_ = hover_row;
            tooltip_visible_ = false;
            KillTimer(hwnd_, kTooltipTimer);
            if (hit == Hit::Prev || hit == Hit::Next) SetTimer(hwnd_, kTooltipTimer, 600, nullptr);
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_leave_ = false;
        if (hover_ != Hit::None) {
            hover_ = Hit::None;
            hover_row_ = -1;
            tooltip_visible_ = false;
            KillTimer(hwnd_, kTooltipTimer);
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    case WM_TIMER:
        if (wparam == kTooltipTimer) {
            KillTimer(hwnd_, kTooltipTimer);
            tooltip_visible_ = hover_ == Hit::Prev || hover_ == Hit::Next;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT) {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd_, &pt);
            int row = -1;
            const Hit hit = HitTest(static_cast<float>(pt.x), static_cast<float>(pt.y), &row);
            const bool hand = (hit != Hit::None && hit != Hit::Body) ||
                              (hit == Hit::Body && row >= 0 && view_[static_cast<size_t>(row)].row < 0);
            SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        SetFocus(hwnd_);
        const float x = static_cast<float>(GET_X_LPARAM(lparam));
        const float y = static_cast<float>(GET_Y_LPARAM(lparam));
        pressed_ = HitTest(x, y, nullptr);
        tooltip_visible_ = false;
        OnClick(x, y, (wparam & MK_SHIFT) != 0);
        return 0;
    }
    case WM_LBUTTONUP:
        pressed_ = Hit::None;
        if (minimap_drag_) {
            minimap_drag_ = false;
            ReleaseCapture();
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_CAPTURECHANGED:
        minimap_drag_ = false;
        return 0;
    case WM_MOUSEWHEEL: {
        const float steps = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
        if (GET_KEYSTATE_WPARAM(wparam) & MK_SHIFT)
            ScrollX(-steps * char_w_ * 8.0f);
        else
            ScrollBy(-steps * 3.0f * RowHeight());
        return 0;
    }
    case WM_MOUSEHWHEEL: {
        const float steps = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
        ScrollX(steps * char_w_ * 8.0f);
        return 0;
    }
    case WM_KEYDOWN: {
        const bool ctrl = GetKeyState(VK_CONTROL) < 0;
        const bool shift = GetKeyState(VK_SHIFT) < 0;
        const float page = (std::max)(RowHeight(), body_.bottom - body_.top - RowHeight() * 2.0f);
        switch (wparam) {
        case VK_F7: StepHunk(shift ? -1 : 1); return 0;
        case VK_ESCAPE: Close(); return 0;
        case VK_UP: ScrollBy(-RowHeight()); return 0;
        case VK_DOWN: ScrollBy(RowHeight()); return 0;
        case VK_PRIOR: ScrollBy(-page); return 0;
        case VK_NEXT: ScrollBy(page); return 0;
        case VK_HOME: ScrollTo(0.0f); return 0;
        case VK_END: ScrollTo(ContentHeight()); return 0;
        case VK_LEFT: ScrollX(-char_w_ * 4.0f); return 0;
        case VK_RIGHT: ScrollX(char_w_ * 4.0f); return 0;
        case 'C':
            if (ctrl) {
                CopySelection();
                return 0;
            }
            break;
        default: break;
        }
        break;
    }
    case WM_SYSCOMMAND:
        // A lone Alt tap would park the window in system-menu keyboard mode (there is no
        // menu bar), swallowing the next Esc / F7. Alt+Space still opens the system menu.
        if ((wparam & 0xFFF0) == SC_KEYMENU && lparam == 0) return 0;
        break;
    case WM_CLOSE:
        Close();
        return 0;
    default: break;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

} // namespace pulse::ui
