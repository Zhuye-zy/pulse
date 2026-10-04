#include "../ui/math_formula.h"
#include "math_formula_stage3_test.h"
#include "math_formula_stage4_test.h"
#include "math_formula_stage5_test.h"
#include "math_formula_architecture_test.h"
#include "math_formula_structures_test.h"
#include "math_formula_reliable_test.h"
#include "../ui/markdown_view.h"
#include "../preview_host/markdown_document.h"
#include <d3d11.h>
#include <d2d1_1.h>
#include <dwrite_2.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
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
            const wchar_t next = text[++i];
            result += next == L'n' ? L'\n' : next == L't' ? L'\t' : next;
        } else result += text[i];
    }
    return result;
}
struct Run { size_t start, length; unsigned flags; };
struct Block { std::wstring kind, text; std::vector<Run> runs; };
struct Payload { std::wstring source; std::vector<Block> blocks; };
Payload Parse(const std::wstring& text) {
    Payload result;
    for (const auto& line : Split(text, L'\n')) {
        const auto fields = Split(line, L'\t');
        if (fields.size() == 2 && fields[0] == L"S") result.source = Unescape(fields[1]);
        if (fields.size() != 8 || fields[0] != L"B") continue;
        Block block{fields[1], Unescape(fields[6]), {}};
        for (const auto& run : Split(fields[7], L';')) {
            const auto values = Split(run, L',');
            if (values.size() >= 3) block.runs.push_back({std::stoul(values[0]), std::stoul(values[1]), std::stoul(values[2])});
        }
        result.blocks.push_back(std::move(block));
    }
    return result;
}
bool HasMath(const Payload& payload, std::wstring_view text, unsigned flag, std::wstring_view kind = {}) {
    for (const auto& block : payload.blocks) {
        if (!kind.empty() && kind != block.kind) continue;
        for (const auto& run : block.runs) {
            if ((run.flags & flag) && run.start <= block.text.size() && run.length <= block.text.size() - run.start &&
                std::wstring_view(block.text).substr(run.start, run.length) == text) return true;
        }
    }
    return false;
}
bool NoMath(const Payload& payload) {
    for (const auto& block : payload.blocks)
        for (const auto& run : block.runs) if (run.flags & (128u | 256u)) return false;
    return true;
}
void TestMarkdown() {
    const std::wstring source = L"# Formula\n\nBefore $x^2 + \\alpha$ after.\n\n$$\n\\frac{a}{b}\n$$\n";
    std::wstring text;
    Check(pulse::preview::MakeMarkdownDocument(source, text), "Markdown math parses");
    const auto payload = Parse(text);
    Check(payload.source == source, "source view retains exact LaTeX and delimiters");
    Check(HasMath(payload, L"$x^2 + \\alpha$", 128), "inline range retains dollar delimiters and formula");
    bool display = false;
    for (const auto& block : payload.blocks) {
        if (block.kind == L"m") {
            for (const auto& run : block.runs)
                if ((run.flags & 256) && block.text.substr(run.start, run.length).find(L"\\frac{a}{b}") != std::wstring::npos)
                    display = block.text.starts_with(L"$$") && block.text.ends_with(L"$$");
        }
    }
    Check(display, "standalone display formula has math block and source delimiters");
    Check(pulse::preview::MakeMarkdownDocument(L"**Bold $x^2$** and [linked $y_i$](https://example.com)\n", text) &&
        HasMath(Parse(text), L"$x^2$", 128) && HasMath(Parse(text), L"$y_i$", 128),
        "math survives nested emphasis and link spans");
    for (const auto& sample : {L"`$x$`\n\n```tex\n$$x^2$$\n```\n", L"Escaped \\$x\\$ and unclosed $y\n"}) {
        Check(pulse::preview::MakeMarkdownDocument(sample, text) && NoMath(Parse(text)), "code and escaped/unclosed dollars remain literal");
    }
    text.clear();
    Check(pulse::preview::AppendMarkdownBlocks(L"Notebook $x_i$\n\n$$x^2$$\n", text, 10000, 1) &&
        HasMath(Parse(text), L"$x_i$", 128) && HasMath(Parse(text), L"$$x^2$$", 256, L"m"),
        "notebook Markdown blocks share inline and display math parsing");
}
ComPtr<IDWriteTextLayout> Layout(IDWriteFactory2* factory, std::wstring_view text, float size, float width) {
    ComPtr<IDWriteTextFormat> format;
    ComPtr<IDWriteTextLayout> layout;
    if (SUCCEEDED(factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-US", &format))) {
        factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format.Get(), width, 2000, &layout);
    }
    return layout;
}
const std::vector<std::wstring> formulas = {
    LR"(E=mc^2)", LR"(\frac{a+b}{c+d})", LR"(x_i^2 + y^{n+1})", LR"(\sqrt{x^2+y^2})",
    LR"(\sum_{i=1}^{n} i^2)", LR"(\int_{0}^{\infty} e^{-x} dx)",
    LR"(\left(\frac{a}{b}\right))", LR"(\begin{pmatrix}a & b \\ c & d\end{pmatrix})"
};
const std::vector<std::wstring> advanced_formulas = {
    LR"(\frac{1}{1+\frac{1}{x}})", LR"(\sqrt[3]{x+1})",
    LR"(\begin{cases}x^2 & x>0 \\ 0 & x=0\end{cases})",
    LR"(\begin{aligned}a &= b+c \\ d &= e+f\end{aligned})",
    LR"(\textbf{with spaces})", LR"(\hat{x}+\bar{y}+\vec{z}+\dot{a}+\ddot{b}+\tilde{c})",
    LR"(\mathbf{A} + \mathit{x} + \mathrm{sin}(x))"
};
const std::vector<std::wstring> stage2_formulas = {
    LR"(f'(x)+f''(x)+x_i'+x'^2)", LR"(\binom{n}{k}+\dbinom{n}{k}+\tbinom{n}{k})",
    LR"(\mathbb{R}+\mathbb{N}+\mathbb{C}+\mathbb{AZaz09})",
    LR"(ab+a\!b+\!a+a\!+a\!\!\!b)",
    LR"(\!a)", LR"(a\!)", LR"(a\!\!\!b)",
    LR"(\displaystyle\sum_{i=1}^{n}i)", LR"(\textstyle\sum_{i=1}^{n}i)",
    LR"(x+{\scriptstyle x}+{\scriptscriptstyle x}+x)",
    LR"(\frac{1}{\frac{2}{3}}+\dfrac{1}{\frac{2}{3}}+\tfrac{1}{\frac{2}{3}})",
    LR"(\textstyle{\displaystyle x}+\sum_{i=1}^{n}i)"
};
const std::wstring stage2_source = LR"(# Stage 2 math

