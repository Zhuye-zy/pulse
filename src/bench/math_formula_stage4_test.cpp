#include "math_formula_stage4_test.h"
#include "../ui/math_formula.h"
#include "../ui/markdown_view.h"
#include "../preview_host/markdown_document.h"
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <string_view>

namespace pulse_test {
using Microsoft::WRL::ComPtr;
const std::vector<std::wstring>& MathStage4Formulas() {
    static const std::vector<std::wstring> formulas = {
        LR"(\begin{array}{lcr}a&bb&ccc\\aaaa&b&c\\aa&bbbb&cc\end{array})",
        LR"(\begin{array}{l}a\\aaaa\end{array}+\begin{array}{c}a\\aaaa\end{array}+\begin{array}{r}a\\aaaa\end{array})",
        LR"(\sum_{\substack{i=1\\j=2}}^{n} a_{ij})",
        LR"(\displaystyle\operatorname*{arg max}_{x\in X} f(x))",
        LR"(\textstyle\operatorname*{arg max}_{x\in X} f(x))",
        LR"(\textstyle\operatorname*{arg max}\limits_{x\in X} f(x))",
        LR"(\acute{a}+\grave{a}+\breve{a}+\check{a}+\mathring{a})",
        LR"(\acute{a})", LR"(\grave{a})", LR"(\breve{a})", LR"(\check{a})", LR"(\mathring{a})",
        LR"(\overrightarrow{abc}+\overleftarrow{abc}+\overleftrightarrow{abc})",
        LR"(\underrightarrow{abc}+\underleftarrow{abc}+\underleftrightarrow{abc})"
    };
    return formulas;
}
const std::wstring& MathStage4Source() {
    static const std::wstring source = LR"(# Stage 4 math

New accents \(\acute{x}\), \(\grave{x}\), \(\breve{x}\), \(\check{x}\), and \(\mathring{x}\).

Columns below are left, center, and right aligned. Unequal cell lengths expose alignment.

\[\begin{array}{lcr}a&bb&ccc\\aaaa&b&c\\aa&bbbb&cc\end{array}\]

\[\sum_{\substack{i=1\\j=2}}^{n} a_{ij}\]

\[\displaystyle\operatorname*{arg max}_{x\in X} f(x)\]

Inline \(\operatorname*{arg max}_{x\in X} f(x)\) keeps limits at the side.

\[\textstyle\operatorname*{arg max}\limits_{x\in X} f(x)\]

\[\overrightarrow{abc}+\overleftarrow{abc}+\overleftrightarrow{abc}\]

\[\underrightarrow{abc}+\underleftarrow{abc}+\underleftrightarrow{abc}\]

- A ring \(\mathring{x}\) inside a list.
- Unsupported \(\begin{array}{|||c}x\end{array}\) stays literal.

| Mark | Formula |
| --- | --- |
| Check | \(\check{x}\) |
| Arrow | \(\underleftarrow{x}\) |

Code `\(\acute{x}\)` stays literal.
)";
    return source;
}
namespace {
struct Measurement { bool valid; float width, height; };
ComPtr<IDWriteTextLayout> Layout(IDWriteFactory2* factory, std::wstring_view source) {
    ComPtr<IDWriteTextFormat> format;
    ComPtr<IDWriteTextLayout> layout;
    if (SUCCEEDED(factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 24, L"en-US", &format)))
        factory->CreateTextLayout(source.data(), static_cast<UINT32>(source.size()), format.Get(), 10000, 2000, &layout);
    return layout;
}
}
int TestMathStage4(IDWriteFactory2* factory) {
    int failures = 0;
    const auto check = [&failures](bool valid, const char* name) {
        std::printf("[%s] stage4 %s\n", valid ? "PASS" : "FAIL", name);
        if (!valid) ++failures;
    };
    const auto measure = [factory](std::wstring_view source, bool display = false) {
        auto layout = Layout(factory, source);
        ComPtr<IDWriteInlineObject> object;
        DWRITE_INLINE_OBJECT_METRICS metrics{};
        const bool valid = layout && pulse::ui::ApplyMathInline(factory, layout.Get(),
            {0, static_cast<UINT32>(source.size())}, source, 24, display, 10000) &&
            SUCCEEDED(layout->GetInlineObject(0, &object)) && object && SUCCEEDED(object->GetMetrics(&metrics));
        return Measurement{valid, metrics.width, metrics.height};
    };
    const auto equal = [](Measurement a, Measurement b) {
        return a.valid && b.valid && std::abs(a.width - b.width) < 0.1f && std::abs(a.height - b.height) < 0.1f;
    };
    for (const auto& source : MathStage4Formulas()) {
        const auto m = measure(source);
        check(m.valid && std::isfinite(m.width) && std::isfinite(m.height) && m.width > 0 && m.height > 0,
            "supported expression produces finite visible geometry");
        if (!m.valid) std::wprintf(L"  Formula: %ls\n", source.c_str());
    }
    check(equal(measure(LR"(\begin{array}{l}a\\aaaa\end{array})"), measure(LR"(\begin{array}{r}a\\aaaa\end{array})")),
        "alignment preserves overall array extent while moving short cell contents");
    check(measure(LR"(\begin{array}{ l c r }a&b&c\\d\end{array})").valid,
        "array accepts whitespace specification and pads shorter rows");
    check(equal(measure(LR"(\scriptstyle\begin{array}{c}x\end{array}x)"),
        measure(LR"(\scriptstyle\begin{array}{c}x\end{array}\scriptstyle x)")), "array restores surrounding script style");
    const auto stacked = measure(LR"(\substack{a\\b})"), single = measure(LR"(\substack{a})");
    check(stacked.valid && single.valid && stacked.height > single.height + 3,
        "substack reserves vertical space for both rows");
    check(equal(measure(LR"(\substack{a\\b}x)"), measure(LR"(\substack{a\\b}\textstyle x)")),
        "substack does not leak script style into following text");
    const auto display_op = measure(LR"(\operatorname*{arg max}_{x\in X})", true),
        inline_op = measure(LR"(\operatorname*{arg max}_{x\in X})"),
        limits_op = measure(LR"(\operatorname*{arg max}\limits_{x\in X})");
    check(display_op.valid && inline_op.valid && limits_op.valid && display_op.height > inline_op.height + 3 &&
        limits_op.height > inline_op.height + 3, "operatorname star stacks display limits and explicit inline limits");
    check(equal(measure(LR"(\operatorname*{arg max}_{x\in X})"), measure(LR"(\operatorname{arg max}_{x\in X})")),
        "operatorname star uses ordinary side scripts in text style");
    const auto named_followed = measure(LR"(\operatorname{op}x)"), starred_followed = measure(LR"(\operatorname*{op}x)"),
        ordinary_followed = measure(LR"(\mathrm{op}x)");
    check(named_followed.valid && starred_followed.valid && ordinary_followed.valid &&
        named_followed.width > ordinary_followed.width + 1 && starred_followed.width > ordinary_followed.width + 1,
        "named operators reserve a visible gap before following expression");
    check(equal(measure(LR"(\textstyle{\displaystyle\operatorname*{op}_{i}}+\sum_{i=1}^{n}i)"),
        measure(LR"(\textstyle{\displaystyle\operatorname*{op}_{i}}+\textstyle\sum_{i=1}^{n}i)")),
        "operator display group restores text style before following sum");
    const auto base = measure(L"a"), reference = measure(LR"(\hat{a})");
    for (const auto& source : {LR"(\acute{a})", LR"(\grave{a})", LR"(\breve{a})", LR"(\check{a})", LR"(\mathring{a})"}) {
        const auto accent = measure(source);
        check(base.valid && reference.valid && accent.valid && accent.height >= base.height && accent.height <= reference.height + 8,
            "new accent stays near its base and reserves visible height");
    }
    const auto upper = measure(LR"(\overleftrightarrow{abc})"), lower = measure(LR"(\underleftrightarrow{abc})"), letters = measure(L"abc");
    check(upper.valid && lower.valid && letters.valid && upper.height > letters.height && lower.height > letters.height,
        "upper and lower arrows reserve decoration height");
    std::vector<std::wstring> invalid = {LR"(\begin{array}{}x\end{array})", LR"(\begin{array}{|||c}x\end{array})",
        LR"(\begin{array}{p{2}}x\end{array})", LR"(\begin{array}{@{}c}x\end{array})", LR"(\begin{array}{c}a&b\end{array})",
        LR"(\begin{array}{lllllllllllllllll}x\end{array})", LR"(\substack{a&b})", LR"(\substack{a)", LR"(\operatorname*)",
        LR"(\acute)", LR"(\overleftarrow{})", LR"(\underleftrightarrow{})", std::wstring(4097, L'x')};
    std::wstring over_rows = LR"(\substack{a)";
    for (int i = 0; i < 32; ++i) over_rows += LR"(\\a)";
    invalid.push_back(over_rows + L"}");
    std::wstring over_cells = LR"(\begin{array}{lllll})";
    for (int i = 0; i < 26; ++i) over_cells += i ? LR"(\\a&a&a&a&a)" : L"a&a&a&a&a";
    invalid.push_back(over_cells + LR"(\end{array})");
    for (const auto& source : invalid) {
        auto layout = Layout(factory, source);
        ComPtr<IDWriteInlineObject> object;
        check(layout && !pulse::ui::ApplyMathInline(factory, layout.Get(), {0, static_cast<UINT32>(source.size())}, source, 24, false, 600) &&
            SUCCEEDED(layout->GetInlineObject(0, &object)) && !object, "malformed unsupported and over-budget source remains untouched");
    }
    std::wstring payload;
    pulse::ui::MarkdownView view;
    check(pulse::preview::MakeMarkdownDocument(MathStage4Source(), payload) && view.SetPayload(payload, L"bench_data/math_preview.md") &&
        view.Source() == MathStage4Source() && view.PlainText().find(LR"(\(\acute{x}\))") != std::wstring::npos &&
        view.PlainText().find(LR"(\begin{array}{lcr})") != std::wstring::npos,
        "source and copied text retain complete stage4 formula syntax");
    return failures;
}
}
