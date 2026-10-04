#include "math_formula_stage3_test.h"
#include "../ui/math_formula.h"
#include "../ui/markdown_view.h"
#include "../preview_host/markdown_document.h"
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <string_view>

namespace pulse_test {
using Microsoft::WRL::ComPtr;
const std::vector<std::wstring>& MathStage3Formulas() {
    static const std::vector<std::wstring> formulas = {
        LR"(\langle x\rangle + \lbrace y\rbrace + \lvert z\rvert + \lfloor a\rfloor + \lceil b\rceil)",
        LR"(\bigl( x \bigr) + \Bigl[ y \Bigr] + \biggl\{ z \biggr\} + \Biggl\langle w \Biggr\rangle)",
        LR"(\begin{Bmatrix}a & b \\ c & d\end{Bmatrix})",
        LR"(A=\begin{smallmatrix}a & b \\ c & d\end{smallmatrix})",
        LR"(\begin{gathered}a+b=c \\ x=y+z\end{gathered})",
        LR"(a\equiv b\pmod{n} + a\bmod n)",
        LR"(a_1,\ldots,a_n + a_1+\cdots+a_n + \vdots + \ddots)",
        LR"(\overset{!}{=} + \underset{n}{\lim} + \overset{a+b}{x})"
    };
    return formulas;
}
const std::wstring& MathStage3Source() {
    static const std::wstring source = LR"(# Stage 3 math

Parenthesized \(x_i^2\) and existing $E=mc^2$ remain in the same paragraph.

\[
\bigl( x \bigr) + \Bigl[ y \Bigr] + \biggl\{ z \biggr\} + \Biggl\langle w \Biggr\rangle
\]

\[\begin{Bmatrix}a & b \\ c & d\end{Bmatrix}\]

Small inline matrix \(A=\begin{smallmatrix}a & b \\ c & d\end{smallmatrix}\) with prose.

\[\begin{gathered}a+b=c \\ x=y+z\end{gathered}\]

Congruence \(a\equiv b\pmod{n}\) and remainder \(a\bmod n\).

\[a_1,\ldots,a_n + a_1+\cdots+a_n + \vdots + \ddots\]

\[\overset{!}{=} + \underset{n}{\lim}\]

- **Emphasized \(x^2\)** and list prose.
- Unclosed \(x+1 remains literal.

| Form | Meaning |
| --- | --- |
| \(\langle x\rangle\) | Inner brackets |
| \(\overset{a}{b}\) | Above |

Code `\(x^2\)` and escaped \\(x\\) stay literal.

```tex
\[\frac{a}{b}\]
```
)";
    return source;
}
namespace {
std::vector<std::wstring> Split(std::wstring_view text, wchar_t delimiter) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == delimiter) {
            parts.emplace_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    return parts;
}
std::wstring Unescape(std::wstring_view text) {
    std::wstring result;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\\' && i + 1 < text.size()) {
            const wchar_t c = text[++i];
            result += c == L'n' ? L'\n' : c == L't' ? L'\t' : c;
        } else result += text[i];
    }
    return result;
}
struct MathRun { std::wstring text; unsigned flags; wchar_t kind; };
std::vector<MathRun> Runs(const std::wstring& payload) {
    std::vector<MathRun> runs;
    for (const auto& line : Split(payload, L'\n')) {
        const auto fields = Split(line, L'\t');
        if (fields.size() != 8 || fields[0] != L"B" || fields[1].empty()) continue;
        const auto text = Unescape(fields[6]);
        for (const auto& entry : Split(fields[7], L';')) {
            const auto values = Split(entry, L',');
            if (values.size() < 3) continue;
            const auto start = std::stoul(values[0]), length = std::stoul(values[1]), flags = std::stoul(values[2]);
            if ((flags & 384) && start <= text.size() && length <= text.size() - start)
                runs.push_back({text.substr(start, length), flags, fields[1][0]});
        }
    }
    return runs;
}
bool HasRunFlag(const std::wstring& payload, unsigned flag) {
    for (const auto& line : Split(payload, L'\n')) {
        const auto fields = Split(line, L'\t');
        if (fields.size() != 8 || fields[0] != L"B") continue;
        for (const auto& entry : Split(fields[7], L';')) {
            const auto values = Split(entry, L',');
            if (values.size() >= 3 && (std::stoul(values[2]) & flag)) return true;
        }
    }
    return false;
}
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
int TestMathStage3(IDWriteFactory2* factory) {
    int failures = 0;
    const auto check = [&failures](bool valid, const char* name) {
        std::printf("[%s] stage3 %s\n", valid ? "PASS" : "FAIL", name);
        if (!valid) ++failures;
    };
    const auto measure = [factory](std::wstring_view source) {
        auto layout = Layout(factory, source);
        ComPtr<IDWriteInlineObject> object;
        DWRITE_INLINE_OBJECT_METRICS metrics{};
        const bool valid = layout && pulse::ui::ApplyMathInline(factory, layout.Get(),
            {0, static_cast<UINT32>(source.size())}, source, 24, false, 10000) &&
            SUCCEEDED(layout->GetInlineObject(0, &object)) && object && SUCCEEDED(object->GetMetrics(&metrics));
        return Measurement{valid, metrics.width, metrics.height};
    };
    for (const auto& source : MathStage3Formulas()) {
        const auto m = measure(source);
        check(m.valid && std::isfinite(m.width) && std::isfinite(m.height) && m.width > 0 && m.height > 0,
            "supported expression produces finite visible geometry");
        if (!m.valid) std::wprintf(L"  Formula: %ls\n", source.c_str());
    }
    const auto big = measure(LR"(\bigl(x\bigr))"), bigger = measure(LR"(\Bigl(x\Bigr))"),
        bigg = measure(LR"(\biggl(x\biggr))"), biggest = measure(LR"(\Biggl(x\Biggr))");
    check(big.valid && bigger.valid && bigg.valid && biggest.valid && big.height < bigger.height &&
        bigger.height < bigg.height && bigg.height < biggest.height, "four explicit delimiter sizes increase monotonically");
    const auto invisible = measure(LR"(\big.x)"), x = measure(L"x");
    check(invisible.valid && x.valid && std::abs(invisible.width - x.width) < 0.1f &&
        std::abs(invisible.height - x.height) < 0.1f, "invisible explicit delimiter adds no visible size");
    const auto dots = measure(LR"(\dots)"), ldots = measure(LR"(\ldots)");
    check(dots.valid && ldots.valid && std::abs(dots.width - ldots.width) < 0.1f &&
        std::abs(dots.height - ldots.height) < 0.1f, "dots alias matches baseline ellipsis");
    const auto matrix = measure(LR"(\begin{matrix}a&b\\c&d\end{matrix})"),
        small_matrix = measure(LR"(\begin{smallmatrix}a&b\\c&d\end{smallmatrix})");
    check(matrix.valid && small_matrix.valid && small_matrix.width < matrix.width && small_matrix.height < matrix.height,
        "smallmatrix is smaller than ordinary matrix");
    const auto base = measure(L"x"), over = measure(LR"(\overset{a+b}{x})"), under = measure(LR"(\underset{a+b}{x})");
    check(base.valid && over.valid && under.valid && over.height > base.height && under.height > base.height,
        "overset and underset reserve annotation height");
    std::vector<std::wstring> invalid = {LR"(\big)", LR"(\Bigq(x))", LR"(\big\unknown)", LR"(\overset{a})",
        LR"(\underset{a}{b)", LR"(\begin{Bmatrix}a\end{matrix})", LR"(\begin{gathered}a)",
        LR"(\begin{gathered}a&b\end{gathered})", LR"(\pmod)", std::wstring(4097, L'x')};
    for (const auto& source : invalid) {
        auto layout = Layout(factory, source);
        ComPtr<IDWriteInlineObject> object;
        check(layout && !pulse::ui::ApplyMathInline(factory, layout.Get(), {0, static_cast<UINT32>(source.size())}, source, 24, false, 600) &&
            SUCCEEDED(layout->GetInlineObject(0, &object)) && !object, "malformed and over-budget syntax preserves raw source");
    }
    std::wstring payload;
    const auto has_run = [&payload](std::wstring_view expected, unsigned flag, wchar_t kind = 0) {
        for (const auto& run : Runs(payload))
            if (run.text == expected && (run.flags & flag) && (!kind || run.kind == kind)) return true;
        return false;
    };
    check(pulse::preview::MakeMarkdownDocument(LR"(Before \(x_i^2\) after.)", payload) && has_run(LR"(\(x_i^2\))", 128),
        "parenthesis math range retains complete original delimiters");
    check(pulse::preview::MakeMarkdownDocument(LR"([label]\(x\))", payload) && has_run(LR"(\(x\))", 128) &&
        !HasRunFlag(payload, 16), "formula after square-bracket text never becomes a Markdown link");
    const std::wstring quote_source = L"> \\[\n> x\n> \\]\n";
    pulse::ui::MarkdownView quoted_view;
    bool quote_ok = pulse::preview::MakeMarkdownDocument(quote_source, payload) &&
        quoted_view.SetPayload(payload, L"bench_data/math_preview.md") && quoted_view.Source() == quote_source;
    for (const auto& run : Runs(payload))
        quote_ok = quote_ok && run.text.find(L'>') == std::wstring::npos && run.text.find(L'x') != std::wstring::npos;
    check(quote_ok, "multiline quote strips structural markers from math or keeps literal fallback");
    check(pulse::preview::MakeMarkdownDocument(LR"(\(x\)_tail_ and \(\text{price $5}\))", payload) &&
        has_run(LR"(\(x\))", 128) && has_run(LR"(\(\text{price $5}\))", 128),
        "adjacent emphasis and a single literal dollar preserve formula boundaries");
    check(pulse::preview::MakeMarkdownDocument(L"\\[\n\\frac{a}{b}\n\\]\n", payload) &&
        has_run(L"\\[\n\\frac{a}{b}\n\\]", 256, L'm'), "standalone bracket math preserves newlines and display kind");
    check(pulse::preview::MakeMarkdownDocument(LR"(**Bold \(x\)** and [linked \(y\)](https://example.com)

- Item \(z\)

| Formula |
| --- |
| \(w\) |
)", payload) && has_run(LR"(\(x\))", 128) && has_run(LR"(\(y\))", 128) &&
        has_run(LR"(\(z\))", 128) && has_run(LR"(\(w\))", 128), "math is recognized within emphasis links lists and tables");
    for (const auto& source : {LR"(`\(x\)` and `\[x\]`)", LR"([target](https://example.com/\(x\)))",
        LR"(Escaped \\(x\\) and \\[x\\])", LR"(Unclosed \(x and mismatched \[y\))", LR"(\(x\\))",
        L"```tex\n\\(x\\)\n\\[x\\]\n```\n"}) {
        check(pulse::preview::MakeMarkdownDocument(source, payload) && Runs(payload).empty(),
            "code URLs escaped and malformed delimiters do not create math runs");
    }
    for (const auto& source : {L"    \\(x\\)\n    \\[y\\]\n", LR"html(<span title="\(x\)">text</span>)html",
        L"<div>\n\\(x\\)\n</div>\n", L"\\(x\n\ny\\)\n", L"| a | b |\n| --- | --- |\n| \\(x | y\\) |\n"}) {
        check(pulse::preview::MakeMarkdownDocument(source, payload) && Runs(payload).empty(),
            "indented code HTML and cross-block or cross-cell delimiters stay literal");
    }
    check(pulse::preview::MakeMarkdownDocument(L"\\(" + std::wstring(4097, L'x') + L"\\)", payload) && Runs(payload).empty(),
        "oversized delimited source stays literal within host budget");
    payload.clear();
    check(pulse::preview::AppendMarkdownBlocks(LR"(Notebook \(x\)

\[y\]
)", payload, 10000, 1) && has_run(LR"(\(x\))", 128) && has_run(LR"(\[y\])", 256, L'm'),
        "notebook blocks share backslash-delimiter support");
    pulse::ui::MarkdownView view;
    check(pulse::preview::MakeMarkdownDocument(MathStage3Source(), payload) &&
        view.SetPayload(payload, L"bench_data/math_preview.md") && view.Source() == MathStage3Source() &&
        view.PlainText().find(LR"(\(x_i^2\))") != std::wstring::npos,
        "source and copied text retain backslash formula delimiters");
    return failures;
}
}