Derivative $f'(x)$ and repeated prime $f''(x)$ retain their source when copied.
Subscript $x_i'$ and explicit exponent $x'^2$ combine with primes.

$$\binom{n}{k}+\dbinom{n}{k}+\tbinom{n}{k}$$

Blackboard bold $\mathbb{R}$, $\mathbb{N}$, $\mathbb{C}$, and supplementary $𝔸$.

$$\mathbb{AZaz09}$$

Spacing: $ab$, $a\!b$, $\!a$, $a\!$, $a\!\!\!b$.

$$\displaystyle\sum_{i=1}^{n}i$$

$$\textstyle\sum_{i=1}^{n}i$$

Scoped styles: $x+{\scriptstyle x}+{\scriptscriptstyle x}+x$.

$$\frac{1}{\frac{2}{3}}+\dfrac{1}{\frac{2}{3}}+\tfrac{1}{\frac{2}{3}}$$

- Prime $x_i'$ in a list.
- Unsupported $\unknowncommand{x}$ remains literal.

| Meaning | Formula |
| --- | --- |
| Choose | $\binom{n}{k}$ |
| Numbers | $\mathbb{R}$ |

`$f'(x)$` is code; escaped \$x\$ is literal.
)";
void TestStage2(IDWriteFactory2* factory) {
    struct Measurement { bool valid; float width, height; };
    const auto measure = [factory](std::wstring_view source, bool display = false) {
        auto layout = Layout(factory, source, 24, 10000);
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
    Check(equal(measure(L"x'"), measure(LR"(x^{\prime})")), "stage2 prime matches explicit prime superscript");
    Check(equal(measure(L"x''"), measure(LR"(x^{\prime\prime})")), "stage2 repeated primes match explicit superscript");
    Check(equal(measure(L"x_i'"), measure(LR"(x_i^{\prime})")) &&
        equal(measure(L"x'^2"), measure(LR"(x^{\prime 2})")), "stage2 primes combine with subscript and explicit exponent");
    Check(equal(measure(L"x_i'^2"), measure(LR"(x_i^{\prime 2})")),
        "stage2 subscript before primes allows immediate explicit exponent");
    const auto binomial = measure(LR"(\binom{n}{k})"), plain = measure(L"(nk)");
    Check(binomial.valid && plain.valid && binomial.height > plain.height + 3, "stage2 binomial vertically stacks operands");
    Check(equal(measure(LR"(\mathbb{R})"), measure(LR"(\mathrm{ℝ})")) &&
        equal(measure(LR"(\mathbb{A})"), measure(LR"(\mathrm{𝔸})")),
        "stage2 mathbb matches actual BMP and supplementary double-struck glyphs");
    Check(equal(measure(LR"(\mathbb{R})"), measure(L"ℝ")) &&
        equal(measure(LR"(\mathbb{CHNPQRZ})"), measure(L"ℂℍℕℙℚℝℤ")),
        "stage2 literal BMP double-struck letters stay upright and match mathbb");
    const auto normal = measure(L"ab"), tight = measure(LR"(a\!b)");
    Check(normal.valid && tight.valid && tight.width < normal.width - 1, "stage2 negative space reduces advance");
    for (const auto& source : {LR"(\!a)", LR"(a\!)", LR"(a\!\!\!b)"}) {
        const auto m = measure(source);
        Check(m.valid && std::isfinite(m.width) && m.width > 0 && m.height > 0, "stage2 negative spacing retains finite visible bounds");
    }
    const auto display = measure(LR"(\displaystyle\sum_{i=1}^{n}i)"),
        text = measure(LR"(\textstyle\sum_{i=1}^{n}i)");
    Check(display.valid && text.valid && display.height > text.height + 3, "stage2 display sum has larger stacked limits than text sum");
    Check(equal(measure(LR"(\textstyle{\displaystyle x}+\sum_{i=1}^{n}i)"),
        measure(LR"(\textstyle{x}+\sum_{i=1}^{n}i)")), "stage2 group restores outer style before next sum");
    const auto base = measure(L"x"), script = measure(LR"(\scriptstyle x)"),
        scriptscript = measure(LR"(\scriptscriptstyle x)");
    Check(base.valid && script.valid && scriptscript.valid && base.height > script.height && script.height > scriptscript.height,
        "stage2 script and scriptscript styles progressively shrink");
    Check(equal(script, measure(LR"(\scriptstyle\scriptstyle x)")), "stage2 repeated style declarations do not repeatedly shrink");
    const auto fraction = measure(LR"(\frac{a}{b})"), dfrac = measure(LR"(\dfrac{a}{b})"),
        tfrac = measure(LR"(\tfrac{a}{b})");
    Check(fraction.valid && dfrac.valid && tfrac.valid && dfrac.height > tfrac.height + 2 &&
        equal(fraction, tfrac), "stage2 inline frac inherits text style and dfrac is larger");
    Check(equal(measure(LR"(\frac{a}{b})", true), dfrac), "stage2 display frac inherits display style");
    for (const std::wstring source : {std::wstring(LR"(x^2')"), std::wstring(LR"(x^2^3)"), std::wstring(LR"(x'_i^2)"),
        std::wstring(LR"(\binom{n})"), std::wstring(LR"(\mathbb{\alpha})"), std::wstring(LR"(\unknownstyle x)"),
        std::wstring(4097, L'x')}) {
        auto layout = Layout(factory, source, 24, 600);
        ComPtr<IDWriteInlineObject> object;
        Check(layout && !pulse::ui::ApplyMathInline(factory, layout.Get(), {0, static_cast<UINT32>(source.size())}, source, 24, false, 600) &&
            SUCCEEDED(layout->GetInlineObject(0, &object)) && !object, "stage2 malformed unsupported and oversized source remain untouched");
    }
    std::wstring payload;
    pulse::ui::MarkdownView view;
    Check(pulse::preview::MakeMarkdownDocument(stage2_source, payload) && view.SetPayload(payload, L"bench_data/math_preview.md") &&
        view.Source() == stage2_source && view.PlainText().find(L"$f'(x)$") != std::wstring::npos &&
        view.PlainText().find(L"$𝔸$") != std::wstring::npos, "stage2 source and copied text retain primes and supplementary Unicode");
}
void TestMath(IDWriteFactory2* factory) {
    const auto started = std::chrono::steady_clock::now();
    auto valid_formulas = formulas;
    const std::vector<std::wstring> additional = {
        LR"(\frac{1}{1+\frac{1}{x}})", LR"(\sqrt[3]{x+1})",
        LR"(\begin{cases}x^2 & x>0 \\ 0 & x=0\end{cases})",
        LR"(\begin{aligned}a &= b+c \\ d &= e+f\end{aligned})",
        LR"(\textbf{with spaces})", LR"(\hat{x}+\bar{y}+\vec{z}+\dot{a})"
    };
    valid_formulas.insert(valid_formulas.end(), additional.begin(), additional.end());
    for (const auto& formula : valid_formulas) {
        auto layout = Layout(factory, formula, 20, 600);
        const bool applied = layout && pulse::ui::ApplyMathInline(factory, layout.Get(),
            {0, static_cast<UINT32>(formula.size())}, formula, 20, true, 600);
        ComPtr<IDWriteInlineObject> object;
        DWRITE_INLINE_OBJECT_METRICS metrics{};
        const bool valid = applied && SUCCEEDED(layout->GetInlineObject(0, &object)) && object &&
            SUCCEEDED(object->GetMetrics(&metrics)) && std::isfinite(metrics.width) && metrics.width > 0 &&
            metrics.height > 0 && metrics.baseline > 0 && metrics.baseline <= metrics.height && metrics.width <= 600.5f;
        Check(valid, "supported formula creates finite inline geometry and baseline");
        if (!valid) std::wprintf(L"  Formula: %ls\n", formula.c_str());
    }
    std::vector<std::wstring> invalid{LR"(\unknowncommand{x})", LR"(\frac{a}{b)", LR"(x^{)",
        LR"(x^1^2)", LR"(\begin{matrix}a&b\end{pmatrix})", LR"(\begin{matrix}a&b)",
        LR"(\left(x)", LR"(x\right))", LR"(\frac{a})", LR"(\sqrt)", LR"(x^2')",
        std::wstring(200, L'{') + L"x" + std::wstring(200, L'}'), std::wstring(20000, L'x')};
    for (const auto& formula : invalid) {
        auto layout = Layout(factory, formula, 20, 600);
        const bool rejected = layout && !pulse::ui::ApplyMathInline(factory, layout.Get(),
            {0, static_cast<UINT32>(formula.size())}, formula, 20, false, 600);
        ComPtr<IDWriteInlineObject> object;
        const bool unchanged = layout && SUCCEEDED(layout->GetInlineObject(0, &object)) && !object;
        Check(rejected && unchanged, "malformed unsupported or over-budget formula keeps raw text");
    }
    float text_widths[2]{};
    bool text_ok = true;
    for (int i = 0; i < 2; ++i) {
        const std::wstring formula = i == 0 ? LR"(\textbf{a b})" : LR"(\textbf{ab})";
        auto text_layout = Layout(factory, formula, 20, 600);
        ComPtr<IDWriteInlineObject> text_object;
        DWRITE_INLINE_OBJECT_METRICS text_metrics{};
        text_ok = text_layout && pulse::ui::ApplyMathInline(factory, text_layout.Get(),
            {0, static_cast<UINT32>(formula.size())}, formula, 20, false, 600) &&
            SUCCEEDED(text_layout->GetInlineObject(0, &text_object)) && text_object &&
            SUCCEEDED(text_object->GetMetrics(&text_metrics)) && text_ok;
        text_widths[i] = text_metrics.width;
    }
    Check(text_ok && text_widths[0] > text_widths[1] + 1, "textbf keeps internal space width");
    float accent_heights[3]{};
    const std::wstring accents[] = {LR"(\dot{a})", LR"(\bar{a})", LR"(\hat{a})"};
    bool accents_ok = true;
    for (size_t i = 0; i < 3; ++i) {
        auto accent_layout = Layout(factory, accents[i], 20, 600);
        ComPtr<IDWriteInlineObject> accent_object;
        DWRITE_INLINE_OBJECT_METRICS accent_metrics{};
        accents_ok = accent_layout && pulse::ui::ApplyMathInline(factory, accent_layout.Get(),
            {0, static_cast<UINT32>(accents[i].size())}, accents[i], 20, false, 600) &&
            SUCCEEDED(accent_layout->GetInlineObject(0, &accent_object)) && accent_object &&
            SUCCEEDED(accent_object->GetMetrics(&accent_metrics)) && accents_ok;
        accent_heights[i] = accent_metrics.height;
    }
    Check(accents_ok && accent_heights[0] <= std::max(accent_heights[1], accent_heights[2]) + 6.0f,
        "dot accent stays within 0.3em of bar and hat instead of floating above base");
    std::printf("[METRICS] accent heights dot %.2f bar %.2f hat %.2f\n",
        accent_heights[0], accent_heights[1], accent_heights[2]);
    const std::wstring fraction = LR"(\frac{a+b}{c+d})";
    const std::wstring multiline = L"Before " + fraction + L" after\nNext line";
    auto layout = Layout(factory, multiline, 20, 600);
    bool rows_ok = layout && pulse::ui::ApplyMathInline(factory, layout.Get(),
        {7, static_cast<UINT32>(fraction.size())}, fraction, 20, false, 500);
    DWRITE_LINE_METRICS lines[4]{};
    UINT32 count = 0;
    ComPtr<IDWriteInlineObject> object;
    DWRITE_INLINE_OBJECT_METRICS object_metrics{};
    rows_ok = rows_ok && SUCCEEDED(layout->GetLineMetrics(lines, 4, &count)) && count == 2 &&
        SUCCEEDED(layout->GetInlineObject(7, &object)) && object && SUCCEEDED(object->GetMetrics(&object_metrics)) &&
        lines[0].height >= object_metrics.height && lines[0].baseline >= object_metrics.baseline;
    Check(rows_ok, "inline fraction reserves ascent and descent before following line");
    const std::wstring wide = LR"(a+b+c+d)";
    layout = Layout(factory, wide, 20, 80);
    const bool fitted = layout && pulse::ui::ApplyMathInline(factory, layout.Get(),
        {0, static_cast<UINT32>(wide.size())}, wide, 20, true, 80);
    object.Reset();
    Check(fitted && SUCCEEDED(layout->GetInlineObject(0, &object)) && object &&
        SUCCEEDED(object->GetMetrics(&object_metrics)) && object_metrics.width <= 80.5f,
        "narrow display math obeys available width");
    const std::wstring excessive = LR"(a+b+c+d+e+f+g+h+i+j+k+l+m+n+o+p)";
    layout = Layout(factory, excessive, 20, 80);
    Check(layout && !pulse::ui::ApplyMathInline(factory, layout.Get(), {0, static_cast<UINT32>(excessive.size())},
        excessive, 20, true, 80), "formula too wide for readable fitting preserves source");
    const auto fixture_end = std::chrono::steady_clock::now();
    std::wstring bounded = L"x";
    for (int i = 0; i < 100; ++i) bounded += LR"(+\frac{x_i}{i+1})";
    layout = Layout(factory, bounded, 20, 100000);
    Check(layout && pulse::ui::ApplyMathInline(factory, layout.Get(), {0, static_cast<UINT32>(bounded.size())},
        bounded, 20, true, 100000), "larger bounded expression remains supported");
    const auto ended = std::chrono::steady_clock::now();
    std::printf("[TIME] representative checks %.2f ms; 100 fractions %.2f ms\n",
        std::chrono::duration<double, std::milli>(fixture_end - started).count(),
        std::chrono::duration<double, std::milli>(ended - fixture_end).count());
}
bool SavePng(ID2D1DeviceContext* dc, ID2D1Bitmap1* bitmap, const std::filesystem::path& path) {
    ComPtr<ID2D1Bitmap1> readable;
    auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        bitmap->GetPixelFormat());
    const auto size = bitmap->GetPixelSize();
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, props, &readable)) ||
        FAILED(readable->CopyFromBitmap(nullptr, bitmap, nullptr))) return false;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    const bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
        SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(size.width, size.height)) &&
        SUCCEEDED(frame->SetPixelFormat(&format)) && SUCCEEDED(frame->WritePixels(size.height, mapped.pitch, mapped.pitch * size.height, mapped.bits)) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    readable->Unmap();
    return ok;
}
bool InkInsideBounds(ID2D1DeviceContext* dc, ID2D1Bitmap1* bitmap, bool dark, float scale,
    const std::vector<D2D1_RECT_F>& bounds, const std::vector<D2D1_RECT_F>& accent_bounds = {}) {
    ComPtr<ID2D1Bitmap1> readable;
    const auto size = bitmap->GetPixelSize();
    const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        bitmap->GetPixelFormat());
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, props, &readable)) ||
        FAILED(readable->CopyFromBitmap(nullptr, bitmap, nullptr))) return false;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
    const int background[] = {dark ? 0x1c : 255, dark ? 0x19 : 255, dark ? 0x17 : 255};
    bool valid = true;
    size_t ink = 0;
    for (UINT32 y = 0; y < size.height && valid; ++y) {
        for (UINT32 x = 0; x < size.width; ++x) {
            const BYTE* pixel = mapped.bits + y * mapped.pitch + x * 4;
            if (std::abs(static_cast<int>(pixel[0]) - background[0]) <= 3 &&
                std::abs(static_cast<int>(pixel[1]) - background[1]) <= 3 &&
                std::abs(static_cast<int>(pixel[2]) - background[2]) <= 3) continue;
            ++ink;
            const float px = (static_cast<float>(x) + 0.5f) / scale;
            const float py = (static_cast<float>(y) + 0.5f) / scale;
            const bool inside = std::any_of(bounds.begin(), bounds.end(), [px, py](const auto& r) {
                return px >= r.left - 2 && px <= r.right + 2 && py >= r.top - 2 && py <= r.bottom + 2;
            });
            if (!inside) {
                std::printf("[INK] outside advertised layout bounds at %.2f, %.2f\n", px, py);
                valid = false;
                break;
            }
        }
    }
    for (const auto& r : accent_bounds) {
        const UINT32 left = static_cast<UINT32>(std::max(0.0f, std::floor((r.left - 1) * scale)));
        const UINT32 right = static_cast<UINT32>(std::min(static_cast<float>(size.width), std::ceil((r.right + 1) * scale)));
        const UINT32 top = static_cast<UINT32>(std::max(0.0f, std::floor((r.top - 1) * scale)));
        const UINT32 bottom = static_cast<UINT32>(std::min(static_cast<float>(size.height), std::ceil((r.bottom + 1) * scale)));
        bool seen_ink = false;
        UINT32 gap = 0, max_gap = 0;
        for (UINT32 y = top; y < bottom; ++y) {
            bool row_ink = false;
            for (UINT32 x = left; x < right; ++x) {
                const BYTE* pixel = mapped.bits + y * mapped.pitch + x * 4;
                if (std::abs(static_cast<int>(pixel[0]) - background[0]) > 3 ||
                    std::abs(static_cast<int>(pixel[1]) - background[1]) > 3 ||
                    std::abs(static_cast<int>(pixel[2]) - background[2]) > 3) { row_ink = true; break; }
            }
            if (row_ink) {
                if (seen_ink) max_gap = std::max(max_gap, gap);
                seen_ink = true;
                gap = 0;
            } else if (seen_ink) ++gap;
        }
        const float gap_dip = static_cast<float>(max_gap) / scale;
        const bool close = seen_ink && gap_dip <= 8.0f;
        Check(close, "stage4 accent ink stays within 0.25em plus antialias allowance of base ink");
        std::printf("[INK] accent maximum internal blank gap %.2f DIP\n", gap_dip);
        valid = valid && close;
    }
    readable->Unmap();
    return valid && ink > 100;
}
bool Render(IDWriteFactory2* factory, bool dark, float scale, bool actual_view = false, bool advanced = false,
    int stage = 0, const pulse_test::MathRenderProfile* profile = nullptr) {
    const bool stage2 = stage == 2, stage3 = stage == 3, stage4 = stage == 4, stage5 = stage == 5, architecture = stage == 6;
    ComPtr<ID3D11Device> d3d;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Device> device;
    ComPtr<ID2D1DeviceContext> dc;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr)) || FAILED(d3d.As(&dxgi)) ||
        FAILED(D2D1CreateDevice(dxgi.Get(), nullptr, &device)) ||
        FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc))) return false;
    const float width = actual_view ? 1120.0f : 760.0f;
    float height = actual_view ? (profile ? profile->markdown_height : 1500.0f) : 1050.0f;
    std::vector<ComPtr<IDWriteTextLayout>> advanced_layouts;
    std::vector<float> advanced_heights;
    std::vector<float> advanced_widths;
    std::vector<ComPtr<IDWriteTextLayout>> advanced_labels;
    std::vector<float> advanced_label_heights;
    const auto& sheet_formulas = profile ? profile->formulas : architecture ? pulse_test::MathArchitectureFormulas() : stage5 ? pulse_test::MathStage5Formulas() : stage4 ? pulse_test::MathStage4Formulas() : stage3 ? pulse_test::MathStage3Formulas() : stage2 ? stage2_formulas : advanced_formulas;
    if (advanced) {
        height = 80;
        for (const auto& formula : sheet_formulas) {
            auto layout = Layout(factory, formula, 24, width - 48);
            auto label = Layout(factory, formula, 12, width - 48);
            DWRITE_TEXT_METRICS metrics{};
            DWRITE_TEXT_METRICS label_metrics{};
            if (!layout || !pulse::ui::ApplyMathInline(factory, layout.Get(), {0, static_cast<UINT32>(formula.size())},
                formula, 24, true, width - 48) || FAILED(layout->GetMetrics(&metrics)) ||
                !label || FAILED(label->GetMetrics(&label_metrics))) return false;
            advanced_layouts.push_back(std::move(layout));
            advanced_heights.push_back(metrics.height);
            advanced_widths.push_back(metrics.widthIncludingTrailingWhitespace);
            advanced_labels.push_back(std::move(label));
            advanced_label_heights.push_back(label_metrics.height);
            height += metrics.height + label_metrics.height + 44;
        }
        height = std::ceil(height);
    }
    ComPtr<ID2D1Bitmap1> bitmap;
    auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        actual_view ? 96 : 96 * scale, actual_view ? 96 : 96 * scale);
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(width * scale), static_cast<UINT32>(height * scale)),
        nullptr, 0, props, &bitmap))) return false;
    dc->SetDpi(actual_view ? 96 : 96 * scale, actual_view ? 96 : 96 * scale);
    dc->SetTarget(bitmap.Get());
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(dark ? 0xe5e7eb : 0x202124), &brush))) return false;
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(dark ? 0x17191c : 0xffffff));
    if (advanced) {
        std::vector<D2D1_RECT_F> ink_bounds;
        std::vector<D2D1_RECT_F> accent_bounds;
        auto title = Layout(factory, profile ? profile->title : architecture ? L"Math architecture / shared reference corpus" : stage5 ? L"Stage 5 math / measured row heights" : stage4 ? L"Stage 4 math / measured row heights" : stage3 ? L"Stage 3 math / measured row heights" : stage2 ? L"Stage 2 math / measured row heights" : L"Advanced math / measured row heights", 20, width - 48);
        dc->DrawTextLayout(D2D1::Point2F(24, 20), title.Get(), brush.Get());
        DWRITE_TEXT_METRICS title_metrics{};
        if (title) title->GetMetrics(&title_metrics);
        ink_bounds.push_back(D2D1::RectF(24, 20, width - 24, 20 + title_metrics.height));
        float y = 64;
        for (size_t i = 0; i < sheet_formulas.size(); ++i) {
            dc->DrawTextLayout(D2D1::Point2F(24, y), advanced_labels[i].Get(), brush.Get());
            ink_bounds.push_back(D2D1::RectF(24, y, width - 24, y + advanced_label_heights[i]));
            y += advanced_label_heights[i] + 12;
            pulse::ui::DrawMathTextLayout(dc.Get(), advanced_layouts[i].Get(), D2D1::Point2F(24, y), brush.Get());
            ink_bounds.push_back(D2D1::RectF(24, y, 24 + advanced_widths[i], y + advanced_heights[i]));
            if (stage4 && (sheet_formulas[i] == LR"(\acute{a})" || sheet_formulas[i] == LR"(\grave{a})" ||
                sheet_formulas[i] == LR"(\breve{a})" || sheet_formulas[i] == LR"(\check{a})" || sheet_formulas[i] == LR"(\mathring{a})"))
                accent_bounds.push_back(ink_bounds.back());
            y += advanced_heights[i] + 32;
        }
        bool valid = SUCCEEDED(dc->EndDraw());
        dc->SetTarget(nullptr);
        if (stage2 || stage3 || stage4 || stage5 || architecture || profile) {
            const bool bounded = valid && InkInsideBounds(dc.Get(), bitmap.Get(), dark, scale, ink_bounds, accent_bounds);
            Check(bounded, architecture ? "architecture corpus ink stays inside advertised layout bounds" : stage5 ? "stage5 rendered ink stays inside advertised layout bounds" : stage4 ? "stage4 rendered ink stays inside advertised layout bounds" : stage3 ? "stage3 rendered ink stays inside advertised layout bounds" : "stage2 rendered ink stays inside advertised layout bounds");
            valid = valid && bounded;
        }
        std::filesystem::create_directories(L"bench_data");
        const std::wstring filename = profile ? profile->file_prefix + (dark ? L"dark-" : L"light-") + std::to_wstring(static_cast<int>(scale * 100)) + L".png" : (stage2 || stage3 || stage4 || stage5 || architecture) ? (std::wstring(architecture ? L"math_preview-architecture-" : stage5 ? L"math_preview-stage5-" : stage4 ? L"math_preview-stage4-" : stage3 ? L"math_preview-stage3-" : L"math_preview-stage2-") + (dark ? L"dark-" : L"light-") +
            std::to_wstring(static_cast<int>(scale * 100)) + L".png") :
            (dark ? L"math_preview-advanced-dark.png" : L"math_preview-advanced-light.png");
        return valid && SavePng(dc.Get(), bitmap.Get(), std::filesystem::path(L"bench_data") / filename);
    }
    if (actual_view) {
        const std::wstring source = profile ? profile->markdown : architecture ? pulse_test::MathArchitectureSource() : stage5 ? pulse_test::MathStage5Source() : stage4 ? pulse_test::MathStage4Source() : stage3 ? pulse_test::MathStage3Source() : stage2 ? stage2_source : LR"(# Markdown math preview

Prose $E=mc^2$ continues after a formula. An inline fraction $\frac{a+b}{c+d}$ reserves line height.
The next sentence verifies paragraph wrapping and separation.

$$\frac{-b \pm \sqrt{b^2-4ac}}{2a}$$

$$\sum_{i=1}^{n} i^2 = \frac{n(n+1)(2n+1)}{6}$$

$$\int_{0}^{\infty} e^{-x} dx = 1$$

$$\left(\frac{a}{b}\right)$$

$$\begin{pmatrix}a & b \\ c & d\end{pmatrix}$$

- First $x_i^2$ and **bold $\alpha$**.
- Second $\frac{1}{2}$ follows without overlap.

| Meaning | Formula |
| --- | --- |
| Fraction | $\frac{a}{b}$ |
| Power | $x^{n+1}$ |

> Quoted $\sqrt{x^2+y^2}$ with a baseline.

Unsupported $\unknowncommand{x}$ remains visible.
Malformed $\frac{a}{b$ remains visible.
Escaped \$x\$ and `$x^2$` stay literal.

```tex
$$\frac{a}{b}$$
```
)";
        std::wstring payload;
        bool valid = pulse::preview::MakeMarkdownDocument(source, payload);
        pulse::ui::MarkdownView view;
        const auto theme = pulse::ui::MakeTheme(dark, pulse::ui::HexColor(0x0078d4));
        for (int column = 0; column < 2; ++column) {
            view.Clear();
            valid = view.SetPayload(payload, L"bench_data/math_preview.md") && valid;
            valid = view.Source() == source && view.PlainText().find(profile ? profile->copy_probe : architecture ? LR"(\(a+b=c\))" : stage5 ? LR"(\(\boxed{x}\))" : stage4 ? LR"(\(\acute{x}\))" : stage3 ? LR"(\(x_i^2\))" : stage2 ? L"$f'(x)$" : L"$\\frac{a+b}{c+d}$") != std::wstring::npos && valid;
            const auto rect = D2D1::RectF((column ? 760.0f : 0.0f) * scale, 0,
                (column ? 1120.0f : 740.0f) * scale, height * scale);
            view.Draw(dc.Get(), factory, rect, theme, dark, scale, nullptr, 1, 0, 0, {});
            uint32_t offset = 0;
            valid = view.HitTest(rect.left + 50 * scale, 65 * scale, offset) && offset <= view.PlainText().size() && valid;
        }
        std::wstring selection_payload;
        const std::wstring selection_source = profile ? profile->copy_probe : architecture ? LR"(\(a+b=c\))" : stage5 ? LR"(\(\boxed{x}\))" : stage4 ? LR"(\(\acute{x}\))" : stage3 ? LR"(\(x_i^2\))" : stage2 ? L"$f'(𝔸)$" : L"$x^2$";
        valid = pulse::preview::MakeMarkdownDocument(selection_source, selection_payload) &&
            view.SetPayload(selection_payload, L"bench_data/math_preview.md") && valid;
        const auto hidden_rect = D2D1::RectF(0, (height + 20) * scale, 740 * scale, (height + 200) * scale);
        view.Draw(dc.Get(), factory, hidden_rect, theme, dark, scale, nullptr, 2, 0, 0, {});
        uint32_t trailing_offset = 0;
        valid = view.HitTest(700 * scale, hidden_rect.top + 30 * scale, trailing_offset) &&
            trailing_offset == selection_source.size() && view.PlainText().substr(0, trailing_offset) == selection_source && valid;
        if (stage3 || stage4 || stage5 || architecture || profile) {
            bool atomic = true, saw_start = false, saw_end = false;
            for (int y = 0; y < 90; y += 3) {
                for (int x = 0; x < 300; x += 3) {
                    uint32_t offset = 0;
                    if (!view.HitTest(static_cast<float>(x) * scale, hidden_rect.top + static_cast<float>(y) * scale, offset)) {
                        atomic = false;
                    } else {
                        atomic = atomic && (offset == 0 || offset == selection_source.size());
                        saw_start = saw_start || offset == 0;
                        saw_end = saw_end || offset == selection_source.size();
                    }
                }
            }
            Check(atomic && saw_start && saw_end, architecture ? "architecture actual view renders formula atomically" : stage5 ? "stage5 actual view treats formula as one rendered object, not raw characters" : stage4 ? "stage4 actual view treats formula as one rendered object, not raw characters" : "stage3 actual view treats formula as one rendered object, not raw characters");
            valid = valid && atomic && saw_start && saw_end;
        }
        Check(valid, "actual view trailing math hit copies entire source span with dollars");
        view.Clear();
        valid = !view.HasData() && view.PlainText().empty() && valid;
        valid = SUCCEEDED(dc->EndDraw()) && valid;
        dc->SetTarget(nullptr);
        std::filesystem::create_directories(L"bench_data");
        const auto path = std::filesystem::path(L"bench_data") / ((profile ? profile->file_prefix + L"view-" : std::wstring(architecture ? L"math_preview-architecture-view-" : stage5 ? L"math_preview-stage5-view-" : stage4 ? L"math_preview-stage4-view-" : stage3 ? L"math_preview-stage3-view-" : stage2 ? L"math_preview-stage2-view-" : L"math_preview-view-")) +
            (dark ? L"dark-" : L"light-") + std::to_wstring(static_cast<int>(scale * 100)) + L".png");
        return valid && SavePng(dc.Get(), bitmap.Get(), path);
    }
    auto title = Layout(factory, dark ? L"Formula preview / dark / wide + narrow" : L"Formula preview / light / wide + narrow", 20, 720);
    dc->DrawTextLayout(D2D1::Point2F(20, 16), title.Get(), brush.Get());
    float y = 64;
    bool ok = true;
    for (const auto& formula : formulas) {
        auto label = Layout(factory, formula, 12, 720);
        dc->DrawTextLayout(D2D1::Point2F(20, y), label.Get(), brush.Get());
        for (int column = 0; column < 2; ++column) {
            const float available = column == 0 ? 440.0f : 220.0f;
            auto layout = Layout(factory, formula, 22, available);
            ok = layout && pulse::ui::ApplyMathInline(factory, layout.Get(), {0, static_cast<UINT32>(formula.size())},
                formula, 22, true, available) && ok;
            if (layout) pulse::ui::DrawMathTextLayout(dc.Get(), layout.Get(), D2D1::Point2F(column == 0 ? 24.0f : 510.0f, y + 24), brush.Get());
        }
        y += 106;
    }
    const std::wstring inline_source = LR"(Line one with \frac{a}{b} followed by prose.)";
    auto inline_layout = Layout(factory, inline_source + L"\nLine two must not overlap the fraction.", 20, 700);
    ok = inline_layout && pulse::ui::ApplyMathInline(factory, inline_layout.Get(), {14, 11}, LR"(\frac{a}{b})", 20, false, 600) && ok;
    if (inline_layout) pulse::ui::DrawMathTextLayout(dc.Get(), inline_layout.Get(), D2D1::Point2F(24, 940), brush.Get());
    ok = SUCCEEDED(dc->EndDraw()) && ok;
    dc->SetTarget(nullptr);
    std::filesystem::create_directories(L"bench_data");
    const auto path = std::filesystem::path(L"bench_data") / (std::wstring(L"math_preview-") + (dark ? L"dark-" : L"light-") +
        std::to_wstring(static_cast<int>(scale * 100)) + L".png");
    return ok && SavePng(dc.Get(), bitmap.Get(), path);
}
}
int wmain(int argc, wchar_t** argv) {
    bool stage2 = false, stage3 = false, stage4 = false, stage5 = false, render = false, architecture = false, compatibility = false, structures = false, reliable = false;
    for (int i = 1; i < argc; ++i) {
        stage2 = stage2 || std::wstring_view(argv[i]) == L"--stage2";
        stage3 = stage3 || std::wstring_view(argv[i]) == L"--stage3";
        stage4 = stage4 || std::wstring_view(argv[i]) == L"--stage4";
        stage5 = stage5 || std::wstring_view(argv[i]) == L"--stage5";
        architecture = architecture || std::wstring_view(argv[i]) == L"--architecture";
        compatibility = compatibility || std::wstring_view(argv[i]) == L"--compat";
        structures = structures || std::wstring_view(argv[i]) == L"--structures";
        reliable = reliable || std::wstring_view(argv[i]) == L"--reliable";
        render = render || std::wstring_view(argv[i]) == L"--render";
    }
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IDWriteFactory2> factory;
    Check(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2),
        reinterpret_cast<IUnknown**>(factory.GetAddressOf()))), "create DirectWrite factory");
    if (structures || reliable) {
        if (factory) failures += reliable ? pulse_test::TestMathReliable(factory.Get()) : pulse_test::TestMathStructures(factory.Get());
        if (factory && render) {
            const auto& profile = reliable ? pulse_test::MathReliableProfile() : pulse_test::MathStructuresProfile();
            for (float scale : {1.0f, 1.5f, 2.0f})
                for (bool dark : {false, true}) {
                    Check(Render(factory.Get(), dark, scale, false, true, 0, &profile), "render complete selected gallery with bounded ink");
                    Check(Render(factory.Get(), dark, scale, true, false, 0, &profile), "render complete selected Markdown wide/narrow and source copy");
                }
        }
        factory.Reset();
        if (SUCCEEDED(com)) CoUninitialize();
        std::printf("Math selected gallery tests: %d failure(s)\n", failures);
        return failures ? 1 : 0;
    }
    if (architecture || compatibility) {
        if (factory) failures += pulse_test::TestMathArchitecture(factory.Get(), compatibility && !architecture);
    } else if (stage5) {
        if (factory) failures += pulse_test::TestMathStage5(factory.Get());
    } else if (stage4) {
        if (factory) failures += pulse_test::TestMathStage4(factory.Get());
    } else if (stage3) {
        if (factory) failures += pulse_test::TestMathStage3(factory.Get());
    } else if (stage2) {
        if (factory) TestStage2(factory.Get());
    } else {
        TestMarkdown();
        if (factory) TestMath(factory.Get());
    }
    if (factory && render && (architecture || compatibility)) {
        for (float scale : {1.0f, 1.5f, 2.0f})
            for (bool dark : {false, true}) {
                Check(Render(factory.Get(), dark, scale, false, true, 6), "render architecture reference corpus at requested DPI");
                Check(Render(factory.Get(), dark, scale, true, false, 6), "render architecture Markdown wide/narrow and source copy");
            }
    }
    if (factory && render && stage5 && !architecture && !compatibility) {
        for (float scale : {1.0f, 1.5f, 2.0f})
            for (bool dark : {false, true}) {
                Check(Render(factory.Get(), dark, scale, false, true, 5), "render stage5 formulas at requested DPI");
                Check(Render(factory.Get(), dark, scale, true, false, 5), "render stage5 Markdown wide/narrow and full-delimiter copy");
            }
    }
    if (factory && render && stage4 && !stage5 && !architecture && !compatibility) {
        for (float scale : {1.0f, 1.5f, 2.0f})
            for (bool dark : {false, true}) {
                Check(Render(factory.Get(), dark, scale, false, true, 4), "render stage4 formulas at requested DPI");
                Check(Render(factory.Get(), dark, scale, true, false, 4), "render stage4 Markdown wide/narrow and full-delimiter copy");
            }
    }
    if (factory && render && stage3 && !stage4 && !stage5 && !architecture && !compatibility) {
        for (float scale : {1.0f, 1.5f, 2.0f})
            for (bool dark : {false, true}) {
                Check(Render(factory.Get(), dark, scale, false, true, 3), "render stage3 formulas at requested DPI");
                Check(Render(factory.Get(), dark, scale, true, false, 3), "render stage3 Markdown wide/narrow and full-delimiter copy");
            }
    }
    if (factory && render && stage2 && !stage3 && !stage4 && !stage5 && !architecture && !compatibility) {
        for (float scale : {1.0f, 1.5f, 2.0f})
            for (bool dark : {false, true}) {
                Check(Render(factory.Get(), dark, scale, false, true, 2), "render stage2 formulas at requested DPI");
                Check(Render(factory.Get(), dark, scale, true, false, 2), "render stage2 Markdown wide/narrow with prime and Unicode copy hit-test");
            }
    }
    if (factory && render && !stage2 && !stage3 && !stage4 && !stage5 && !architecture && !compatibility) {
        for (float scale : {1.0f, 1.5f, 2.0f})
            for (bool dark : {false, true}) {
                Check(Render(factory.Get(), dark, scale), "render light/dark formula sheet at requested DPI");
                Check(Render(factory.Get(), dark, scale, true), "render actual Markdown view wide/narrow with copy hit-test and reload checks");
            }
        for (bool dark : {false, true})
            Check(Render(factory.Get(), dark, 1, false, true), "render advanced formulas with measured row heights");
    }
    factory.Reset();
    if (SUCCEEDED(com)) CoUninitialize();
    std::printf("Math tests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
