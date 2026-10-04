#include "math_formula_reliable_test.h"
#include "../ui/math_formula.h"
#include "../ui/math_layout.h"
#include "../ui/math_alphabet.h"
#include "../ui/markdown_view.h"
#include "../preview_host/markdown_document.h"
#include <wrl/client.h>
#include <cmath>
#include <cstdio>

namespace pulse_test {
using Microsoft::WRL::ComPtr;
const MathRenderProfile& MathReliableProfile() {
    static const MathRenderProfile profile = [] {
        MathRenderProfile result;
        result.title = L"Reliable Markdown mathematics / macros and mathematical alphabets";
        result.file_prefix = L"math_preview-reliable-";
        result.copy_probe = LR"(\(\newcommand{\sq}[1]{#1^2}\sq{x}\))";
        result.markdown_height = 5000;
        result.formulas = {
            LR"(\newcommand{\sq}[1]{#1^2}\sq{x}+\sq{y}=r^2)",
            LR"(\newcommand{\pow}[2][2]{#2^{#1}}\pow{x}+\pow[3]{y})",
            LR"(\newcommand{\pair}[2]{\left(#1,#2\right)}\pair{a+b}{\frac{x}{y}})",
            LR"(\newcommand{\f}[1]{\g{#1}}\newcommand{\g}[1]{\sqrt{#1}}\f{x^2+y^2})",
            LR"(\newcommand{\f}{x}{\renewcommand{\f}{y}\f}+\f)",
            LR"(\newcommand{\f}{x}\providecommand{\f}{y}\f+x)",
            LR"(\providecommand{\frac}[2]{#1+#2}\frac{a}{b})",
            LR"(\newcommand{\vectx}[1]{\bm{#1}}\vectx{\alpha}+\vectx{x}=\vectx{y})",
            LR"(\mathcal{ABC}+\mathscr{DEF}+\mathscr{GHI})",
            LR"(\mathfrak{ABCxyz}+\mathsf{ABC123}+\mathtt{ABC123})",
            LR"(\boldsymbol{\alpha+\beta}+\bm{x+y+123})",
            LR"(\mathcal{A\mathfrak{B}C}+\mathsf{x\mathtt{y}z})",
            LR"(\mathcal{\bm{ABC}}+\mathfrak{\bm{ABC}}+\mathsf{\bm{ABC}})",
            LR"(\newcommand{\E}[1]{\mathcal{E}_{#1}}\E{n}=\sum_{i=1}^{n}\mathsf{x}_i)",
            LR"(\newcommand{\op}[1][x]{\operatorname*{argmax}_{#1}}\op f(x)+\op[y]g(y))",
            LR"(\newcommand{\M}[2]{\begin{pmatrix}#1&0\\0&#2\end{pmatrix}}\M{\mathfrak{a}}{\mathfrak{b}})",
            LR"(\mathsf{\text{plain text}}+\mathcal{\textbf{plain bold}})",
            LR"(\newcommand{\br}[1]{\overbrace{#1}^{\mathcal{S}}}\br{a+b+c})"
        };
        result.markdown = L"# Reliable Markdown mathematics\n\nInline " + result.copy_probe + L" preserves source definitions when copied.\n\n";
        for (size_t i = 0; i < result.formulas.size(); ++i)
            result.markdown += L"### Case " + std::to_wstring(i + 1) + L"\n\n\\[" + result.formulas[i] + L"\\]\n\n";
        result.markdown += L"Macros are isolated to each formula. Code `\\newcommand{\\x}{y}` remains literal.\n";
        return result;
    }();
    return profile;
}
int TestMathReliable(IDWriteFactory2* factory) {
    using namespace pulse::ui;
    using namespace pulse::ui::math;
    int failures = 0;
    const auto check = [&failures](bool valid, const char* name) {
        std::printf("[%s] reliable %s\n", valid ? "PASS" : "FAIL", name);
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
    check(found, "load real mathematical font for scalar and layout verification");
    if (!found) return failures;
    const auto metrics = ReadFontMathMetrics(face.Get());
    const auto build = [&](std::wstring_view source, FormulaLayout& layout) {
        const auto syntax = ParseMathSyntax(source);
        return syntax && BuildMathLayout(factory, face.Get(), metrics, syntax.root, 24, false, layout);
    };
    const auto same_geometry = [&](std::wstring_view left, std::wstring_view right) {
        FormulaLayout a, b;
        if (!build(left, a) || !build(right, b) || a.glyphs.size() != b.glyphs.size() || a.rules.size() != b.rules.size() ||
            std::abs(a.width - b.width) > 0.05f || std::abs(a.ascent - b.ascent) > 0.05f || std::abs(a.descent - b.descent) > 0.05f) return false;
        for (size_t i = 0; i < a.glyphs.size(); ++i)
            if (std::abs(a.glyphs[i].x - b.glyphs[i].x) > 0.05f || std::abs(a.glyphs[i].y - b.glyphs[i].y) > 0.05f) return false;
        return true;
    };
    for (const auto& source : MathReliableProfile().formulas) {
        FormulaLayout layout;
        check(build(source, layout) && std::isfinite(layout.width) && layout.width > 0 && !layout.glyphs.empty(),
            "complete macro and alphabet gallery formula produces visible geometry");
    }
    check(same_geometry(LR"(\newcommand{\sq}[1]{#1^2}\sq{x})", L"x^2"), "required macro arguments produce the same geometry as literal expansion");
    check(same_geometry(LR"(\newcommand{\sq}[1]{#1^2}\sq𝒜)", L"𝒜^2") &&
        same_geometry(LR"(\newcommand{\twice}[1]{#1+#1}\twice𝔄)", L"𝔄+𝔄"),
        "unbraced supplementary argument shapes identically to a complete literal scalar");
    check(same_geometry(LR"(\newcommand{\sq}[1]{#1^2}\sq xyz)", L"x^2yz"),
        "single BMP argument preserves following literal letters");
    check(same_geometry(LR"(\newcommand{\pow}[2][2]{#2^{#1}}\pow{x}+\pow[3]{y})", L"x^2+y^3"),
        "optional argument defaults and overrides produce literal-equivalent geometry");
    check(same_geometry(LR"(\newcommand{\f}{x}{\renewcommand{\f}{y}\f}+\f)", L"{y}+x"),
        "group-scoped redefinition restores outer macro");
    check(same_geometry(LR"(\newcommand{\f}{a}\begin{matrix}\renewcommand{\f}{b}\f&1\\0&\f\end{matrix}+\f)",
        LR"(\begin{matrix}b&1\\0&a\end{matrix}+a)"),
        "cell-local macro renewal matches literal matrix entries and outer expression");
    check(same_geometry(LR"(\newcommand{\f}{a}\begin{matrix}\renewcommand{\f}{b}\f\\\f\end{matrix}+\f)",
        LR"(\begin{matrix}b\\a\end{matrix}+a)"), "row-local macro renewal matches literal next-row restoration");
    check(same_geometry(LR"(\newcommand{\f}{a}\begin{matrix}{\renewcommand{\f}{b}\f}+\f&\f\end{matrix})",
        LR"(\begin{matrix}{b}+a&a\end{matrix})"), "nested group macro renewal does not leak into its own cell or adjacent cell");
    check(same_geometry(LR"(\newcommand{\f}{a}\begin{matrix}\renewcommand{\f}{b}\begin{matrix}\renewcommand{\f}{c}\f&\f\end{matrix}+\f&\f\end{matrix}+\f)",
        LR"(\begin{matrix}\begin{matrix}c&b\end{matrix}+b&a\end{matrix}+a)"),
        "nested matrix restores inherited outer-cell override in literal-equivalent geometry");
    check(same_geometry(LR"(\newcommand{\f}{a}\substack{\renewcommand{\f}{b}\f\\\f}+\f)", LR"(\substack{b\\a}+a)"),
        "substack row-scoped macro renewal matches literal restored rows");
    check(same_geometry(LR"(\providecommand{\frac}[2]{#1+#2}\frac{a}{b})", LR"(\frac{a}{b})"),
        "providecommand cannot replace an existing builtin");
    check(same_geometry(LR"(\newcommand{\vectx}[1]{\bm{#1}}\mathcal{\vectx{A}})", LR"(\mathcal{\bm{A}})"),
        "macro expansion preserves surrounding alphabet and bold modifier");
    check(same_geometry(LR"(\mathcal{ABC})", LR"(\mathscr{ABC})"),
        "script family aliases produce the same glyph geometry");
    check(same_geometry(LR"(\mathcal{A\mathfrak{B}C})", LR"(\mathcal{A}\mathfrak{B}\mathcal{C})"),
        "nested alphabet overrides restore the outer family");
    check(same_geometry(LR"(\mathcal{\text{plain text}})", LR"(\text{plain text})") &&
        same_geometry(LR"(\mathsf{\textbf{plain text}})", LR"(\textbf{plain text})"),
        "text commands preserve literal text instead of applying mathematical Unicode mapping");
    check(same_geometry(LR"(\newcommand{\word}{abc}\text{a b \word xyz})", LR"(\text{a b abcxyz})"),
        "expanded text obeys control word whitespace without removing normal text spaces");
    struct AlphabetCase { const wchar_t* command; MathAlphabet family; const wchar_t* expected; };
    for (const auto& sample : {AlphabetCase{L"mathcal", MathAlphabet::Script, L"𝒜"},
        AlphabetCase{L"mathfrak", MathAlphabet::Fraktur, L"𝔄"}, AlphabetCase{L"mathsf", MathAlphabet::SansSerif, L"𝖠"},
        AlphabetCase{L"mathtt", MathAlphabet::Monospace, L"𝙰"}, AlphabetCase{L"bm", MathAlphabet::BoldItalic, L"𝑨"}}) {
        std::wstring mapped;
        const std::wstring formula = L"\\" + std::wstring(sample.command) + L"{A}";
        const std::wstring literal = L"\\mathrm{" + std::wstring(sample.expected) + L"}";
        const std::wstring expected(sample.expected);
        const UINT32 scalar = expected.size() == 2 ? 0x10000 + ((static_cast<UINT32>(expected[0]) - 0xd800) << 10) +
            static_cast<UINT32>(expected[1]) - 0xdc00 : static_cast<UINT32>(expected[0]);
        UINT16 glyph = 0;
        check(MapMathAlphabet(L"A", sample.family, mapped) && mapped == expected &&
            SUCCEEDED(face->GetGlyphIndices(&scalar, 1, &glyph)) && glyph != 0 && same_geometry(formula, literal),
            "alphabet maps to exact Unicode scalar present in font and matching literal glyph geometry");
    }
    const UINT32 missing_scalar = 0x10fffd;
    UINT16 missing_glyph = 1;
    const std::wstring missing{static_cast<wchar_t>(0xdbff), static_cast<wchar_t>(0xdffd)};
    FormulaLayout absent;
    check(SUCCEEDED(face->GetGlyphIndices(&missing_scalar, 1, &missing_glyph)) && missing_glyph == 0 && !build(missing, absent),
        "missing font glyph causes fallback rather than accepting a Unicode mapping as proof of rendering");
    const std::vector<std::wstring> invalid = {LR"(\newcommand{\frac}[2]{#1+#2}\frac{a}{b})", LR"(\renewcommand{\absent}{x}\absent)",
        LR"(\newcommand{\f}[1]{#2}\f{x})", LR"(\newcommand{\f}{\f}\f)", LR"({\newcommand{\f}{x}\f}+\f)",
        LR"(\def\f{x}\f)", LR"(\gdef\f{x}\f)", LR"(\let\f=x\f)", LR"(\catcode{a})", LR"(\newcommand{\f}[10]{x}\f)",
        LR"(\newcommand{\f}[1][x]{#1}\f[unclosed)", LR"(\newcommand{\f}[1]{#1}\f)",
        LR"(\substack{\newcommand{\localrow}{b}\localrow\\\localrow})"};
    ComPtr<IDWriteTextFormat> format;
    factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 24, L"en-US", &format);
    for (const auto& source : invalid) {
        ComPtr<IDWriteTextLayout> layout;
        ComPtr<IDWriteInlineObject> object;
        if (format) factory->CreateTextLayout(source.data(), static_cast<UINT32>(source.size()), format.Get(), 10000, 2000, &layout);
        check(layout && !ApplyMathInline(factory, layout.Get(), {0, static_cast<UINT32>(source.size())}, source, 24, false, 10000) &&
            SUCCEEDED(layout->GetInlineObject(0, &object)) && !object, "unsupported unsafe or malformed macro leaves raw layout unchanged");
    }
    std::wstring payload;
    MarkdownView view;
    const auto& profile = MathReliableProfile();
    check(pulse::preview::MakeMarkdownDocument(profile.markdown, payload) && view.SetPayload(payload, L"bench_data/math_preview.md") &&
        view.Source() == profile.markdown && view.PlainText().find(profile.copy_probe) != std::wstring::npos,
        "source and copied text preserve original macro definitions and invocations");
    std::printf("[COVERAGE] Reliable mathematics gallery: all %zu cases, no sampling.\n", profile.formulas.size());
    return failures;
}
}
