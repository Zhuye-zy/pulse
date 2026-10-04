#include "math_formula_architecture_test.h"
#include "../ui/math_formula.h"
#include "../ui/math_font_metrics.h"
#include "../ui/math_layout.h"
#include "../ui/math_syntax.h"
#include "../ui/markdown_view.h"
#include "../preview_host/markdown_document.h"
#include <wrl/client.h>
#include <cmath>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace pulse_test {
using Microsoft::WRL::ComPtr;
namespace {
struct Case { std::string id, expectation; bool display; std::wstring source; };
std::wstring Utf8(std::string_view text) {
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}
std::vector<Case> ReadCorpus() {
    std::ifstream file("tools/math_reference/corpus.tsv", std::ios::binary);
    std::vector<Case> cases;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t a = line.find('\t'), b = line.find('\t', a == std::string::npos ? line.size() : a + 1),
            c = line.find('\t', b == std::string::npos ? line.size() : b + 1);
        if (a == std::string::npos || b == std::string::npos || c == std::string::npos) continue;
        if (line.substr(0, a) == "id") continue;
        cases.push_back({line.substr(0, a), line.substr(b + 1, c - b - 1), line.substr(a + 1, b - a - 1) == "1", Utf8(line.substr(c + 1))});
    }
    return cases;
}
struct Measurement { bool valid = false; float width = 0, height = 0, baseline = 0; };
ComPtr<IDWriteTextLayout> Layout(IDWriteFactory2* factory, std::wstring_view source) {
    ComPtr<IDWriteTextFormat> format;
    ComPtr<IDWriteTextLayout> layout;
    if (SUCCEEDED(factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 24, L"en-US", &format)))
        factory->CreateTextLayout(source.data(), static_cast<UINT32>(source.size()), format.Get(), 20000, 3000, &layout);
    return layout;
}
Measurement Measure(IDWriteFactory2* factory, std::wstring_view source, bool display = false) {
    auto layout = Layout(factory, source);
    ComPtr<IDWriteInlineObject> object;
    DWRITE_INLINE_OBJECT_METRICS metrics{};
    const bool valid = layout && pulse::ui::ApplyMathInline(factory, layout.Get(), {0, static_cast<UINT32>(source.size())},
        source, 24, display, 20000) && SUCCEEDED(layout->GetInlineObject(0, &object)) && object && SUCCEEDED(object->GetMetrics(&metrics));
    return {valid, metrics.width, metrics.height, metrics.baseline};
}
std::string Structures(std::wstring_view source) {
    using pulse::ui::MathNodeKind;
    const auto syntax = pulse::ui::ParseMathSyntax(source);
    if (!syntax) return {};
    constexpr std::array kinds{MathNodeKind::Fraction, MathNodeKind::Radical, MathNodeKind::Script,
        MathNodeKind::Environment, MathNodeKind::Fence, MathNodeKind::Accent, MathNodeKind::Decoration, MathNodeKind::Arrow,
        MathNodeKind::Brace, MathNodeKind::Phantom};
    constexpr std::array names{"fraction", "radical", "script", "environment", "fence", "accent", "decoration", "arrow", "brace", "phantom"};
    std::array<size_t, 10> counts{};
    const auto count = [&kinds, &counts](const auto& self, const pulse::ui::MathNode& node) -> void {
        for (size_t i = 0; i < kinds.size(); ++i) if (node.kind == kinds[i]) ++counts[i];
        for (const auto& child : node.children) self(self, child);
    };
    count(count, syntax.root);
    std::string result;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) result += ',';
        result += std::string(names[i]) + ':' + std::to_string(counts[i]);
    }
    return result;
}
}
const std::vector<std::wstring>& MathArchitectureFormulas() {
    // Rendering is a twelve-case sample; TestMathArchitecture checks the entire
    // shared corpus and writes every AST summary for the independent comparator.
    static const std::vector<std::wstring> formulas = [] {
        std::vector<std::wstring> result;
        for (const auto& item : ReadCorpus()) {
            if (item.expectation == "native_accept") result.push_back(item.source);
            if (result.size() == 12) break;
        }
        return result;
    }();
    return formulas;
}
const std::wstring& MathArchitectureSource() {
    static const std::wstring source = [] {
        std::wstring result = L"# Mathematical layout architecture\n\nInline \\(a+b=c\\) must be a single rendered object and preserve source when copied.\n\n";
        size_t count = 0;
        for (const auto& formula : MathArchitectureFormulas()) {
            result += L"\\[" + formula + L"\\]\n\n";
            if (++count == 8) break;
        }
        result += L"- Binary \\(a+b\\), relation \\(a=b\\), unary \\(-a\\).\n- Script \\(x_{a+b}^{c=d}\\).\n";
        return result;
    }();
    return source;
}
int TestMathArchitecture(IDWriteFactory2* factory, bool compatibility_only) {
    int failures = 0;
    const auto check = [&failures](bool valid, const char* name) {
        std::printf("[%s] architecture %s\n", valid ? "PASS" : "FAIL", name);
        if (!valid) ++failures;
    };
    const auto cases = ReadCorpus();
    check(cases.size() >= 20, "shared independent-reference corpus loads at least twenty cases");
    std::filesystem::create_directories("build");
    std::ofstream report("build/math-native-compat.tsv", std::ios::binary);
    report << "id\texpectation\tnative_accepted\twidth\theight\tbaseline\tnative_structures\n";
    size_t accepted = 0, rejected = 0;
    for (const auto& item : cases) {
        const auto measured = Measure(factory, item.source, item.display);
        const bool wanted = item.expectation == "native_accept";
        bool valid = !item.source.empty() && (wanted || item.expectation == "native_fallback" || item.expectation == "invalid") &&
            measured.valid == wanted;
        if (wanted) {
            ++accepted;
            valid = valid && std::isfinite(measured.width) && std::isfinite(measured.height) && measured.width > 0 &&
                measured.height > 0 && measured.baseline > 0 && measured.baseline <= measured.height;
        } else {
            ++rejected;
            auto layout = Layout(factory, item.source);
            DWRITE_TEXT_METRICS before{}, after{};
            ComPtr<IDWriteInlineObject> object;
            valid = valid && layout && SUCCEEDED(layout->GetMetrics(&before)) &&
                !pulse::ui::ApplyMathInline(factory, layout.Get(), {0, static_cast<UINT32>(item.source.size())}, item.source, 24, item.display, 20000) &&
                SUCCEEDED(layout->GetInlineObject(0, &object)) && !object && SUCCEEDED(layout->GetMetrics(&after)) &&
                std::abs(before.width - after.width) < 0.01f && std::abs(before.height - after.height) < 0.01f;
        }
        check(valid, item.id.c_str());
        report << item.id << '\t' << item.expectation << '\t' << measured.valid << '\t' << measured.width << '\t'
            << measured.height << '\t' << measured.baseline << '\t' << Structures(item.source) << '\n';
    }
    check(accepted >= 10 && rejected >= 3, "corpus covers native rendering and deliberate fallback");
    check(report.good(), "native compatibility measurements saved for reference comparison");
    std::printf("[COVERAGE] Acceptance and AST report: all %zu corpus cases (%zu native, %zu fallback/error). "
        "Formula PNGs: first 12 native cases; Markdown PNGs: first 8. No full-corpus image equivalence claim.\n",
        cases.size(), accepted, rejected);
    if (compatibility_only) return failures;
    ComPtr<IDWriteFontCollection> collection;
    ComPtr<IDWriteFontFamily> family;
    ComPtr<IDWriteFont> font;
    ComPtr<IDWriteFontFace> face;
    UINT32 family_index = 0;
    BOOL exists = FALSE;
    const bool font_found = SUCCEEDED(factory->GetSystemFontCollection(&collection)) &&
        SUCCEEDED(collection->FindFamilyName(L"Cambria Math", &family_index, &exists)) && exists &&
        SUCCEEDED(collection->GetFontFamily(family_index, &family)) &&
        SUCCEEDED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font)) &&
        SUCCEEDED(font->CreateFontFace(&face));
    const auto font_metrics = pulse::ui::math::ReadFontMathMetrics(face.Get());
    check(font_found && font_metrics.from_font && font_metrics.script_scale > font_metrics.script_script_scale &&
        font_metrics.axis_height > 0 && font_metrics.fraction_rule_thickness > 0,
        "installed Cambria Math MATH constants are actually read rather than fallback defaults");
    const auto geometry = [factory, &face](std::wstring_view source, const pulse::ui::math::FontMathMetrics& metrics,
        pulse::ui::math::FormulaLayout& result) {
        const auto syntax = pulse::ui::ParseMathSyntax(source);
        return syntax && pulse::ui::math::BuildMathLayout(factory, face.Get(), metrics, syntax.root, 24, false, result);
    };
    // Inter-atom advance is a layout property. Inline-object metrics add one
    // outer padding on each side, which must not be counted once per atom.
    const auto width = [&geometry, &font_metrics](std::wstring_view source) {
        pulse::ui::math::FormulaLayout result;
        const bool valid = geometry(source, font_metrics, result);
        return Measurement{valid, result.width, result.ascent + result.descent, result.ascent};
    };
    auto changed_metrics = font_metrics;
    changed_metrics.axis_height += 0.1f;
    changed_metrics.fraction_rule_thickness += 0.1f;
    changed_metrics.radical_rule_thickness += 0.1f;
    pulse::ui::math::FormulaLayout fraction_original, fraction_changed, radical_original, radical_changed;
    const bool fraction_geometry = geometry(LR"(\frac{a}{b})", font_metrics, fraction_original) &&
        geometry(LR"(\frac{a}{b})", changed_metrics, fraction_changed);
    check(fraction_geometry && fraction_original.rules.size() == 1 && fraction_changed.rules.size() == 1 &&
        fraction_changed.rules[0].width > fraction_original.rules[0].width + 1 &&
        std::abs(fraction_changed.rules[0].y1 - fraction_original.rules[0].y1) > 1,
        "font fraction rule and axis constants actually change layout geometry");
    const bool radical_geometry = geometry(LR"(\sqrt{x})", font_metrics, radical_original) &&
        geometry(LR"(\sqrt{x})", changed_metrics, radical_changed);
    check(radical_geometry && !radical_original.rules.empty() && !radical_changed.rules.empty() &&
        radical_changed.rules[0].width > radical_original.rules[0].width + 1,
        "font radical rule constant actually changes drawn radical strokes");
    changed_metrics = font_metrics;
    changed_metrics.script_scale *= 0.8f;
    pulse::ui::math::FormulaLayout script_original, script_changed;
    float original_script_size = 0, changed_script_size = 0;
    const bool script_geometry = geometry(L"x_i", font_metrics, script_original) &&
        geometry(L"x_i", changed_metrics, script_changed);
    check(script_geometry && script_original.glyphs.size() == 2 && script_changed.glyphs.size() == 2 &&
        SUCCEEDED(script_original.glyphs[1].layout->GetFontSize(0, &original_script_size)) &&
        SUCCEEDED(script_changed.glyphs[1].layout->GetFontSize(0, &changed_script_size)) &&
        changed_script_size < original_script_size - 1,
        "font script percentage changes the actual shaped script glyph size");
    changed_metrics = font_metrics;
    changed_metrics.space_after_script += 0.1f;
    pulse::ui::math::FormulaLayout spacing_original, spacing_changed;
    const bool script_spacing = geometry(L"x^2", font_metrics, spacing_original) &&
        geometry(L"x^2", changed_metrics, spacing_changed);
    check(script_spacing && spacing_original.glyphs.size() == 2 && spacing_changed.glyphs.size() == 2 &&
        spacing_changed.width > spacing_original.width + 2 &&
        std::abs(spacing_changed.glyphs[1].x - spacing_original.glyphs[1].x) < 0.001f,
        "SpaceAfterScript changes trailing advance without moving the script glyph");
    pulse::ui::math::FormulaLayout empty_original, empty_changed, unscripted;
    const bool empty_spacing = geometry(L"x^{}", font_metrics, empty_original) &&
        geometry(L"x^{}", changed_metrics, empty_changed) && geometry(L"x", font_metrics, unscripted);
    check(empty_spacing && empty_original.glyphs.size() == 1 && empty_changed.glyphs.size() == 1 &&
        empty_changed.width > empty_original.width + 2 &&
        std::abs(empty_changed.glyphs[0].x - empty_original.glyphs[0].x) < 0.001f &&
        std::abs(empty_original.ascent - unscripted.ascent) < 0.001f &&
        std::abs(empty_original.descent - unscripted.descent) < 0.001f,
        "explicit empty script retains trailing space without adding glyphs or vertical extent");
    const auto a = width(L"a"), b = width(L"b"), plus = width(L"+"), minus = width(L"-"), equals = width(L"="), comma = width(L",");
    const auto binary = width(L"a+b"), relation = width(L"a=b"), unary = width(L"-a"), punctuation = width(L"a,b");
    const float binary_glue = binary.width - a.width - plus.width - b.width;
    const float relation_glue = relation.width - a.width - equals.width - b.width;
    check(a.valid && b.valid && plus.valid && binary.valid && binary_glue > 3,
        "binary operator receives visible inter-atom spacing");
    check(equals.valid && relation.valid && relation_glue > binary_glue + 1,
        "relation spacing is larger than binary spacing");
    check(minus.valid && unary.valid && std::abs(unary.width - minus.width - a.width) < 2,
        "leading unary minus does not receive binary spacing");
    check(comma.valid && punctuation.valid && punctuation.width - a.width - comma.width - b.width > 1,
        "punctuation leaves a following thin space");
    const auto open = width(L"("), close = width(L")"), parenthesized = width(L"(-a)");
    check(open.valid && close.valid && parenthesized.valid &&
        std::abs(parenthesized.width - open.width - minus.width - a.width - close.width) < 3,
        "minus after an opening delimiter stays unary");
    const auto after_relation = width(L"a=-b"), after_binary = width(L"a+-b");
    check(after_relation.valid && std::abs((after_relation.width - a.width - equals.width - minus.width - b.width) - relation_glue) < 2,
        "minus following a relation does not add another binary gap");
    check(after_binary.valid && std::abs((after_binary.width - a.width - plus.width - minus.width - b.width) - binary_glue) < 2,
        "minus following a binary operator stays unary");
    const auto sa = width(LR"(\scriptstyle a)"), sb = width(LR"(\scriptstyle b)"), sp = width(LR"(\scriptstyle +)"),
        script_binary = width(LR"(\scriptstyle a+b)");
    check(sa.valid && sb.valid && sp.valid && script_binary.valid &&
        std::abs(script_binary.width - sa.width - sp.width - sb.width) < 2,
        "binary spacing is suppressed in script style");
    std::printf("[SPACING] binary %.3f relation %.3f unary-extra %.3f script-extra %.3f DIP\n", binary_glue, relation_glue,
        unary.width - minus.width - a.width, script_binary.width - sa.width - sp.width - sb.width);
    std::wstring payload;
    pulse::ui::MarkdownView view;
    check(pulse::preview::MakeMarkdownDocument(MathArchitectureSource(), payload) &&
        view.SetPayload(payload, L"bench_data/math_preview.md") && view.Source() == MathArchitectureSource() &&
        view.PlainText().find(LR"(\(a+b=c\))") != std::wstring::npos,
        "complete corpus document preserves source and copy text");
    return failures;
}
}
