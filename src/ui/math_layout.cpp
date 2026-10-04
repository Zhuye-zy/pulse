#include "math_layout.h"
#include "math_alphabet.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <utility>

namespace pulse::ui::math {
namespace {
using Microsoft::WRL::ComPtr;
using Box = FormulaLayout;
using Kind = MathNodeKind;
using Style = MathStyle;

void Put(Box &dst, const Box &src, float x, float y = 0) {
    dst.intentional_space = dst.intentional_space || src.intentional_space;
    dst.width = std::max(dst.width, x + src.width);
    dst.ascent = std::max(dst.ascent, src.ascent - y);
    dst.descent = std::max(dst.descent, src.descent + y);
    for (const auto &glyph : src.glyphs)
        dst.glyphs.push_back({glyph.layout, glyph.x + x, glyph.y + y});
    for (const auto &rule : src.rules)
        dst.rules.push_back({rule.x1 + x, rule.y1 + y, rule.x2 + x, rule.y2 + y, rule.width});
}
struct Ink {
    float left, top, right, bottom;
};
bool Bounds(const Box &box, Ink &bounds) {
    const float infinity = std::numeric_limits<float>::infinity();
    bounds = {infinity, infinity, -infinity, -infinity};
    for (const auto &glyph : box.glyphs) {
        DWRITE_OVERHANG_METRICS ink{};
        if (FAILED(glyph.layout->GetOverhangMetrics(&ink)))
            return false;
        bounds.left = std::min(bounds.left, glyph.x - ink.left);
        bounds.top = std::min(bounds.top, glyph.y - ink.top);
        bounds.right = std::max(bounds.right, glyph.x + glyph.layout->GetMaxWidth() + ink.right);
        bounds.bottom = std::max(bounds.bottom, glyph.y + glyph.layout->GetMaxHeight() + ink.bottom);
    }
    for (const auto &rule : box.rules) {
        const float half = rule.width / 2;
        bounds.left = std::min(bounds.left, std::min(rule.x1, rule.x2) - half);
        bounds.top = std::min(bounds.top, std::min(rule.y1, rule.y2) - half);
        bounds.right = std::max(bounds.right, std::max(rule.x1, rule.x2) + half);
        bounds.bottom = std::max(bounds.bottom, std::max(rule.y1, rule.y2) + half);
    }
    return std::isfinite(bounds.left) && std::isfinite(bounds.top) && std::isfinite(bounds.right) &&
           std::isfinite(bounds.bottom);
}
bool ContainInk(Box &box) {
    Ink ink{};
    if (Bounds(box, ink)) {
        const float left = std::min(0.0f, ink.left);
        box.width = std::max({0.0f, box.width, ink.right}) - left;
        box.ascent = std::max(box.ascent, -ink.top);
        box.descent = std::max(box.descent, ink.bottom);
        for (auto &glyph : box.glyphs)
            glyph.x -= left;
        for (auto &rule : box.rules) {
            rule.x1 -= left;
            rule.x2 -= left;
        }
    } else if (!box.glyphs.empty() || !box.rules.empty())
        return false;
    else
        box.width = std::max(0.0f, box.width);
    return std::isfinite(box.width) && std::isfinite(box.ascent) && std::isfinite(box.descent);
}
bool ContentBounds(const Box &box, Ink &bounds) {
    const bool ink = Bounds(box, bounds);
    if (box.intentional_space) {
        if (!ink)
            bounds = {0, -box.ascent, box.width, box.descent};
        else {
            bounds.left = std::min(bounds.left, 0.0f);
            bounds.top = std::min(bounds.top, -box.ascent);
            bounds.right = std::max(bounds.right, box.width);
            bounds.bottom = std::max(bounds.bottom, box.descent);
        }
    }
    return ink || box.intentional_space;
}
struct Context {
    Style style;
    bool roman = false, bold = false;
    MathAlphabet alphabet = MathAlphabet::Default;
    bool bold_symbols = false;
};

class Layout {
  public:
    Layout(IDWriteFactory2 *factory, IDWriteFontFace *face, const FontMathMetrics &metrics, float size)
        : factory_(factory), face_(face), metrics_(metrics), base_size_(size) {}
    bool Run(const MathNode &root, bool display, Box &result) {
        result = Build(root, {display ? Style::Display : Style::Text});
        if (!ok_ || (result.glyphs.empty() && result.rules.empty() && !result.intentional_space) || !ContainInk(result))
            return false;
        return result.width >= 0 && result.ascent + result.descent <= 4096;
    }

