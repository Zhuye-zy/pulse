#include "math_formula_structures_test.h"
#include "../ui/math_formula.h"
#include "../ui/math_layout.h"
#include "../ui/markdown_view.h"
#include "../preview_host/markdown_document.h"
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pulse_test {
using Microsoft::WRL::ComPtr;
const MathRenderProfile& MathStructuresProfile() {
    static const MathRenderProfile profile = [] {
        MathRenderProfile result;
        result.title = L"Complex mathematical structures / complete gallery";
        result.file_prefix = L"math_preview-structures-";
        result.copy_probe = LR"(\(\overbrace{x+y}^{n}\))";
        result.markdown_height = 4000;
        result.formulas = {
            LR"(\begin{array}{|l||r|}\hline a&bbbb\\\hline\hline aaaa&b\\\hline\end{array})",
            LR"(\begin{array}{|c|c|}\hline\frac{a}{b}&\sqrt{x}\\[1em]\hline x^2&y_i\\\hline\end{array})",
            LR"(\begin{array}{c}a\\[2ex]b\\[6pt]c\end{array})",
            LR"(\left(\frac{a}{b}\middle|x\middle\|y\right))",
            LR"(\left\{x\middle|\left(\frac{a}{b}\middle|y\right)>0\right\})",
            LR"(\overbrace{a+b+c}^{n\text{ terms}})",
            LR"(\underbrace{a+b+c}_{n\text{ terms}})",
            LR"(\overbrace{x+y}^{n}_{i}+\underbrace{x+y}_{n}^{i})",
            LR"(\overbrace{a+\underbrace{b+c}_{k}}^{m})",
            LR"(A\phantom{\frac{x}{y}}B+A\hphantom{\frac{x}{y}}B+A\vphantom{\frac{x}{y}}B)",
            LR"(\begin{array}{rcl}a&=&b+c\\\phantom{a}&=&d\end{array})",
            LR"(a\hspace{1em}b+a\hspace{-0.25em}b+a\hspace{18pt}b)",
            LR"(\boxed{\left[\overbrace{a+b}^{n}\middle|\frac{x}{y}\right]})",
            LR"(\sum_{i=1}^{n}\underbrace{\frac{x_i}{1+x_i}}_{\text{bounded}})",
            LR"(\phantom{\frac{x}{y}})", LR"(\hphantom{abc})", LR"(\vphantom{\frac{x}{y}})"
        };
        result.markdown = L"# Complex mathematical structures\n\nInline " + result.copy_probe + L" retains its full source.\n\n";
        for (size_t i = 0; i < result.formulas.size(); ++i)
            result.markdown += L"### Case " + std::to_wstring(i + 1) + L"\n\n\\[" + result.formulas[i] + L"\\]\n\n";
        result.markdown += L"Pure phantom cases intentionally reserve blank space.\n\nCode `\\overbrace{x}^{n}` remains literal.\n";
        return result;
    }();
    return profile;
}
int TestMathStructures(IDWriteFactory2* factory) {
    using namespace pulse::ui;
    using namespace pulse::ui::math;
    int failures = 0;
    const auto check = [&failures](bool valid, const char* name) {
        std::printf("[%s] structures %s\n", valid ? "PASS" : "FAIL", name);
        if (!valid) ++failures;
    };
    ComPtr<IDWriteFontCollection> collection;
    ComPtr<IDWriteFontFamily> family;
    ComPtr<IDWriteFont> font;
    ComPtr<IDWriteFontFace> face;
    UINT32 index = 0;
    BOOL exists = FALSE;
    const bool found = SUCCEEDED(factory->GetSystemFontCollection(&collection)) &&
        SUCCEEDED(collection->FindFamilyName(L"Cambria Math", &index, &exists)) && exists &&
        SUCCEEDED(collection->GetFontFamily(index, &family)) &&
        SUCCEEDED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font)) &&
        SUCCEEDED(font->CreateFontFace(&face));
    check(found, "load actual mathematical font for geometry validation");
    if (!found) return failures;
    const auto metrics = ReadFontMathMetrics(face.Get());
    const auto build = [&](std::wstring_view source, FormulaLayout& layout) {
        const auto syntax = ParseMathSyntax(source);
        return syntax && BuildMathLayout(factory, face.Get(), metrics, syntax.root, 24, false, layout);
    };
    const auto close = [](float a, float b) { return std::abs(a - b) < 0.05f; };
    for (const auto& source : MathStructuresProfile().formulas) {
        FormulaLayout layout;
        check(build(source, layout) && std::isfinite(layout.width) && std::isfinite(layout.ascent) &&
            std::isfinite(layout.descent) && layout.width >= 0 && layout.ascent >= 0 && layout.descent >= 0,
            "complete gallery formula produces finite geometry including invisible structures");
    }
    FormulaLayout body, phantom, horizontal, vertical;
    const bool phantoms = build(LR"(\frac{x}{y})", body) && build(LR"(\phantom{\frac{x}{y}})", phantom) &&
        build(LR"(\hphantom{\frac{x}{y}})", horizontal) && build(LR"(\vphantom{\frac{x}{y}})", vertical);
    check(phantoms && close(phantom.width, body.width) && close(phantom.ascent, body.ascent) && close(phantom.descent, body.descent) &&
        phantom.glyphs.empty() && phantom.rules.empty(), "phantom preserves all dimensions without visible drawing primitives");
    check(phantoms && close(horizontal.width, body.width) && close(horizontal.ascent, 0) && close(horizontal.descent, 0) &&
        horizontal.glyphs.empty() && horizontal.rules.empty(), "hphantom preserves only width and draws no ink");
    check(phantoms && close(vertical.width, 0) && close(vertical.ascent, body.ascent) && close(vertical.descent, body.descent) &&
        vertical.glyphs.empty() && vertical.rules.empty(), "vphantom preserves only vertical extent and draws no ink");
    FormulaLayout ordinary, positive, negative, points, ex_space;
    const bool spaces = build(L"ab", ordinary) && build(LR"(a\hspace{1em}b)", positive) &&
        build(LR"(a\hspace{-0.25em}b)", negative) && build(LR"(a\hspace{18pt}b)", points) && build(LR"(a\hspace{1ex}b)", ex_space);
    DWRITE_FONT_METRICS font_dimensions{};
    face->GetMetrics(&font_dimensions);
    const float expected_ex = 24.0f * static_cast<float>(font_dimensions.xHeight) / static_cast<float>(font_dimensions.designUnitsPerEm);
    check(spaces && close(positive.width - ordinary.width, 24) && close(points.width - ordinary.width, 18 * 96.0f / 72.27f) &&
        close(ex_space.width - ordinary.width, expected_ex) && negative.width < ordinary.width - 2,
        "signed hspace advances and physical point conversion affect actual geometry");
    FormulaLayout ruled;
    bool rules_ok = build(MathStructuresProfile().formulas[0], ruled);
    size_t vertical_rules = 0, horizontal_rules = 0;
    for (const auto& rule : ruled.rules) {
        vertical_rules += close(rule.x1, rule.x2) && std::abs(rule.y2 - rule.y1) > 1;
        horizontal_rules += close(rule.y1, rule.y2) && std::abs(rule.x2 - rule.x1) > 1;
    }
    check(rules_ok && vertical_rules == 4 && horizontal_rules == 4,
        "single and double array boundaries create all four vertical and four horizontal rules");
    float right_border = 0, right_horizontal = 0;
    for (const auto& rule : ruled.rules) {
        if (close(rule.x1, rule.x2)) right_border = std::max(right_border, rule.x1 + rule.width / 2);
        else if (close(rule.y1, rule.y2)) right_horizontal = std::max(right_horizontal, rule.x2);
    }
    check(rules_ok && close(right_border, right_horizontal),
        "outer vertical border meets horizontal rule endpoints without inset or protrusion");
    FormulaLayout standard_rows, spaced_rows;
    const bool rows = build(LR"(\begin{array}{c}a\\b\end{array})", standard_rows) &&
        build(LR"(\begin{array}{c}a\\[1em]b\end{array})", spaced_rows);
    check(rows && standard_rows.glyphs.size() == 2 && spaced_rows.glyphs.size() == 2 &&
        close((spaced_rows.glyphs[1].y - spaced_rows.glyphs[0].y) - (standard_rows.glyphs[1].y - standard_rows.glyphs[0].y), 24),
        "explicit row gap increases baseline separation by requested amount");
    FormulaLayout bare, over, under, over_label, under_label;
    const bool braces = build(L"x+y", bare) && build(LR"(\overbrace{x+y})", over) && build(LR"(\underbrace{x+y})", under) &&
        build(LR"(\overbrace{x+y}^{n})", over_label) && build(LR"(\underbrace{x+y}_{n})", under_label);
    check(braces && over.ascent > bare.ascent && under.descent > bare.descent &&
        over_label.ascent > over.ascent && under_label.descent > under.descent,
        "braces and corresponding labels extend the correct side of baseline");
    FormulaLayout long_label;
    const auto rule_span = [](const FormulaLayout& layout) {
        if (layout.rules.empty()) return 0.0f;
        float left = layout.rules[0].x1, right = left;
        for (const auto& rule : layout.rules) {
            left = std::min(left, std::min(rule.x1, rule.x2));
            right = std::max(right, std::max(rule.x1, rule.x2));
        }
        return right - left;
    };
    check(braces && build(LR"(\overbrace{x+y}^{abcdefghijklmno})", long_label) &&
        long_label.width > over.width + 10 && close(rule_span(long_label), rule_span(over)),
        "long brace label expands formula width without stretching the brace over its body");
    FormulaLayout fenced, ordinary_bar;
    const bool delimiters = build(LR"(\left(\frac{a}{b}\middle|x\right))", fenced) && build(L"|", ordinary_bar);
    float ordinary_size = 0;
    if (!ordinary_bar.glyphs.empty()) ordinary_bar.glyphs[0].layout->GetFontSize(0, &ordinary_size);
    std::vector<float> large_delimiters;
    for (const auto& glyph : fenced.glyphs) {
        float size = 0;
        if (SUCCEEDED(glyph.layout->GetFontSize(0, &size)) && size > ordinary_size + 1) large_delimiters.push_back(size);
    }
    check(delimiters && large_delimiters.size() == 3 && close(large_delimiters[0], large_delimiters[1]) &&
        close(large_delimiters[1], large_delimiters[2]), "left middle and right delimiters share the same stretched size");
    FormulaLayout ordinary_line, phantom_line;
    check(build(LR"(\overline{x})", ordinary_line) && build(LR"(\overline{x\vphantom{\frac{a}{b}}})", phantom_line) &&
        ordinary_line.rules.size() == 1 && phantom_line.rules.size() == 1 && phantom_line.glyphs.size() == 1 &&
        phantom_line.rules[0].y1 < ordinary_line.rules[0].y1 - 1 && close(phantom_line.width, ordinary_line.width),
        "vphantom raises an overline while adding neither width nor phantom glyphs");
    FormulaLayout scoped, reset;
    check(build(LR"(\scriptstyle\overbrace{x}^{n}y)", scoped) &&
        build(LR"(\scriptstyle\overbrace{x}^{n}\scriptstyle y)", reset) && close(scoped.width, reset.width) &&
        close(scoped.ascent, reset.ascent), "brace annotation restores surrounding script style");
    const std::vector<std::wstring> invalid = {LR"(\begin{array}{|||c}x\end{array})", LR"(\begin{array}{p{1em}}x\end{array})",
        LR"(\begin{array}{@{}c}x\end{array})", LR"(\begin{array}{c}x\hline\end{array})", LR"(\begin{array}{c}\hline\hline\hline x\end{array})",
        LR"(\begin{array}{c}x\\[-1em]y\end{array})", LR"(\begin{array}{c}x\\[1px]y\end{array})", LR"(\middle|x)",
        LR"(\left({x\middle|y}\right))", LR"(\hspace{1px})", LR"(\hspace{1e2em})", LR"(\hspace{nanem})",
        LR"(\hspace{21em})", LR"(\hspace{41ex})", LR"(\hspace{201pt})", LR"(\phantom)", LR"(\overbrace)", LR"(\quad)", LR"(\hspace{1em})"};
    ComPtr<IDWriteTextFormat> format;
    factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 24, L"en-US", &format);
    for (const auto& source : invalid) {
        ComPtr<IDWriteTextLayout> layout;
        ComPtr<IDWriteInlineObject> object;
        if (format) factory->CreateTextLayout(source.data(), static_cast<UINT32>(source.size()), format.Get(), 10000, 2000, &layout);
        check(layout && !ApplyMathInline(factory, layout.Get(), {0, static_cast<UINT32>(source.size())}, source, 24, false, 10000) &&
            SUCCEEDED(layout->GetInlineObject(0, &object)) && !object, "invalid complex structure or units leave original layout unchanged");
    }
    std::wstring payload;
    MarkdownView view;
    const auto& profile = MathStructuresProfile();
    check(pulse::preview::MakeMarkdownDocument(profile.markdown, payload) && view.SetPayload(payload, L"bench_data/math_preview.md") &&
        view.Source() == profile.markdown && view.PlainText().find(profile.copy_probe) != std::wstring::npos,
        "complete structure gallery preserves original source and copied formula delimiters");
    std::printf("[COVERAGE] Complete structures gallery: all %zu cases, no first-N sampling.\n", profile.formulas.size());
    return failures;
}
}
