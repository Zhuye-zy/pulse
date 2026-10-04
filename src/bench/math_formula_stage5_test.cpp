#include "math_formula_stage5_test.h"
#include "../ui/math_formula.h"
#include "../ui/markdown_view.h"
#include "../preview_host/markdown_document.h"
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <string_view>

namespace pulse_test {
using Microsoft::WRL::ComPtr;
const std::vector<std::wstring>& MathStage5Formulas() {
    static const std::vector<std::wstring> formulas = {
        LR"(\boxed{x}+\boxed{\frac{a+b}{c+d}})",
        LR"(\cancel{abc}+\bcancel{abc}+\xcancel{abc})",
        LR"(\cancel{\frac{a}{b}}+\bcancel{\frac{a}{b}}+\xcancel{\frac{a}{b}})",
        LR"(A\xrightarrow{f}B\xleftarrow{g}C)",
        LR"(A\xrightarrow[below]{longlabel}B)", LR"(A\xleftarrow[longlabel]{f}B)",
        LR"(\xrightarrow{}+\xleftarrow[]{}+\boxed{})",
        LR"(\arg x+\deg f+\dim V+\hom V+\ker f+\coth x+\lg x)",
        LR"(\displaystyle\inf_{x}f+\sup_{x}f+\liminf_{n}a_n+\limsup_{n}a_n)",
        LR"(\displaystyle\Pr_{x}f+\argmax_{x}f+\argmin_{x}f)",
        LR"(\textstyle\Pr_{x}f+\argmax_{x}f+\argmin_{x}f)",
        LR"(\operatornamewithlimits{custom}\limits_{x} f(x))"
    };
    return formulas;
}
const std::wstring& MathStage5Source() {
    static const std::wstring source = LR"(# Stage 5 math

Boxed inline \(\boxed{x}\) preserves the prose baseline.

\[\boxed{\frac{a+b}{c+d}}\]

The three cancellation strokes ascend, descend, and cross:

\[\cancel{abc}+\bcancel{abc}+\xcancel{abc}\]

\[\cancel{\frac{a}{b}}+\bcancel{\frac{a}{b}}+\xcancel{\frac{a}{b}}\]

\[A\xrightarrow{f}B\xleftarrow{g}C\]

\[A\xrightarrow[below]{longlabel}B\]

\[A\xleftarrow[longlabel]{f}B\]

\[\arg x+\deg f+\dim V+\hom V+\ker f+\coth x+\lg x\]

\[\inf_{x}f+\sup_{x}f+\liminf_{n}a_n+\limsup_{n}a_n\]

\[\Pr_{x}f+\argmax_{x}f+\argmin_{x}f\]

Inline \(\argmax_{x}f\) and explicit limits \(\operatornamewithlimits{custom}\limits_{x}f\).

- Incomplete \(\boxed\) stays literal.
- Code `\(\cancel{x}\)` stays literal.

| Decoration | Formula |
| --- | --- |
| Box | \(\boxed{x}\) |
| Arrow | \(\xleftarrow[f]{g}\) |
)";
    return source;
}
namespace {
struct Measurement { bool valid; float width, height, baseline; };
ComPtr<IDWriteTextLayout> Layout(IDWriteFactory2* factory, std::wstring_view source) {
    ComPtr<IDWriteTextFormat> format;
    ComPtr<IDWriteTextLayout> layout;
    if (SUCCEEDED(factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 24, L"en-US", &format)))
        factory->CreateTextLayout(source.data(), static_cast<UINT32>(source.size()), format.Get(), 10000, 2000, &layout);
    return layout;
}
}
int TestMathStage5(IDWriteFactory2* factory) {
    int failures = 0;
    const auto check = [&failures](bool valid, const char* name) {
        std::printf("[%s] stage5 %s\n", valid ? "PASS" : "FAIL", name);
        if (!valid) ++failures;
    };
    const auto measure = [factory](std::wstring_view source, bool display = false) {
        auto layout = Layout(factory, source);
        ComPtr<IDWriteInlineObject> object;
        DWRITE_INLINE_OBJECT_METRICS metrics{};
        const bool valid = layout && pulse::ui::ApplyMathInline(factory, layout.Get(),
            {0, static_cast<UINT32>(source.size())}, source, 24, display, 10000) &&
            SUCCEEDED(layout->GetInlineObject(0, &object)) && object && SUCCEEDED(object->GetMetrics(&metrics));
        return Measurement{valid, metrics.width, metrics.height, metrics.baseline};
    };
    const auto equal = [](Measurement a, Measurement b) {
        return a.valid && b.valid && std::abs(a.width - b.width) < 0.1f && std::abs(a.height - b.height) < 0.1f &&
            std::abs(a.baseline - b.baseline) < 0.1f;
    };
    for (const auto& source : MathStage5Formulas()) {
        const auto m = measure(source);
        check(m.valid && std::isfinite(m.width) && std::isfinite(m.height) && m.width > 0 && m.height > 0 &&
            m.baseline > 0 && m.baseline <= m.height, "supported expression has finite geometry and baseline");
        if (!m.valid) std::wprintf(L"  Formula: %ls\n", source.c_str());
    }
    const auto base = measure(L"abc"), boxed = measure(LR"(\boxed{abc})");
    check(base.valid && boxed.valid && boxed.width > base.width + 4 && boxed.height > base.height + 4 &&
        boxed.baseline > base.baseline && boxed.height - boxed.baseline > base.height - base.baseline,
        "box reserves frame and padding above and below the baseline");
    for (const auto& source : {LR"(\cancel{abc})", LR"(\bcancel{abc})", LR"(\xcancel{abc})"}) {
        const auto cancelled = measure(source);
        check(cancelled.valid && base.valid && std::abs(cancelled.baseline - base.baseline) <= 2,
            "cancellation retains base baseline while containing its strokes");
    }
    const auto short_arrow = measure(LR"(\xrightarrow{a})"), long_arrow = measure(LR"(\xrightarrow{longlabel})"),
        both_arrow = measure(LR"(\xrightarrow[b]{a})"), empty_arrow = measure(LR"(\xrightarrow{})");
    check(short_arrow.valid && long_arrow.valid && long_arrow.width > short_arrow.width + 10,
        "long annotation expands arrow shaft");
    check(both_arrow.valid && short_arrow.valid && both_arrow.height > short_arrow.height + 3,
        "lower annotation reserves additional vertical space");
    check(empty_arrow.valid && empty_arrow.width > 24 && empty_arrow.height > 0,
        "empty annotation retains a visible minimum arrow");
    check(measure(LR"(\xrightarrow[]{})").valid && measure(LR"(\boxed{})").valid,
        "empty arrow annotations and empty boxed group still draw visible rules");
    const auto boxed_fraction = measure(LR"(\boxed{\frac{a}{b}})"), inline_fraction = measure(LR"(\frac{a}{b})");
    check(boxed_fraction.valid && inline_fraction.valid && boxed_fraction.height > inline_fraction.height + 4 &&
        equal(measure(LR"(\boxed{x}\sum_{i=1}^{n}i)"), measure(LR"(\boxed{x}\textstyle\sum_{i=1}^{n}i)")),
        "boxed uses display style internally and restores surrounding inline style");
    check(equal(measure(LR"(\xrightarrow[a]{b}x)"), measure(LR"(\xrightarrow[a]{b}\textstyle x)")) &&
        equal(measure(LR"(\scriptstyle\xleftarrow[a]{b}x)"), measure(LR"(\scriptstyle\xleftarrow[a]{b}\scriptstyle x)")),
        "arrow labels restore surrounding text and script styles");
    for (const auto& command : {L"inf", L"sup", L"liminf", L"limsup", L"Pr", L"argmax", L"argmin", L"det", L"gcd"}) {
        const std::wstring expression = L"\\" + std::wstring(command) + L"_{x}";
        const auto inline_op = measure(expression), display_op = measure(expression, true);
        const auto forced = measure(L"\\" + std::wstring(command) + LR"(\limits_{x})");
        check(inline_op.valid && display_op.valid && forced.valid && display_op.height > inline_op.height + 3 &&
            forced.height > inline_op.height + 3, "named limit operator changes between inline and stacked limits");
    }
    check(equal(measure(LR"(\operatornamewithlimits{custom}_{x})", true), measure(LR"(\operatorname*{custom}_{x})", true)) &&
        equal(measure(LR"(\operatornamewithlimits{custom}_{x})"), measure(LR"(\operatorname*{custom}_{x})")),
        "operatornamewithlimits matches starred operator semantics in both styles");
    const auto upright = measure(LR"(\mathrm{arg}x)"), named = measure(LR"(\arg x)");
    check(upright.valid && named.valid && named.width > upright.width + 1, "new named operator includes following separation");
    const auto side_operator = measure(LR"(\arg_{x})", true), forced_operator = measure(LR"(\arg\limits_{x})", true);
    check(equal(side_operator, measure(LR"(\arg_{x})")) && forced_operator.valid &&
        forced_operator.height > side_operator.height + 3, "side operator stacks limits only when explicitly requested");
    std::vector<std::wstring> invalid = {LR"(\boxed)", LR"(\cancel{})", LR"(\bcancel)", LR"(\xcancel{x)", LR"(\quad)", L"{}",
        LR"(\xrightarrow)", LR"(\xleftarrow[a)", LR"(\xrightarrow[a])", LR"(\xrightarrow[a][b]{c})",
        LR"(\operatornamewithlimits)", LR"(\unknownarrow{x})", std::wstring(4097, L'x')};
    std::wstring deep;
    for (int i = 0; i < 40; ++i) deep += LR"(\boxed{)";
    deep += L"x" + std::wstring(40, L'}');
    invalid.push_back(deep);
    for (const auto& source : invalid) {
        auto layout = Layout(factory, source);
        ComPtr<IDWriteInlineObject> object;
        check(layout && !pulse::ui::ApplyMathInline(factory, layout.Get(), {0, static_cast<UINT32>(source.size())}, source, 24, false, 600) &&
            SUCCEEDED(layout->GetInlineObject(0, &object)) && !object, "malformed unsupported and over-budget source remains untouched");
    }
    std::wstring payload;
    pulse::ui::MarkdownView view;
    check(pulse::preview::MakeMarkdownDocument(MathStage5Source(), payload) && view.SetPayload(payload, L"bench_data/math_preview.md") &&
        view.Source() == MathStage5Source() && view.PlainText().find(LR"(\(\boxed{x}\))") != std::wstring::npos &&
        view.PlainText().find(LR"(\xrightarrow[below]{longlabel})") != std::wstring::npos,
        "source and copied text retain decoration and arrow label syntax");
    return failures;
}
}