  private:
    IDWriteFactory2 *factory_;
    IDWriteFontFace *face_;
    const FontMathMetrics &metrics_;
    float base_size_;
    bool ok_ = true;
    size_t visited_ = 0;
    size_t depth_ = 0;
    float Size(Context context) const {
        return base_size_ * (context.style == Style::Script         ? metrics_.script_scale
                             : context.style == Style::ScriptScript ? metrics_.script_script_scale
                                                                    : 1);
    }
    Context Script(Context context) const {
        context.style =
            context.style == Style::Display || context.style == Style::Text ? Style::Script : Style::ScriptScript;
        return context;
    }
    float Length(const MathLength &length, Context context) {
        const float limit = length.unit == MathLengthUnit::Em   ? 20.0f
                            : length.unit == MathLengthUnit::Ex ? 40.0f
                                                                : 200.0f;
        if (!std::isfinite(length.value) || std::abs(length.value) > limit) {
            ok_ = false;
            return 0;
        }
        if (length.unit == MathLengthUnit::Pt)
            return length.value * (96.0f / 72.27f);
        if (length.unit == MathLengthUnit::Ex) {
            DWRITE_FONT_METRICS font{};
            face_->GetMetrics(&font);
            return length.value * Size(context) *
                   (font.designUnitsPerEm && font.xHeight ? static_cast<float>(font.xHeight) / font.designUnitsPerEm
                                                          : 0.5f);
        }
        return length.value * Size(context);
    }
    bool HasGlyph(UINT32 scalar) const {
        UINT16 glyph = 0;
        return SUCCEEDED(face_->GetGlyphIndices(&scalar, 1, &glyph)) && glyph != 0;
    }
    bool CheckGlyphs(std::wstring_view text) const {
        for (size_t i = 0; i < text.size(); ++i) {
            UINT32 scalar = text[i];
            if (scalar >= 0xd800 && scalar <= 0xdbff) {
                if (++i == text.size() || text[i] < 0xdc00 || text[i] > 0xdfff)
                    return false;
                scalar = 0x10000 + ((scalar - 0xd800) << 10) + (static_cast<UINT32>(text[i]) - 0xdc00);
            } else if (scalar >= 0xdc00 && scalar <= 0xdfff)
                return false;
            if (!HasGlyph(scalar))
                return false;
        }
        return true;
    }
    Box Text(std::wstring_view text, float size, Context context, bool upright = true, bool tight = false) {
        Box box;
        if (text.empty())
            return box;
        ComPtr<IDWriteTextFormat> format;
        if (FAILED(factory_->CreateTextFormat(
                L"Cambria Math", nullptr, context.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                (upright || context.roman) ? DWRITE_FONT_STYLE_NORMAL : DWRITE_FONT_STYLE_ITALIC,
                DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &format))) {
            ok_ = false;
            return {};
        }
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(factory_->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format.Get(), 100000,
                                              100000, &layout))) {
            ok_ = false;
            return {};
        }
        DWRITE_TEXT_METRICS metrics{};
        DWRITE_OVERHANG_METRICS overhang{};
        DWRITE_LINE_METRICS line{};
        UINT32 count = 0;
        if (FAILED(layout->GetMetrics(&metrics)) ||
            FAILED(layout->SetMaxWidth(std::max(1.0f, metrics.widthIncludingTrailingWhitespace))) ||
            FAILED(layout->SetMaxHeight(std::max(1.0f, metrics.height))) ||
            FAILED(layout->GetOverhangMetrics(&overhang)) || FAILED(layout->GetLineMetrics(&line, 1, &count))) {
            ok_ = false;
            return {};
        }
        if (tight) {
            const float bottom = layout->GetMaxHeight() + overhang.bottom;
            box.width = layout->GetMaxWidth() + overhang.left + overhang.right;
            box.ascent = bottom + overhang.top;
            if (box.width <= 0 || box.ascent <= 0) {
                ok_ = false;
                return {};
            }
            box.glyphs.push_back({std::move(layout), overhang.left, -bottom});
        } else {
            const float left = std::max(0.0f, overhang.left);
            box.width = metrics.widthIncludingTrailingWhitespace + left + std::max(0.0f, overhang.right);
            box.ascent = line.baseline + std::max(0.0f, overhang.top);
            box.descent = metrics.height - line.baseline + std::max(0.0f, overhang.bottom);
            box.glyphs.push_back({std::move(layout), left, -line.baseline});
        }
        return box;
    }
    Box JoinRow(const MathNode &node, Context context, const std::vector<Box> &boxes) {
        std::vector<MathAtomClass> classes;
        for (const auto &child : node.children)
            if (child.kind != Kind::Space && !(child.kind == Kind::Style && child.children.empty()))
                classes.push_back(child.atom_class);
        NormalizeMathAtomClasses(classes);
        Box row;
        size_t atom = 0;
        for (size_t i = 0; i < node.children.size(); ++i) {
            const auto &child = node.children[i];
            if (!ok_)
                break;
            if (child.kind == Kind::Style && child.children.empty()) {
                context.style = child.style;
                continue;
            }
            const bool is_atom = child.kind != Kind::Space;
            if (is_atom && atom > 0)
                row.width += Size(context) * MathAtomSpacing(classes[atom - 1], classes[atom], context.style);
            const Box &box = boxes[i];
            const float end = row.width + box.width;
            Put(row, box, row.width);
            row.width = end;
            if (is_atom)
                ++atom;
        }
        return row;
    }
    Box Row(const MathNode &node, Context context) {
        std::vector<Box> boxes;
        Context current = context;
        for (const auto &child : node.children) {
            if (child.kind == Kind::Style && child.children.empty())
                current.style = child.style;
            boxes.push_back(Build(child, current));
        }
        return JoinRow(node, context, boxes);
    }
    Box Scripts(const MathNode &node, Context context) {
        if (node.children.size() != 3) {
            ok_ = false;
            return {};
        }
        const auto &nucleus = node.children[0];
        const Box base = Build(nucleus, context);
        if (base.glyphs.empty() && base.rules.empty() && !base.intentional_space) {
            ok_ = false;
            return {};
        }
        const Box sup = Build(node.children[1], Script(context)), sub = Build(node.children[2], Script(context));
        const float size = Size(context);
        const bool visible_sup =
            node.has_superscript && (!sup.glyphs.empty() || !sup.rules.empty() || sup.intentional_space);
        const bool visible_sub =
            node.has_subscript && (!sub.glyphs.empty() || !sub.rules.empty() || sub.intentional_space);
        const bool limits =
            node.limits == MathLimits::AboveBelow ||
            (node.limits == MathLimits::Default && nucleus.default_limits && context.style == Style::Display);
        Box box;
        if (nucleus.kind == Kind::Brace) {
            const bool over = nucleus.text == L"overbrace";
            const Box &label = over ? sup : sub;
            const bool has_label = over ? visible_sup : visible_sub;
            const Box &side = over ? sub : sup;
            const bool has_side = over ? visible_sub : visible_sup;
            const float width = std::max(base.width, has_label ? label.width : 0);
            Put(box, base, (width - base.width) / 2);
            if (has_label)
                Put(box, label, (width - label.width) / 2,
                    over ? -base.ascent - size * .2f - label.descent : base.descent + size * .2f + label.ascent);
            box.width = width;
            if (has_side) {
                Put(box, side, width,
                    over ? size * metrics_.subscript_shift_down
                         : -std::max(size * metrics_.superscript_shift_up, base.ascent - size * .3f));
                box.width += size * metrics_.space_after_script;
            }
            return box;
        }
        if (limits) {
            const float width = std::max({base.width, sup.width, sub.width});
            Put(box, base, (width - base.width) / 2);
            if (visible_sup)
                Put(box, sup, (width - sup.width) / 2, -base.ascent - size * metrics_.upper_limit_gap - sup.descent);
            if (visible_sub)
                Put(box, sub, (width - sub.width) / 2, base.descent + size * metrics_.lower_limit_gap + sub.ascent);
            box.width = width;
        } else {
            Put(box, base, 0);
            const float up = std::max(size * metrics_.superscript_shift_up, base.ascent - size * 0.3f);
            float down = std::max(size * metrics_.subscript_shift_down, base.descent + size * 0.1f);
            if (visible_sup && visible_sub)
                down += std::max(0.0f, size * metrics_.sub_superscript_gap - (up + down - sup.descent - sub.ascent));
            if (visible_sup)
                Put(box, sup, base.width, -up);
            if (visible_sub)
                Put(box, sub, base.width, down);
            box.width = std::max(box.width, base.width + std::max(sup.width, sub.width));
            // This is trailing script space, not a kern between base and script.
            // An explicit empty script retains the space without adding height.
            if (node.has_superscript || node.has_subscript)
                box.width += size * metrics_.space_after_script;
        }
        return box;
    }
    Box FencedAt(Box body, std::wstring_view left, std::wstring_view right, Context context, float fence_size,
                 float center) {
        const float size = Size(context);
        Box box;
        const Box l = Text(left, fence_size, context), r = Text(right, fence_size, context);
        if (!left.empty())
            Put(box, l, 0, center + (l.ascent - l.descent) / 2);
        Put(box, body, l.width + (left.empty() ? 0 : size * 0.05f));
        if (!right.empty())
            Put(box, r, box.width + size * 0.05f, center + (r.ascent - r.descent) / 2);
        return box;
    }
    Box Fenced(Box body, std::wstring_view left, std::wstring_view right, Context context) {
        const float fence_size = std::max(Size(context), (body.ascent + body.descent) * .88f);
        const float center = (body.descent - body.ascent) / 2;
        return FencedAt(std::move(body), left, right, context, fence_size, center);
    }
    Box Fence(const MathNode &node, Context context) {
        if (node.children.size() != 1 || node.children[0].kind != Kind::Row) {
            ok_ = false;
            return {};
        }
        const auto &row = node.children[0];
        if (++visited_ > 4096) {
            ok_ = false;
            return {};
        }
        std::vector<Box> boxes(row.children.size());
        std::vector<Context> contexts(row.children.size(), context);
        Context current = context;
        float ascent = 0, descent = 0;
        for (size_t i = 0; i < row.children.size(); ++i) {
            const auto &child = row.children[i];
            if (child.kind == Kind::Style && child.children.empty())
                current.style = child.style;
            contexts[i] = current;
            if (child.kind != Kind::Middle) {
                boxes[i] = Build(child, current);
                ascent = std::max(ascent, boxes[i].ascent);
                descent = std::max(descent, boxes[i].descent);
            }
        }
        const float size = std::max(Size(context), (ascent + descent) * .88f);
        const float center = (descent - ascent) / 2;
        for (size_t i = 0; i < row.children.size(); ++i) {
            if (row.children[i].kind != Kind::Middle)
                continue;
            if (++visited_ > 4096) {
                ok_ = false;
                return {};
            }
            const Box glyph = Text(row.children[i].text, size, contexts[i]);
            if (!row.children[i].text.empty())
                Put(boxes[i], glyph, 0, center + (glyph.ascent - glyph.descent) / 2);
        }
        return FencedAt(JoinRow(row, context, boxes), node.text, node.auxiliary, context, size, center);
    }
    Box Brace(const MathNode &node, Context context) {
        if (node.children.size() != 1) {
            ok_ = false;
            return {};
        }
        Context body_context = context;
        body_context.style = Style::Display;
        Box body = Build(node.children[0], body_context);
        if (!ContainInk(body)) {
            ok_ = false;
            return {};
        }
        const float size = Size(context), stroke = std::max(.6f, size * .04f);
        const float width = std::max(body.width, size * .8f), depth = size * .32f;
        const bool over = node.text == L"overbrace";
        const float near_y = over ? -body.ascent - size * .1f - stroke / 2 : body.descent + size * .1f + stroke / 2;
        const float direction = over ? -1.0f : 1.0f;
        const float shoulder = std::min(width * .2f, size * .4f);
        const float middle = near_y + direction * depth * .5f, far_y = near_y + direction * depth;
        Box box;
        Put(box, body, (width - body.width) / 2);
        box.width = width;
        // Four cubic segments keep the hooks and central cusp fixed in size as
        // the horizontal brace grows. Only the connecting shoulders stretch.
        const auto curve = [&box, stroke](float x0, float y0, float x1, float y1, float x2, float y2, float x3,
                                          float y3) {
            float px = x0, py = y0;
            for (int i = 1; i <= 12; ++i) {
                const float t = static_cast<float>(i) / 12, u = 1 - t;
                const float x = u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3;
                const float y = u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3;
                box.rules.push_back({px, py, x, y, stroke});
                px = x;
                py = y;
            }
        };
        curve(0, near_y, 0, middle, shoulder * .3f, middle, shoulder, middle);
        box.rules.push_back({shoulder, middle, width / 2 - shoulder, middle, stroke});
        curve(width / 2 - shoulder, middle, width / 2 - shoulder * .2f, middle, width / 2, middle, width / 2, far_y);
        curve(width / 2, far_y, width / 2, middle, width / 2 + shoulder * .2f, middle, width / 2 + shoulder, middle);
        box.rules.push_back({width / 2 + shoulder, middle, width - shoulder, middle, stroke});
        curve(width - shoulder, middle, width - shoulder * .3f, middle, width, middle, width, near_y);
        if (over)
            box.ascent = std::max(box.ascent, -far_y + stroke / 2);
        else
            box.descent = std::max(box.descent, far_y + stroke / 2);
        return box;
    }
    Box Fraction(const MathNode &node, Context context) {
        if (node.children.size() != 2) {
            ok_ = false;
            return {};
        }
        if (node.style != Style::Inherit)
            context.style = node.style;
        const float size = Size(context);
        const bool display = context.style == Style::Display;
        Context child = context;
        child.style = display ? Style::Text : Script(context).style;
        const Box numerator = Build(node.children[0], child), denominator = Build(node.children[1], child);
        const float pad = size * 0.16f, width = std::max(numerator.width, denominator.width) + 2 * pad;
        const float axis = -size * metrics_.axis_height,
                    stroke = std::max(0.6f, size * metrics_.fraction_rule_thickness);
        const float numerator_gap =
            size * (display ? metrics_.fraction_display_numerator_gap : metrics_.fraction_numerator_gap);
        const float denominator_gap =
            size * (display ? metrics_.fraction_display_denominator_gap : metrics_.fraction_denominator_gap);
        const float up = std::max(
            size * (display ? metrics_.fraction_display_numerator_shift_up : metrics_.fraction_numerator_shift_up),
            -axis + stroke / 2 + numerator_gap + numerator.descent);
        const float down = std::max(size * (display ? metrics_.fraction_display_denominator_shift_down
                                                    : metrics_.fraction_denominator_shift_down),
                                    axis + stroke / 2 + denominator_gap + denominator.ascent);
        Box box;
        Put(box, numerator, (width - numerator.width) / 2, -up);
        Put(box, denominator, (width - denominator.width) / 2, down);
        box.width = width;
        if (node.text == L"binom" || node.text == L"dbinom" || node.text == L"tbinom")
            return Fenced(std::move(box), L"(", L")", context);
        box.rules.push_back({0, axis, width, axis, stroke});
        return box;
    }
    Box Radical(const MathNode &node, Context context) {
        if (node.children.size() != 2) {
            ok_ = false;
            return {};
        }
        const float size = Size(context);
        const Box body = Build(node.children[0], context);
        Context index_context = context;
        index_context.style = Style::ScriptScript;
        const Box index = Build(node.children[1], index_context);
        const float stroke = std::max(0.6f, size * metrics_.radical_rule_thickness);
        const float top =
            -body.ascent -
            size * (context.style == Style::Display ? metrics_.radical_display_gap : metrics_.radical_gap) - stroke / 2;
        const float start = node.has_superscript ? index.width * 0.7f : 0, x = start + size * 0.65f;
        Box box;
        Put(box, body, x + size * 0.1f);
        if (node.has_superscript)
            Put(box, index, 0, top + index.ascent * 0.45f);
        box.rules.push_back({start + size * 0.04f, -size * 0.18f, start + size * 0.2f, -size * 0.28f, stroke});
        box.rules.push_back({start + size * 0.2f, -size * 0.28f, start + size * 0.36f, body.descent, stroke});
        box.rules.push_back({start + size * 0.36f, body.descent, x, top, stroke});
        box.rules.push_back({x, top, box.width + size * 0.06f, top, stroke});
        box.width += size * 0.1f;
        box.ascent = std::max(box.ascent, -top + stroke);
        box.descent += stroke;
        return box;
    }
    Box Environment(const MathNode &node, Context context) {
        if (node.style != Style::Inherit)
            context.style = node.style;
        const float size = Size(context);
        const bool stack = node.text == L"substack";
        std::vector<std::vector<Box>> rows;
        size_t columns = node.auxiliary.size();
        for (const auto &row : node.children) {
            rows.emplace_back();
            for (const auto &cell : row.children)
                rows.back().push_back(Build(cell, context));
            columns = std::max(columns, rows.back().size());
        }
        if (columns > 16 || rows.size() > 32 || (!node.auxiliary.empty() && columns > node.auxiliary.size())) {
            ok_ = false;
            return {};
        }
        std::vector<float> widths(columns);
        for (const auto &row : rows)
            for (size_t c = 0; c < row.size(); ++c)
                widths[c] = std::max(widths[c], row[c].width);
        const float row_gap = size * (stack ? .15f : .25f);
        if ((!node.column_rules.empty() && node.column_rules.size() != columns + 1) ||
            (!node.row_rules.empty() && node.row_rules.size() != rows.size() + 1) ||
            (!node.row_gaps.empty() && node.row_gaps.size() != rows.size())) {
            ok_ = false;
            return {};
        }
        const float stroke = std::max(.6f, size * metrics_.fraction_rule_thickness);
        const float separation = size * .12f;
        const auto count = [this](const std::vector<unsigned char> &rules, size_t at) {
            const unsigned n = rules.empty() ? 0u : rules[at];
            if (n > 2)
                ok_ = false;
            return std::min(n, 2u);
        };
        const auto band = [stroke, separation](unsigned n) {
            return n ? static_cast<float>(n) * stroke + static_cast<float>(n - 1) * separation : 0.0f;
        };
        std::vector<float> starts(columns), boundaries(columns + 1);
        float total_width = 0;
        for (size_t c = 0; c <= columns; ++c) {
            const unsigned n = count(node.column_rules, c);
            const bool outer = c == 0 || c == columns;
            const float gap = outer ? (n ? size * .2f : 0) : size * (node.text == L"aligned" && c % 2 == 1 ? .2f : .7f);
            boundaries[c] = total_width + (c == 0 ? 0 : c == columns ? gap : gap / 2);
            total_width += gap + band(n);
            if (c < columns) {
                starts[c] = total_width;
                total_width += widths[c];
            }
        }
        Box box;
        float y = 0;
        const auto horizontal = [&](unsigned n) {
            for (unsigned line = 0; line < n; ++line) {
                const float center = y + stroke / 2 + static_cast<float>(line) * (stroke + separation);
                box.rules.push_back({0, center, total_width, center, stroke});
            }
            y += band(n);
        };
        const unsigned top_rules = count(node.row_rules, 0);
        horizontal(top_rules);
        if (top_rules)
            y += size * .15f;
        for (size_t r = 0; r < rows.size(); ++r) {
            const auto &row = rows[r];
            float ascent = size * .7f, descent = size * .2f;
            for (const auto &cell : row) {
                ascent = std::max(ascent, cell.ascent);
                descent = std::max(descent, cell.descent);
            }
            y += ascent;
            for (size_t c = 0; c < columns; ++c) {
                if (c < row.size()) {
                    const float free = widths[c] - row[c].width;
                    const float offset = !node.auxiliary.empty()   ? (node.auxiliary[c] == L'l'   ? 0
                                                                      : node.auxiliary[c] == L'r' ? free
                                                                                                  : free / 2)
                                         : node.text == L"cases"   ? 0
                                         : node.text == L"aligned" ? (c % 2 == 0 ? free : 0)
                                                                   : free / 2;
                    Put(box, row[c], starts[c] + offset, y);
                }
            }
            y += descent;
            if (!node.row_gaps.empty()) {
                if (node.row_gaps[r].value < 0)
                    ok_ = false;
                y += Length(node.row_gaps[r], context);
            }
            const unsigned n = count(node.row_rules, r + 1);
            if (n) {
                y += size * .15f;
                horizontal(n);
                if (r + 1 < rows.size())
                    y += size * .15f;
            } else if (r + 1 < rows.size())
                y += row_gap;
        }
        box.width = total_width;
        box.descent = std::max(box.descent, y);
        for (size_t c = 0; c <= columns; ++c)
            for (unsigned line = 0; line < count(node.column_rules, c); ++line) {
                const float x = boundaries[c] + stroke / 2 + static_cast<float>(line) * (stroke + separation);
                box.rules.push_back({x, 0, x, y, stroke});
            }
        Box centered;
        Put(centered, box, 0, -y / 2 - size * metrics_.axis_height);
        if (node.text == L"pmatrix")
            return Fenced(std::move(centered), L"(", L")", context);
        if (node.text == L"bmatrix")
            return Fenced(std::move(centered), L"[", L"]", context);
        if (node.text == L"Bmatrix")
            return Fenced(std::move(centered), L"{", L"}", context);
        if (node.text == L"vmatrix")
            return Fenced(std::move(centered), L"|", L"|", context);
        if (node.text == L"Vmatrix")
            return Fenced(std::move(centered), L"‖", L"‖", context);
        if (node.text == L"cases")
            return Fenced(std::move(centered), L"{", L"", context);
        return centered;
    }
    Box Decoration(const MathNode &node, Context context) {
        if (node.children.size() != 1) {
            ok_ = false;
            return {};
        }
        if (node.style != Style::Inherit)
            context.style = node.style;
        const float size = Size(context), stroke = std::max(.6f, size * .045f);
        Box body = Build(node.children[0], context);
        if (node.text == L"boxed") {
            if (!ContainInk(body)) {
                ok_ = false;
                return {};
            }
            const float pad = size * .2f, width = body.width + 2 * pad, top = -body.ascent - pad,
                        bottom = body.descent + pad;
            Box box;
            Put(box, body, pad);
            box.width = width;
            box.ascent = -top + stroke / 2;
            box.descent = bottom + stroke / 2;
            box.rules.push_back({0, top, width, top, stroke});
            box.rules.push_back({width, top, width, bottom, stroke});
            box.rules.push_back({width, bottom, 0, bottom, stroke});
            box.rules.push_back({0, bottom, 0, top, stroke});
            return box;
        }
        Ink ink{};
        if (!ContentBounds(body, ink)) {
            ok_ = false;
            return {};
        }
        if (node.text != L"bcancel")
            body.rules.push_back({ink.left, ink.bottom, ink.right, ink.top, stroke});
        if (node.text != L"cancel")
            body.rules.push_back({ink.left, ink.top, ink.right, ink.bottom, stroke});
        return body;
    }
    Box Arrow(const MathNode &node, Context context) {
        if (node.children.size() != 2) {
            ok_ = false;
            return {};
        }
        const float size = Size(context);
        Box above = Build(node.children[0], Script(context)), below = Build(node.children[1], Script(context));
        if (!ContainInk(above) || !ContainInk(below)) {
            ok_ = false;
            return {};
        }
        const float width = std::max(size * 1.6f, std::max(above.width, below.width) + size * .5f);
        const float axis = -size * metrics_.axis_height, head_half = size * .12f, stroke = std::max(.6f, size * .05f);
        Box box;
        Ink a{}, b{};
        if (Bounds(above, a))
            Put(box, above, (width - above.width) / 2, axis - head_half - size * .15f - a.bottom);
        if (Bounds(below, b))
            Put(box, below, (width - below.width) / 2, axis + head_half + size * .15f - b.top);
        box.rules.push_back({0, axis, width, axis, stroke});
        const float tip = node.text == L"xrightarrow" ? width : 0,
                    head = tip + (node.text == L"xrightarrow" ? -1 : 1) * size * .2f;
        box.rules.push_back({head, axis - head_half, tip, axis, stroke});
        box.rules.push_back({head, axis + head_half, tip, axis, stroke});
        box.width = width;
        box.ascent = std::max(box.ascent, -axis + head_half + stroke / 2);
        box.descent = std::max(box.descent, axis + head_half + stroke / 2);
        return box;
    }
    Box Accent(const MathNode &node, Context context) {
        const float size = Size(context);
        const auto &cmd = node.text;
        if (cmd == L"overset" || cmd == L"underset") {
            if (node.children.size() != 2) {
                ok_ = false;
                return {};
            }
            const Box annotation = Build(node.children[0], Script(context)), base = Build(node.children[1], context);
            const float width = std::max(base.width, annotation.width);
            Box box;
            Put(box, base, (width - base.width) / 2);
            Put(box, annotation, (width - annotation.width) / 2,
                cmd == L"overset" ? -base.ascent - size * metrics_.upper_limit_gap - annotation.descent
                                  : base.descent + size * metrics_.lower_limit_gap + annotation.ascent);
            return box;
        }
        if (node.children.size() != 1) {
            ok_ = false;
            return {};
        }
        Box box = Build(node.children[0], context);
        if (box.width <= 0 && !box.intentional_space) {
            ok_ = false;
            return {};
        }
        if (cmd.ends_with(L"arrow")) {
            if (box.glyphs.empty() && box.rules.empty() && !box.intentional_space) {
                ok_ = false;
                return {};
            }
            const float width = std::max(box.width, size * .6f);
            Box arrow;
            Put(arrow, box, (width - box.width) / 2);
            arrow.width = width;
            const bool under = cmd.starts_with(L"under"),
                       left = cmd == L"overleftarrow" || cmd == L"underleftarrow" || cmd.ends_with(L"leftrightarrow"),
                       right =
                           cmd == L"overrightarrow" || cmd == L"underrightarrow" || cmd.ends_with(L"leftrightarrow");
            const float stroke = std::max(.6f, size * .05f), half = size * .12f, head = size * .2f;
            const float y = under ? box.descent + size * .13f + half : -box.ascent - size * .13f - half;
            arrow.rules.push_back({0, y, width, y, stroke});
            if (left) {
                arrow.rules.push_back({head, y - half, 0, y, stroke});
                arrow.rules.push_back({head, y + half, 0, y, stroke});
            }
            if (right) {
                arrow.rules.push_back({width - head, y - half, width, y, stroke});
                arrow.rules.push_back({width - head, y + half, width, y, stroke});
            }
            if (under)
                arrow.descent = y + half + stroke;
            else
                arrow.ascent = -y + half + stroke;
            return arrow;
        }
        const float stroke = std::max(.6f, size * metrics_.overbar_rule_thickness),
                    y = -box.ascent - size * metrics_.overbar_gap - stroke / 2;
        if (cmd == L"underline") {
            const float weight = std::max(.6f, size * metrics_.underbar_rule_thickness),
                        bottom = box.descent + size * metrics_.underbar_gap + weight / 2;
            box.rules.push_back({0, bottom, box.width, bottom, weight});
            box.descent = bottom + weight;
        } else if (cmd == L"hat" || cmd == L"widehat") {
            box.rules.push_back({0, y, box.width / 2, y - size * .15f, stroke});
            box.rules.push_back({box.width / 2, y - size * .15f, box.width, y, stroke});
            box.ascent = -y + size * .15f + stroke;
        } else if (cmd == L"bar" || cmd == L"overline" || cmd == L"vec") {
            box.rules.push_back({0, y, box.width, y, stroke});
            if (cmd == L"vec") {
                box.rules.push_back({box.width - size * .2f, y - size * .12f, box.width, y, stroke});
                box.rules.push_back({box.width - size * .2f, y + size * .12f, box.width, y, stroke});
            }
            box.ascent = -y + (cmd == L"vec" ? size * .12f : 0) + stroke;
        } else {
            const std::wstring_view mark = cmd == L"dot"        ? L"˙"
                                           : cmd == L"ddot"     ? L"¨"
                                           : cmd == L"acute"    ? L"´"
                                           : cmd == L"grave"    ? L"`"
                                           : cmd == L"breve"    ? L"˘"
                                           : cmd == L"check"    ? L"ˇ"
                                           : cmd == L"mathring" ? L"˚"
                                                                : L"~";
            if (!HasGlyph(static_cast<UINT32>(mark[0]))) {
                ok_ = false;
                return {};
            }
            const Box accent = Text(mark, size * .8f, context, true, true);
            Ink ink{};
            if (!ContentBounds(box, ink)) {
                ok_ = false;
                return {};
            }
            Box accented;
            const float width = std::max(box.width, accent.width);
            Put(accented, box, (width - box.width) / 2);
            Put(accented, accent, (width - accent.width) / 2, ink.top - size * .08f - accent.descent);
            box = std::move(accented);
        }
        return box;
    }
    Box Build(const MathNode &node, Context context) {
        if (!ok_ || ++visited_ > 4096 || depth_ >= 64) {
            ok_ = false;
            return {};
        }
        struct DepthScope {
            size_t &depth;
            explicit DepthScope(size_t &value) : depth(value) { ++depth; }
            ~DepthScope() { --depth; }
        } depth_scope(depth_);
        switch (node.kind) {
        case Kind::Row:
            return Row(node, context);
        case Kind::Space: {
            Box box;
            box.width = node.text == L"hspace" ? Length(node.length, context) : node.space_em * Size(context);
            return box;
        }
        case Kind::Phantom: {
            if (node.children.size() != 1) {
                ok_ = false;
                return {};
            }
            Box box = Build(node.children[0], context);
            if (!ContainInk(box)) {
                ok_ = false;
                return {};
            }
            box.glyphs.clear();
            box.rules.clear();
            box.intentional_space = true;
            if (node.text == L"hphantom")
                box.ascent = box.descent = 0;
            else if (node.text == L"vphantom")
                box.width = 0;
            return box;
        }
        case Kind::Brace:
            return Brace(node, context);
        case Kind::Middle:
            // Middle is only meaningful in the directly enclosing Fence row.
            ok_ = false;
            return {};
        case Kind::Style:
            if (node.children.empty())
                return {};
            if (node.style != Style::Inherit)
                context.style = node.style;
            if (node.text == L"mathit") {
                context.roman = false;
                context.alphabet = context.bold_symbols ? MathAlphabet::BoldItalic : MathAlphabet::Default;
                context.bold = context.bold_symbols;
            } else if (node.text == L"mathrm" || node.text == L"mathbf") {
                context.roman = true;
                context.alphabet = MathAlphabet::Default;
                context.bold = node.text == L"mathbf" || context.bold_symbols;
            } else if (node.text == L"boldsymbol" || node.text == L"bm") {
                context.bold_symbols = true;
                context.bold = true;
                if (context.alphabet == MathAlphabet::Default && !context.roman)
                    context.alphabet = MathAlphabet::BoldItalic;
            } else if (node.text == L"mathcal" || node.text == L"mathscr" || node.text == L"mathfrak" ||
                node.text == L"mathsf" || node.text == L"mathtt") {
                context.alphabet = node.text == L"mathcal" || node.text == L"mathscr" ? MathAlphabet::Script :
                    node.text == L"mathfrak" ? MathAlphabet::Fraktur :
                    node.text == L"mathsf" ? MathAlphabet::SansSerif : MathAlphabet::Monospace;
                context.roman = true;
                context.bold = context.bold_symbols;
            }
            return Build(node.children[0], context);
        case Kind::Glyph: {
            if (node.require_glyph && !CheckGlyphs(node.text)) {
                ok_ = false;
                return {};
            }
            context.bold = context.bold || node.bold;
            if (node.auxiliary == L"mathbb")
                context.bold = false;
            const float size = Size(context);
            if (node.auxiliary == L"sized-delimiter") {
                const Box glyph = Text(node.text, size * node.space_em, context);
                Box box;
                if (!node.text.empty())
                    Put(box, glyph, 0, (glyph.ascent - glyph.descent) / 2 - size * metrics_.axis_height);
                return box;
            }
            if (context.alphabet != MathAlphabet::Default && node.auxiliary != L"mathbb" && node.auxiliary != L"text") {
                std::wstring mapped;
                if (!MapMathAlphabet(node.text, context.alphabet, mapped, context.bold_symbols) || !CheckGlyphs(mapped)) {
                    ok_ = false;
                    return {};
                }
                if (mapped != node.text) {
                    // The Unicode glyph already encodes its slant and weight.
                    context.roman = true;
                    context.bold = context.alphabet == MathAlphabet::Monospace && context.bold_symbols;
                    return Text(mapped, size, context);
                }
            }
            return Text(node.text, size, context, node.upright);
        }
        case Kind::Operator: {
            const float size = Size(context);
            if (node.text == L"bmod")
                return Text(L"mod", size, context);
            if (node.text == L"pmod") {
                if (node.children.size() != 1) {
                    ok_ = false;
                    return {};
                }
                const Box argument = Build(node.children[0], context);
                Box box;
                Put(box, Text(L"(", size, context), size * (context.style == Style::Display ? 1.0f : .45f));
                Put(box, Text(L"mod", size, context), box.width);
                Put(box, argument, box.width + size * .33f);
                Put(box, Text(L")", size, context), box.width);
                return box;
            }
            return Text(node.text, size * (node.large_operator && context.style == Style::Display ? 1.45f : 1),
                        context);
        }
        case Kind::Script:
            return Scripts(node, context);
        case Kind::Fraction:
            return Fraction(node, context);
        case Kind::Radical:
            return Radical(node, context);
        case Kind::Fence:
            return Fence(node, context);
        case Kind::Environment:
            return Environment(node, context);
        case Kind::Accent:
            return Accent(node, context);
        case Kind::Decoration:
            return Decoration(node, context);
        case Kind::Arrow:
            return Arrow(node, context);
        }
        ok_ = false;
        return {};
    }
};
} // namespace

bool BuildMathLayout(IDWriteFactory2 *factory, IDWriteFontFace *face, const FontMathMetrics &metrics,
                     const MathNode &root, float font_size, bool display, FormulaLayout &result) {
    if (!factory || !face || !std::isfinite(font_size) || font_size < 4 || font_size > 256)
        return false;
    return Layout(factory, face, metrics, font_size).Run(root, display, result);
}
} // namespace pulse::ui::math
