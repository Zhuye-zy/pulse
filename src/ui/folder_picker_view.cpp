#include "../common/windows_compat.h"
#include "folder_picker_view.h"

#include "../common/display_path.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "typography.h"
#include "window_helpers.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>

namespace pulse::ui {
namespace {

constexpr float kTitleBar = 40.0f;
constexpr float kToolbarTop = 46.0f;
constexpr float kToolbarH = 34.0f;
constexpr float kSidebarW = 192.0f;
constexpr float kPlaceH = 36.0f;
constexpr float kHeaderH = 32.0f;
constexpr float kRowH = 36.0f;
constexpr float kFooterH = 64.0f;
constexpr float kButtonMinW = 104.0f;

float Dip(float scale, float value) { return value * scale; }

// Fixed-width detail columns so dates never wrap; the name takes the rest.
// Narrow windows drop the type column first. Size only exists for pictures.
struct Columns {
    float left, name_right, modified_right, type_right, right;
};

Columns ColumnsFor(const D2D1_RECT_F& row, float scale, bool with_size) {
    const float left = row.left + Dip(scale, 12.0f);
    const float right = row.right - Dip(scale, 12.0f);
    const float size_w = with_size ? Dip(scale, 84.0f) : 0.0f;
    float type_w = Dip(scale, 76.0f);
    const float modified_w = Dip(scale, 136.0f);
    if (right - left - size_w - type_w - modified_w < Dip(scale, 180.0f)) type_w = 0.0f;
    const float type_right = right - size_w;
    const float modified_right = type_right - type_w;
    return {left, modified_right - modified_w, modified_right, type_right, right};
}

std::wstring TypeText(const PickerEntry& entry) {
    if (entry.kind == PickerEntryKind::Folder) return l10n::Get(l10n::StringId::TypeFolder);
    const size_t dot = entry.name.rfind(L'.');
    std::wstring ext = dot == std::wstring::npos ? L"" : entry.name.substr(dot + 1);
    for (wchar_t& ch : ext) ch = static_cast<wchar_t>(std::towupper(ch));
    return ext;
}

std::wstring DriveDetail(const PickerEntry& entry) {
    if (entry.size == 0) return L"";
    wchar_t text[128]{};
    const auto compact = format::ByteSizeStyle::Compact;
    swprintf_s(text, l10n::Get(l10n::StringId::PickerDriveFreeFormat).c_str(),
               format::ByteSize(entry.free, false, compact).c_str(),
               format::ByteSize(entry.size, false, compact).c_str());
    return text;
}

fluent::ControlState StateFor(const FolderPickerVisual& v, int id, bool enabled = true) {
    fluent::ControlState state{};
    state.enabled = enabled;
    state.hovered = enabled && v.hover == id;
    state.pressed = enabled && v.pressed == id;
    state.keyboard_focus = v.show_focus && v.focus == id;
    return state;
}

// High contrast highlights rows with COLOR_HIGHLIGHT, whose text colour is
// the window colour in every system contrast theme.
bool InverseRow(const fluent::ControlState& state, bool high_contrast) {
    return high_contrast && (state.selected || state.hovered || state.pressed);
}

// One-line text that ends in "…" instead of being clipped mid-glyph.
void DrawFittedText(Compositor& compositor, fluent::Painter& painter, const std::wstring& text,
                    const D2D1_RECT_F& rect, IDWriteTextFormat* format,
                    const D2D1_COLOR_F& color) {
    const std::wstring fitted = FitTextEnd(text, rect.right - rect.left,
        [&](std::wstring_view s) { return typography::MeasureLine(&compositor, format, s); });
    painter.DrawText(fitted, rect, format, color);
}

void DrawDriveRow(Compositor& compositor, fluent::Painter& painter, const Theme& theme,
                  const PickerEntry& entry, const D2D1_RECT_F& row,
                  const fluent::ControlState& state, bool high_contrast, float scale) {
    const bool inverse = InverseRow(state, high_contrast);
    const D2D1_COLOR_F text = inverse ? theme.bg : theme.text;
    const D2D1_COLOR_F secondary = inverse ? theme.bg : theme.text_secondary;
    fluent::ListRowSpec background{};
    background.bounds = row;
    background.state = state;
    painter.DrawListRowBackground(background);
    const float left = row.left + Dip(scale, 12.0f);
    const float right = row.right - Dip(scale, 12.0f);
    const float detail_left = right - Dip(scale, 176.0f);
    const float bar_left = std::max(left + Dip(scale, 160.0f), detail_left - Dip(scale, 128.0f));
    painter.DrawGlyph(L"\xE7F1", D2D1::RectF(left, row.top, left + Dip(scale, 20.0f),
                                           row.bottom),
                      secondary);
    DrawFittedText(compositor, painter, entry.name,
                   D2D1::RectF(left + Dip(scale, 28.0f), row.top, bar_left - Dip(scale, 8.0f),
                               row.bottom),
                   compositor.TextFormat(), text);
    if (entry.size == 0) return;
    const float used = 1.0f - static_cast<float>(static_cast<double>(entry.free) /
                                                 static_cast<double>(entry.size));
    const float mid = (row.top + row.bottom) * 0.5f;
    const float bar_h = Dip(scale, 6.0f);
    const D2D1_RECT_F track = D2D1::RectF(bar_left, mid - bar_h * 0.5f,
                                          detail_left - Dip(scale, 8.0f), mid + bar_h * 0.5f);
    painter.FillRoundedRect(track, bar_h * 0.5f, theme.fill_pressed);
    D2D1_RECT_F fill = track;
    fill.right = track.left + (track.right - track.left) * std::clamp(used, 0.0f, 1.0f);
    painter.FillRoundedRect(fill, bar_h * 0.5f, used > 0.9f ? theme.danger : theme.accent);
    painter.DrawText(DriveDetail(entry), D2D1::RectF(detail_left, row.top, right, row.bottom),
                     compositor.SmallFormat(), secondary,
                     fluent::HorizontalAlignment::Right);
}

void DrawEntryRow(Compositor& compositor, fluent::Painter& painter, const Theme& theme,
                  const PickerEntry& entry, const D2D1_RECT_F& row,
                  const fluent::ControlState& state, bool with_size, bool high_contrast,
                  float scale) {
    fluent::ListRowSpec background{};
    background.bounds = row;
    background.state = state;
    painter.DrawListRowBackground(background);
    const Columns c = ColumnsFor(row, scale, with_size);
    const bool inverse = InverseRow(state, high_contrast);
    const D2D1_COLOR_F text = inverse ? theme.bg : theme.text;
    const D2D1_COLOR_F secondary = inverse ? theme.bg : theme.text_secondary;
    const bool folder = entry.kind == PickerEntryKind::Folder;
    painter.DrawGlyph(folder ? L"\xE8B7" : L"\xEB9F",
                      D2D1::RectF(c.left, row.top, c.left + Dip(scale, 20.0f), row.bottom),
                      high_contrast ? text : folder ? theme.icon_folder : theme.icon_file);
    DrawFittedText(compositor, painter, entry.name,
                   D2D1::RectF(c.left + Dip(scale, 28.0f), row.top,
                               c.name_right - Dip(scale, 8.0f), row.bottom),
                   compositor.TextFormat(), text);
    painter.DrawText(format::LocalFileTime(entry.modified),
                     D2D1::RectF(c.name_right + Dip(scale, 8.0f), row.top,
                                 c.modified_right - Dip(scale, 4.0f), row.bottom),
                     compositor.SmallFormat(), secondary);
    if (c.type_right > c.modified_right) {
        painter.DrawText(TypeText(entry), D2D1::RectF(c.modified_right + Dip(scale, 8.0f),
                                                      row.top, c.type_right - Dip(scale, 4.0f),
                                                      row.bottom),
                         compositor.SmallFormat(), secondary);
    }
    if (with_size && entry.kind == PickerEntryKind::Image) {
        painter.DrawText(format::ByteSize(entry.size),
                         D2D1::RectF(c.type_right + Dip(scale, 8.0f), row.top, c.right,
                                     row.bottom),
                         compositor.SmallFormat(), secondary,
                         fluent::HorizontalAlignment::Right);
    }
}

} // namespace

FolderPickerLayout LayoutFolderPicker(float width, float height,
                                      const std::vector<PickerPlace>& places,
                                      const fluent::Painter& painter,
                                      const std::wstring& primary_text,
                                      const std::wstring& cancel_text, float scale) {
    FolderPickerLayout l;
    l.width = width;
    l.height = height;
    l.title_bar = Dip(scale, kTitleBar);
    l.row_h = std::round(Dip(scale, kRowH));
    l.close = D2D1::RectF(width - Dip(scale, 46.0f), 0, width, l.title_bar);

    const float ty = Dip(scale, kToolbarTop);
    const float th = Dip(scale, kToolbarH);
    l.back = D2D1::RectF(Dip(scale, 12.0f), ty, Dip(scale, 12.0f) + th, ty + th);
    l.up = D2D1::RectF(l.back.right + Dip(scale, 4.0f), ty,
                       l.back.right + Dip(scale, 4.0f) + th, ty + th);
    l.path = D2D1::RectF(l.up.right + Dip(scale, 8.0f), ty, width - Dip(scale, 16.0f), ty + th);

    const float body_top = ty + th + Dip(scale, 12.0f);
    const float footer_top = height - Dip(scale, kFooterH);
    l.sidebar = D2D1::RectF(Dip(scale, 8.0f), body_top, Dip(scale, 8.0f + kSidebarW),
                            footer_top - Dip(scale, 8.0f));
    float y = body_top + Dip(scale, 26.0f);  // room for the section label
    for (const PickerPlace& place : places) {
        if (place.separated) y += Dip(scale, 12.0f);
        l.places.push_back(D2D1::RectF(l.sidebar.left, y, l.sidebar.right,
                                       y + Dip(scale, kPlaceH)));
        y += Dip(scale, kPlaceH + 2.0f);
    }

    l.card = D2D1::RectF(l.sidebar.right + Dip(scale, 8.0f), body_top,
                         width - Dip(scale, 16.0f), footer_top - Dip(scale, 12.0f));
    l.header = D2D1::RectF(l.card.left + Dip(scale, 4.0f), l.card.top + Dip(scale, 2.0f),
                           l.card.right - Dip(scale, 4.0f),
                           l.card.top + Dip(scale, 2.0f + kHeaderH));
    l.rows = D2D1::RectF(l.card.left + Dip(scale, 4.0f), l.header.bottom + Dip(scale, 2.0f),
                         l.card.right - Dip(scale, 4.0f), l.card.bottom - Dip(scale, 4.0f));

    l.footer = D2D1::RectF(0, footer_top, width, height);
    const float btn_h = painter.MeasureButtonHeight();
    const float by = footer_top + (Dip(scale, kFooterH) - btn_h) * 0.5f;
    const float primary_w = std::max(painter.MeasureButtonWidth(primary_text),
                                     Dip(scale, kButtonMinW));
    const float cancel_w = std::max(painter.MeasureButtonWidth(cancel_text),
                                    Dip(scale, kButtonMinW));
    const float right = width - Dip(scale, 16.0f);
    l.primary = D2D1::RectF(right - primary_w, by, right, by + btn_h);
    l.cancel = D2D1::RectF(l.primary.left - Dip(scale, 8.0f) - cancel_w, by,
                           l.primary.left - Dip(scale, 8.0f), by + btn_h);
    l.summary = D2D1::RectF(Dip(scale, 20.0f), footer_top, l.cancel.left - Dip(scale, 16.0f),
                            height);
    return l;
}

float PickerContentHeight(const FolderPickerLayout& layout, size_t count) {
    return layout.row_h * static_cast<float>(count);
}

float ClampPickerScroll(const FolderPickerLayout& layout, size_t count, float scroll) {
    const float viewport = layout.rows.bottom - layout.rows.top;
    const float max_scroll = std::max(0.0f, PickerContentHeight(layout, count) - viewport);
    return std::clamp(scroll, 0.0f, max_scroll);
}

float ScrollPickerRowIntoView(const FolderPickerLayout& layout, size_t count, float scroll,
                              int index) {
    if (index < 0) return ClampPickerScroll(layout, count, scroll);
    const float viewport = layout.rows.bottom - layout.rows.top;
    const float top = layout.row_h * static_cast<float>(index);
    if (top < scroll) scroll = top;
    else if (top + layout.row_h > scroll + viewport) scroll = top + layout.row_h - viewport;
    return ClampPickerScroll(layout, count, scroll);
}

int HitTestFolderPicker(const FolderPickerLayout& l, const FolderPickerVisual& v,
                        float x, float y) {
    if (ContainsRect(l.close, x, y)) return kPickClose;
    if (ContainsRect(l.back, x, y)) return v.can_back ? kPickBack : kPickNone;
    if (ContainsRect(l.up, x, y)) return v.can_up ? kPickUp : kPickNone;
    if (ContainsRect(l.path, x, y)) return kPickPath;
    if (ContainsRect(l.primary, x, y)) return v.chosen.empty() ? kPickNone : kPickPrimary;
    if (ContainsRect(l.cancel, x, y)) return kPickCancel;
    for (size_t i = 0; i < l.places.size(); ++i) {
        if (ContainsRect(l.places[i], x, y)) return kPickPlace + static_cast<int>(i);
    }
    if (ContainsRect(l.rows, x, y)) {
        const float viewport = l.rows.bottom - l.rows.top;
        if (PickerContentHeight(l, v.entries.size()) > viewport &&
            x >= l.rows.right - 12.0f * (l.row_h / kRowH))
            return kPickScrollbar;
        const int index = static_cast<int>(std::floor((y - l.rows.top + v.scroll) / l.row_h));
        if (index >= 0 && index < static_cast<int>(v.entries.size())) return kPickRow + index;
        return kPickList;
    }
    if (ContainsRect(l.card, x, y)) return kPickList;
    return kPickNone;
}

void DrawFolderPicker(Compositor& compositor, fluent::Painter& painter, const Theme& theme,
                      const FolderPickerVisual& v, const FolderPickerLayout& l,
                      bool /*dark*/, bool high_contrast) {
    const float scale = painter.Scale();
    auto* dc = compositor.Dc();

    // Title strip.
    painter.DrawGlyph(v.mode == PickerMode::Image ? L"\xEB9F" : L"\xE8B7",
                      D2D1::RectF(Dip(scale, 14.0f), 0, Dip(scale, 38.0f), l.title_bar),
                      high_contrast ? theme.text : theme.accent);
    painter.DrawText(v.title, D2D1::RectF(Dip(scale, 46.0f), 0, l.close.left, l.title_bar),
                     compositor.HeaderFormat(), theme.text);
    fluent::ControlState close_state{};
    close_state.hovered = v.hover == kPickClose;
    close_state.pressed = v.pressed == kPickClose;
    painter.DrawTitleBarButton(l.close, fluent::TitleBarButtonRole::Close, {}, close_state);

    // Toolbar: back, up and the editable path.
    fluent::ButtonSpec back{l.back, {}, L"\xE72B", fluent::ButtonKind::Transparent,
                            StateFor(v, kPickBack, v.can_back)};
    back.icon_only = true;
    painter.DrawButton(back);
    fluent::ButtonSpec up{l.up, {}, L"\xE74A", fluent::ButtonKind::Transparent,
                          StateFor(v, kPickUp, v.can_up)};
    up.icon_only = true;
    painter.DrawButton(up);
    fluent::ControlState field{};
    field.focused = v.path_focused;
    field.hovered = v.hover == kPickPath;
    painter.DrawTextFieldFrame(l.path, field, v.hosted_edit);
    if (!v.hosted_edit) {
        painter.DrawText(v.path_text, D2D1::RectF(l.path.left + Dip(scale, 11.0f), l.path.top,
                                                  l.path.right - Dip(scale, 11.0f),
                                                  l.path.bottom),
                         compositor.TextFormat(), theme.text);
    }

    // Sidebar.
    painter.DrawText(l10n::Get(l10n::StringId::PickerQuickAccess),
                     D2D1::RectF(l.sidebar.left + Dip(scale, 12.0f), l.sidebar.top,
                                 l.sidebar.right, l.sidebar.top + Dip(scale, 22.0f)),
                     compositor.SmallFormat(), theme.text_secondary);
    for (size_t i = 0; i < v.places.size() && i < l.places.size(); ++i) {
        const PickerPlace& place = v.places[i];
        fluent::SidebarItemSpec item{};
        item.bounds = l.places[i];
        item.text = place.label;
        item.glyph = place.glyph;
        if (!high_contrast) item.icon_color = place.color;
        item.state = StateFor(v, kPickPlace + static_cast<int>(i));
        item.state.selected = SamePickerPath(place.path, v.current);
        painter.DrawSidebarItem(item);
    }

    // List card with column headers.
    const float radius = Dip(scale, theme.radius_control);
    painter.FillRoundedRect(l.card, radius, theme.surface_card);
    painter.StrokeRoundedRect(l.card, radius, theme.stroke_card);
    {
        const bool with_size = v.mode == PickerMode::Image;
        // Same span as the rows, which leave room for the scrollbar.
        const Columns c = ColumnsFor(D2D1::RectF(l.rows.left, l.header.top,
                                                 l.rows.right - Dip(scale, 8.0f),
                                                 l.header.bottom),
                                     scale, with_size);
        const bool drives = v.current.empty();
        auto header = [&](l10n::StringId id, float left, float right,
                          fluent::HorizontalAlignment align) {
            fluent::ColumnHeaderSpec spec{};
            spec.bounds = D2D1::RectF(left, l.header.top, right, l.header.bottom);
            spec.text = l10n::Get(id);
            spec.alignment = align;
            painter.DrawColumnHeader(spec);
        };
        const auto left = fluent::HorizontalAlignment::Left;
        header(l10n::StringId::ColumnName, l.header.left,
               drives ? l.header.right : c.name_right, left);
        if (!drives) {
            header(l10n::StringId::ColumnModified, c.name_right, c.modified_right, left);
            if (c.type_right > c.modified_right)
                header(l10n::StringId::ColumnType, c.modified_right, c.type_right, left);
            if (with_size)
                header(l10n::StringId::ColumnSize, c.type_right, c.right + Dip(scale, 10.0f),
                       fluent::HorizontalAlignment::Right);
        }
        painter.FillRoundedRect(D2D1::RectF(l.card.left, l.header.bottom, l.card.right,
                                            l.header.bottom + 1.0f),
                                0, theme.stroke_divider);
    }

    const D2D1_RECT_F& rows = l.rows;
    if (!v.error.empty()) {
        fluent::EmptyStateSpec empty{};
        empty.bounds = rows;
        empty.glyph = L"\xE783";
        empty.title = l10n::Get(l10n::StringId::PickerOpenFailed);
        empty.message = v.error;
        painter.DrawEmptyState(empty);
    } else if (v.waiting) {
        // Most folders arrive within a frame or two; the spinner only shows
        // once `loading` is set after a short delay, so nothing flashes.
        if (v.loading) {
            const float ring = Dip(scale, 28.0f);
            const float cx = (rows.left + rows.right) * 0.5f;
            const float cy = (rows.top + rows.bottom) * 0.5f - Dip(scale, 12.0f);
            fluent::ProgressSpec spinner{};
            spinner.bounds = D2D1::RectF(cx - ring * 0.5f, cy - ring * 0.5f, cx + ring * 0.5f,
                                         cy + ring * 0.5f);
            spinner.indeterminate = true;
            spinner.animation_progress = v.spinner;
            painter.DrawProgressRing(spinner);
            painter.DrawText(l10n::Get(l10n::StringId::PickerLoading),
                             D2D1::RectF(rows.left, spinner.bounds.bottom + Dip(scale, 8.0f),
                                         rows.right, spinner.bounds.bottom + Dip(scale, 30.0f)),
                             compositor.TextFormat(), theme.text_secondary,
                             fluent::HorizontalAlignment::Center);
        }
    } else if (v.entries.empty()) {
        const bool image = v.mode == PickerMode::Image;
        fluent::EmptyStateSpec empty{};
        empty.bounds = rows;
        empty.glyph = image ? L"\xEB9F" : L"\xE8B7";
        empty.title = l10n::Get(image ? l10n::StringId::PickerNoImages
                                      : l10n::StringId::PickerNoFolders);
        empty.message = l10n::Get(image ? l10n::StringId::PickerNoImagesDesc
                                        : l10n::StringId::PickerNoFoldersDesc);
        painter.DrawEmptyState(empty);
    } else if (dc) {
        dc->PushAxisAlignedClip(rows, D2D1_ANTIALIAS_MODE_ALIASED);
        const int first = std::max(0, static_cast<int>(std::floor(v.scroll / l.row_h)));
        const int count = static_cast<int>(v.entries.size());
        for (int i = first; i < count; ++i) {
            const float top = rows.top + l.row_h * static_cast<float>(i) - v.scroll;
            if (top >= rows.bottom) break;
            const D2D1_RECT_F row = D2D1::RectF(rows.left, top, rows.right - Dip(scale, 8.0f),
                                                top + l.row_h);
            const PickerEntry& entry = v.entries[static_cast<size_t>(i)];
            fluent::ControlState state = StateFor(v, kPickRow + i);
            state.selected = i == v.selected;
            state.keyboard_focus = false;
            if (entry.kind == PickerEntryKind::Drive) {
                DrawDriveRow(compositor, painter, theme, entry, row, state, high_contrast, scale);
            } else {
                DrawEntryRow(compositor, painter, theme, entry, row, state,
                             v.mode == PickerMode::Image, high_contrast, scale);
            }
            if (i == v.selected && v.show_focus && v.focus == kPickList)
                painter.DrawFocusRing(row, radius);
        }
        dc->PopAxisAlignedClip();

        fluent::ScrollbarSpec bar{};
        bar.viewport = rows;
        bar.offset = v.scroll;
        bar.viewport_extent = rows.bottom - rows.top;
        bar.content_extent = PickerContentHeight(l, v.entries.size());
        bar.expand_progress = v.hover == kPickScrollbar || v.pressed == kPickScrollbar ? 1.0f
                                                                                       : 0.0f;
        painter.DrawScrollbar(bar);
    }

    // Footer: what will be picked, then the buttons.
    painter.FillRoundedRect(l.footer, 0, theme.surface_sheet);
    painter.FillRoundedRect(D2D1::RectF(l.footer.left, l.footer.top, l.footer.right,
                                        l.footer.top + 1.0f),
                            0, theme.stroke_divider);
    std::wstring summary;
    if (!v.chosen.empty()) {
        // Long choices keep the drive and the chosen folder's name visible.
        const std::wstring prefix = l10n::Get(l10n::StringId::PickerWillSelect);
        const auto measure = [&](std::wstring_view s) {
            return typography::MeasureLine(&compositor, compositor.TextFormat(), s);
        };
        summary = prefix + FitPathMiddle(path::FriendlyPathText(v.chosen),
                                         l.summary.right - l.summary.left - measure(prefix),
                                         measure);
    } else if (v.mode == PickerMode::Image) {
        summary = l10n::Get(l10n::StringId::PickerPickImageHint);
    }
    painter.DrawText(summary, l.summary, compositor.TextFormat(), theme.text_secondary,
                     fluent::HorizontalAlignment::Left, theme.surface_sheet);
    painter.DrawButton({l.cancel, v.cancel_text, {}, fluent::ButtonKind::Standard,
                        StateFor(v, kPickCancel)});
    painter.DrawButton({l.primary, v.primary_text, {}, fluent::ButtonKind::Primary,
                        StateFor(v, kPickPrimary, !v.chosen.empty())});
    // The shared accent ring vanishes against a filled button; add the Fluent
    // outer ring in the text colour.
    if (StateFor(v, kPickPrimary, !v.chosen.empty()).keyboard_focus) {
        const float gap = Dip(scale, 3.0f);
        painter.StrokeRoundedRect(D2D1::RectF(l.primary.left - gap, l.primary.top - gap,
                                              l.primary.right + gap, l.primary.bottom + gap),
                                  radius + gap, theme.text, Dip(scale, 1.5f));
    }

    // The footer band covers the surface outline; draw it again on top.
    const float inset = 0.5f;
    painter.StrokeRoundedRect(D2D1::RectF(inset, inset, l.width - inset, l.height - inset),
                              Dip(scale, 12.0f), theme.stroke_card);

}

} // namespace pulse::ui
