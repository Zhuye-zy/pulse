#include "../ui/math_syntax.h"
#include "../ui/math_font_metrics.h"
#include "../ui/math_alphabet.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace pulse::ui;
int failures = 0;
void Check(bool valid, const char* name) {
    std::printf("[%s] syntax %s\n", valid ? "PASS" : "FAIL", name);
    if (!valid) ++failures;
}
const MathNode* Find(const MathNode& node, MathNodeKind kind) {
    if (node.kind == kind) return &node;
    for (const auto& child : node.children)
        if (const auto* found = Find(child, kind)) return found;
    return nullptr;
}
bool RangesValid(const MathNode& node, size_t source_size) {
    if (node.source_start > source_size || node.source_length > source_size - node.source_start) return false;
    for (const auto& child : node.children) if (!RangesValid(child, source_size)) return false;
    return true;
}
void ExpectError(std::wstring_view source, MathParseErrorCode code, const char* name) {
    const auto result = ParseMathSyntax(source);
    const bool valid = !result && result.error.code == code && result.error.offset <= source.size();
    Check(valid, name);
    if (!valid) std::printf("  error=%d expected=%d offset=%zu source=%zu\n",
        static_cast<int>(result.error.code), static_cast<int>(code), result.error.offset, source.size());
}
void TestFontConstants() {
    using namespace pulse::ui::math;
    std::vector<uint8_t> table(224, 0);
    const auto put = [](std::vector<uint8_t>& bytes, size_t at, uint16_t value) {
        bytes[at] = static_cast<uint8_t>(value >> 8);
        bytes[at + 1] = static_cast<uint8_t>(value & 255);
    };
    put(table, 0, 1);
    put(table, 4, 10);
    put(table, 10, 80);
    put(table, 12, 60);
    for (size_t i = 0; i < 51; ++i) put(table, 18 + 4 * i, 100);
    put(table, 22, 250);
    put(table, 18 + 4 * 34, 50);
    put(table, 18 + 4 * 47, 40);
    FontMathMetrics metrics;
    Check(ParseMathFontTable(table, 1000, metrics) && metrics.from_font &&
        std::abs(metrics.script_scale - 0.8f) < 0.0001f && std::abs(metrics.script_script_scale - 0.6f) < 0.0001f &&
        std::abs(metrics.axis_height - 0.25f) < 0.0001f && std::abs(metrics.fraction_rule_thickness - 0.05f) < 0.0001f &&
        std::abs(metrics.radical_rule_thickness - 0.04f) < 0.0001f,
        "OpenType big-endian constants normalize design units and font script percentages");
    const auto rejects = [&metrics](std::span<const uint8_t> bytes, uint16_t units = 1000) {
        FontMathMetrics unchanged = metrics;
        return !ParseMathFontTable(bytes, units, unchanged) && unchanged.from_font == metrics.from_font &&
            unchanged.axis_height == metrics.axis_height && unchanged.script_scale == metrics.script_scale &&
            unchanged.fraction_rule_thickness == metrics.fraction_rule_thickness;
    };
    bool truncated = true;
    for (size_t length = 0; length < table.size(); ++length)
        truncated = rejects(std::span<const uint8_t>(table.data(), length)) && truncated;
    Check(truncated, "every truncated header/constants prefix fails without mutating output");
    struct InvalidConstant { size_t offset; uint16_t value; };
    for (const auto [offset, value] : std::initializer_list<InvalidConstant>{
        {0, 2}, {2, 1}, {4, 9}, {4, 65535}, {10, 101}, {12, 90}, {22, 65535}, {22, 4001},
        {18 + 4 * 34, 0}, {18 + 4 * 34, 501}, {18 + 4 * 47, 0}}) {
        auto malformed = table;
        put(malformed, offset, value);
        Check(rejects(malformed), "invalid version offset percentages or metric bounds preserve output");
    }
    Check(rejects(table, 0) && rejects(table, 15) && rejects(table, 16385), "invalid font units per em are rejected");
    auto ignored_offsets = table;
    put(ignored_offsets, 6, 65535);
    put(ignored_offsets, 8, 65535);
    for (size_t i = 0; i < 51; ++i) put(ignored_offsets, 20 + 4 * i, 65535);
    FontMathMetrics ignored;
    Check(ParseMathFontTable(ignored_offsets, 1000, ignored) && ignored.axis_height == metrics.axis_height,
        "unneeded glyph and device offsets are never dereferenced");
}
void TestStructureSyntax() {
    const std::wstring ruled = LR"(\begin{array}{|l||r|}\hline a&b\\[1em]\hline\hline c&d\\\hline\end{array})";
    const auto parsed = ParseMathSyntax(ruled);
    const auto* array = Find(parsed.root, MathNodeKind::Environment);
    Check(parsed && array && array->auxiliary == L"lr" && array->column_rules == std::vector<unsigned char>{1, 2, 1} &&
        array->row_rules == std::vector<unsigned char>{1, 2, 1} && array->row_gaps.size() == 2 &&
        array->row_gaps[0].unit == MathLengthUnit::Em && array->row_gaps[0].value == 1 && RangesValid(parsed.root, ruled.size()),
        "array AST separates alignments vertical rules horizontal boundaries and row gaps");
    const auto middle = ParseMathSyntax(LR"(\left(x\middle|y\middle\|z\right))");
    const auto* fence = Find(middle.root, MathNodeKind::Fence);
    size_t middle_count = 0;
    if (fence && !fence->children.empty())
        for (const auto& child : fence->children[0].children) middle_count += child.kind == MathNodeKind::Middle;
    Check(middle && middle_count == 2, "multiple middle delimiters remain scoped children of one fence");
    const auto brace = ParseMathSyntax(LR"(\overbrace{x+y}^{n}_{i})");
    const auto* brace_node = Find(brace.root, MathNodeKind::Brace);
    const auto* scripts = Find(brace.root, MathNodeKind::Script);
    Check(brace && brace_node && brace_node->text == L"overbrace" && brace_node->children.size() == 1 &&
        scripts && scripts->has_superscript && scripts->has_subscript, "brace keeps body and both script roles available to layout");
    const auto phantom = ParseMathSyntax(LR"(\hphantom{\frac{a}{b}})");
    const auto* phantom_node = Find(phantom.root, MathNodeKind::Phantom);
    Check(phantom && phantom_node && phantom_node->text == L"hphantom" &&
        Find(*phantom_node, MathNodeKind::Fraction), "phantom keeps hidden nested syntax for dimension layout");
    const auto negative_space = ParseMathSyntax(LR"(a\hspace{-1.25pt}b)");
    const auto* length = Find(negative_space.root, MathNodeKind::Space);
    Check(negative_space && length && length->text == L"hspace" && length->length.unit == MathLengthUnit::Pt &&
        length->length.value == -1.25f, "explicit length preserves sign value and unit without font work");
    for (const auto& valid : {LR"(\begin{array}{|c|}x\end{array})", LR"(\left(x\middle|\left[y\middle|z\right]\right))",
        LR"(\hspace{20em}x)", LR"(\hspace{40ex}x)", LR"(\hspace{200pt}x)", LR"(\hspace{-20em}x)",
        LR"(\begin{array}{c}x\\[0pt]y\end{array})", LR"(\phantom{})"}) {
        const auto result = ParseMathSyntax(valid);
        Check(result && RangesValid(result.root, std::wstring_view(valid).size()), "new structures and explicit dimension boundaries parse");
    }
    for (const auto& invalid : {LR"(\begin{array}{|||c}x\end{array})", LR"(\begin{array}{c}x\hline\end{array})",
        LR"(\begin{array}{c}\hline\hline\hline x\end{array})", LR"(\middle|x)", LR"(\left({x\middle|y}\right))",
        LR"(\left(x\middle|^2y\right))", LR"(\hspace{20.01em}x)", LR"(\hspace{40.01ex}x)", LR"(\hspace{200.01pt}x)",
        LR"(\hspace{NaNem}x)", LR"(\hspace{infpt}x)", LR"(\hspace{1e2em}x)", LR"(\hspace{1emjunk}x)",
        LR"(\hspace{}x)", LR"(\begin{array}{c}x\\[-1em]y\end{array})"}) {
        const auto result = ParseMathSyntax(invalid);
        Check(!result && result.error.offset <= std::wstring_view(invalid).size(), "malformed complex structure returns bounded source diagnostic");
    }
}
bool SameSyntax(const MathNode& a, const MathNode& b) {
    if (a.kind != b.kind || a.atom_class != b.atom_class || a.text != b.text || a.auxiliary != b.auxiliary ||
        a.style != b.style || a.limits != b.limits || a.has_superscript != b.has_superscript ||
        a.has_subscript != b.has_subscript || a.children.size() != b.children.size()) return false;
    for (size_t i = 0; i < a.children.size(); ++i) if (!SameSyntax(a.children[i], b.children[i])) return false;
    return true;
}
void TestMacroSyntax() {
    const auto equivalent = [](std::wstring_view macro, std::wstring_view expanded) {
        const auto a = ParseMathSyntax(macro), b = ParseMathSyntax(expanded);
        return a && b && SameSyntax(a.root, b.root) && RangesValid(a.root, macro.size());
    };
    Check(equivalent(LR"(\newcommand{\sq}[1]{#1^2}\sq{x})", L"x^2"), "required macro substitution matches literal AST");
    Check(equivalent(LR"(\newcommand{\sq}[1]{#1^2}\sq𝒜)", L"𝒜^2") &&
        equivalent(LR"(\newcommand{\twice}[1]{#1+#1}\twice𝔄)", L"𝔄+𝔄"),
        "unbraced supplementary macro argument stays one scalar and repeated origins remain bounded");
    Check(equivalent(LR"(\newcommand{\sq}[1]{#1^2}\sq xyz)", L"x^2yz"),
        "unbraced BMP argument consumes one token rather than the following word");
    Check(equivalent(LR"(\newcommand{\pow}[2][2]{#2^{#1}}\pow{x}+\pow[3]{y})", L"x^{2}+y^{3}"),
        "optional macro default and override match literal AST without external reference assumptions");
    Check(equivalent(LR"(\newcommand{\f}[2][]{#1+#2}\f{x}+\f[]{y})", L"+x++y"),
        "empty default and explicit empty optional argument substitute correctly");
    Check(equivalent(LR"(\newcommand{\f}[9]{#1+#9}\f{a}{b}{c}{d}{e}{f}{g}{h}{i})", L"a+i"),
        "nine required arguments substitute their correct positions");
    Check(equivalent(LR"(\newcommand{\f}{x}{\renewcommand{\f}{y}\f}+\f)", L"{y}+x"),
        "group macro scope restores outer definition");
    Check(equivalent(LR"(\newcommand{\f}{a}\begin{matrix}\renewcommand{\f}{b}\f&1\\0&\f\end{matrix}+\f)",
        LR"(\begin{matrix}b&1\\0&a\end{matrix}+a)"),
        "matrix column separator restores macro scope before later cells and outside environment");
    Check(equivalent(LR"(\newcommand{\f}{a}\begin{matrix}\renewcommand{\f}{b}\f\\\f\end{matrix}+\f)",
        LR"(\begin{matrix}b\\a\end{matrix}+a)"), "matrix row separator restores cell-local macro definition");
    Check(equivalent(LR"(\newcommand{\f}{a}\begin{matrix}{\renewcommand{\f}{b}\f}+\f&\f\end{matrix})",
        LR"(\begin{matrix}{b}+a&a\end{matrix})"), "inner group restoration remains independent of surrounding cell scope");
    Check(equivalent(LR"(\newcommand{\f}{a}\begin{matrix}\renewcommand{\f}{b}\begin{matrix}\renewcommand{\f}{c}\f&\f\end{matrix}+\f&\f\end{matrix}+\f)",
        LR"(\begin{matrix}\begin{matrix}c&b\end{matrix}+b&a\end{matrix}+a)"),
        "nested matrix cells restore their inherited outer-cell macro definition");
    Check(equivalent(LR"(\newcommand{\f}{a}\substack{\renewcommand{\f}{b}\f\\\f}+\f)", LR"(\substack{b\\a}+a)"),
        "substack row renewal restores inherited macro for next row and outer expression");
    const std::wstring substack_local = LR"(\substack{\newcommand{\localrow}{b}\localrow\\\localrow})";
    const auto row_error = ParseMathSyntax(substack_local);
    Check(!row_error && row_error.error.code == MathParseErrorCode::UnknownCommand && row_error.error.offset <= substack_local.size(),
        "macro introduced in one substack row cannot be called from the next row");
    Check(equivalent(LR"(\newcommand{\f}[1]{\g{#1}}\newcommand{\g}[1]{#1^2}\f{x})", L"x^2"),
        "nested macro calls expand before syntax construction");
    Check(equivalent(LR"(\providecommand{\frac}[2]{#1+#2}\frac{a}{b})", LR"(\frac{a}{b})"),
        "providecommand leaves builtin definition intact");
    Check(equivalent(LR"(\newcommand{\word}{abc}\text{\word xyz})", LR"(\text{abcxyz})") &&
        equivalent(LR"(\newcommand{\word}{abc}\text{a b \word xyz})", LR"(\text{a b abcxyz})"),
        "macro control word consumes following whitespace while preserving ordinary text spaces");
    const std::wstring origin = LR"(\newcommand{\f}[1]{#1+y}\f{x})";
    const auto mapped = ParseMathSyntax(origin);
    bool argument_origin = false, body_origin = false;
    if (mapped) for (const auto& node : mapped.root.children) {
        if (node.kind != MathNodeKind::Glyph) continue;
        if (node.text == L"x") argument_origin = node.source_start == origin.rfind(L'x');
        if (node.text == L"y") body_origin = node.source_start == origin.rfind(L"\\f") &&
            node.source_length == origin.size() - origin.rfind(L"\\f");
    }
    Check(mapped && argument_origin && body_origin && RangesValid(mapped.root, origin.size()),
        "macro arguments keep their origin and generated body maps to invocation span");
    const std::wstring bad_body = LR"(\newcommand{\f}{\notacommand}\f)";
    const auto error = ParseMathSyntax(bad_body);
    Check(!error && error.error.code == MathParseErrorCode::UnknownCommand && error.error.offset == bad_body.rfind(L"\\f"),
        "expanded unknown command diagnostic maps to original invocation");
    const auto defined = ParseMathSyntax(LR"(\newcommand{\f}{x}\f)");
    const auto separate = ParseMathSyntax(LR"(\f)");
    Check(defined && !separate, "macro definition cannot leak into a subsequent formula");
    std::wstring definitions;
    for (int i = 0; i < 65; ++i) {
        const std::wstring name{L'm', static_cast<wchar_t>(L'a' + i / 26), static_cast<wchar_t>(L'a' + i % 26)};
        definitions += L"\\newcommand{\\zz" + name + L"}{x}";
    }
    ExpectError(definitions + L"x", MathParseErrorCode::NodeLimit, "macro definition budget is enforced");
    std::wstring calls = LR"(\newcommand{\f}{x})";
    for (int i = 0; i < 257; ++i) calls += LR"(\f)";
    ExpectError(calls, MathParseErrorCode::NodeLimit, "macro invocation budget is enforced");
    std::wstring expansion = LR"(\newcommand{\f}{)" + std::wstring(200, L'x') + L"}";
    for (int i = 0; i < 21; ++i) expansion += LR"(\f)";
    ExpectError(expansion, MathParseErrorCode::SourceLimit, "expanded source budget is independent of small raw source");
    ExpectError(LR"(\newcommand{\f}{\f}\f)", MathParseErrorCode::DepthLimit, "recursive macro expansion stops at bounded depth");
    for (const auto& invalid : {LR"(\newcommand{\frac}[2]{#1+#2}\frac{a}{b})", LR"(\renewcommand{\missing}{x}\missing)",
        LR"(\newcommand{\f}[1]{#0}\f{x})", LR"(\newcommand{\f}[1]{#2}\f{x})", LR"(\newcommand{\f}{##}\f)",
        LR"(\newcommand{\f}[10]{x}\f)", LR"({\newcommand{\f}{x}\f}\f)", LR"(\def\f{x}\f)", LR"(\input{file})"}) {
        const auto result = ParseMathSyntax(invalid);
        Check(!result && result.error.offset <= std::wstring_view(invalid).size(), "unsupported or invalid macro returns bounded original-source diagnostic");
    }
}
void TestAlphabetMapping() {
    using namespace pulse::ui::math;
    struct Case { const wchar_t* input; MathAlphabet family; const wchar_t* expected; bool bold = false; };
    const Case cases[] = {{L"AB", MathAlphabet::Script, L"𝒜ℬ"}, {L"AC", MathAlphabet::Fraktur, L"𝔄ℭ"},
        {L"A0", MathAlphabet::SansSerif, L"𝖠𝟢"}, {L"A0", MathAlphabet::Monospace, L"𝙰𝟶"},
        {L"Aα0", MathAlphabet::BoldItalic, L"𝑨𝜶𝟎"}, {L"A", MathAlphabet::Script, L"𝓐", true},
        {L"A", MathAlphabet::Fraktur, L"𝕬", true}, {L"A", MathAlphabet::SansSerif, L"𝗔", true},
        {L"12+α", MathAlphabet::Script, L"12+α"}, {L"12+α", MathAlphabet::Fraktur, L"12+α"}};
    for (const auto& item : cases) {
        std::wstring output;
        Check(MapMathAlphabet(item.input, item.family, output, item.bold) && output == item.expected,
            "mathematical alphabet maps exact Unicode scalars including Letterlike holes and preserved symbols");
    }
    for (const auto& invalid : {std::wstring(1, static_cast<wchar_t>(0xd835)), std::wstring(1, static_cast<wchar_t>(0xdd38)),
        std::wstring{static_cast<wchar_t>(0xd835), L'A'}}) {
        std::wstring output = L"unchanged";
        Check(!MapMathAlphabet(invalid, MathAlphabet::Script, output) && output == L"unchanged",
            "malformed UTF-16 alphabet mapping leaves output unchanged");
    }
}
}
int main() {
    TestFontConstants();
    TestStructureSyntax();
    TestMacroSyntax();
    TestAlphabetMapping();
    const float binary_space = MathAtomSpacing(MathAtomClass::Ord, MathAtomClass::Bin, MathStyle::Text);
    const float relation_space = MathAtomSpacing(MathAtomClass::Ord, MathAtomClass::Rel, MathStyle::Text);
    Check(binary_space > 0 && relation_space > binary_space &&
        MathAtomSpacing(MathAtomClass::Ord, MathAtomClass::Ord, MathStyle::Text) == 0,
        "TeX binary and relation spaces exceed ordinary adjacency");
    Check(std::abs(MathAtomSpacing(MathAtomClass::Punct, MathAtomClass::Rel, MathStyle::Text) - 5.0f / 18.0f) < 0.00001f &&
        MathAtomSpacing(MathAtomClass::Punct, MathAtomClass::Rel, MathStyle::Script) == 0,
        "punctuation before relation uses five mu in text and no glue in script");
    Check(MathAtomSpacing(MathAtomClass::Ord, MathAtomClass::Bin, MathStyle::Script) == 0 &&
        MathAtomSpacing(MathAtomClass::Ord, MathAtomClass::Rel, MathStyle::ScriptScript) == 0 &&
        MathAtomSpacing(MathAtomClass::Ord, MathAtomClass::Op, MathStyle::Script) > 0,
        "script styles suppress binary relation glue while retaining operator thin space");
    for (const auto& preceding : {MathAtomClass::Open, MathAtomClass::Rel, MathAtomClass::Bin, MathAtomClass::Punct}) {
        std::vector<MathAtomClass> sequence{MathAtomClass::Ord, preceding, MathAtomClass::Bin, MathAtomClass::Ord};
        NormalizeMathAtomClasses(sequence);
        Check(sequence[2] == MathAtomClass::Ord, "binary after open relation binary or punctuation is normalized to unary");
    }
    std::vector<MathAtomClass> leading{MathAtomClass::Bin, MathAtomClass::Ord};
    std::vector<MathAtomClass> trailing{MathAtomClass::Ord, MathAtomClass::Bin, MathAtomClass::Close};
    NormalizeMathAtomClasses(leading);
    NormalizeMathAtomClasses(trailing);
    Check(leading[0] == MathAtomClass::Ord && trailing[1] == MathAtomClass::Ord,
        "leading binary and binary before closing delimiter become ordinary");
    const std::wstring source = LR"(a+\frac{x_i^2}{\sqrt[3]{y}})";
    const auto parsed = ParseMathSyntax(source);
    Check(parsed && parsed.root.kind == MathNodeKind::Row && parsed.root.source_start == 0 &&
        parsed.root.source_length == source.size() && RangesValid(parsed.root, source.size()),
        "root and every child retain bounded original source offsets");
    const auto* fraction = Find(parsed.root, MathNodeKind::Fraction);
    Check(parsed && fraction && fraction->children.size() == 2 && fraction->source_start == 2 &&
        source.substr(fraction->source_start, fraction->source_length) == LR"(\frac{x_i^2}{\sqrt[3]{y}})",
        "fraction source range and numerator denominator structure are preserved");
    const auto* script = Find(parsed.root, MathNodeKind::Script);
    Check(parsed && script && script->children.size() == 3 && script->has_superscript && script->has_subscript,
        "script AST separates base superscript and subscript");
    const auto* radical = Find(parsed.root, MathNodeKind::Radical);
    Check(parsed && radical && radical->children.size() == 2 && !radical->children[1].children.empty(),
        "indexed radical keeps body and index as separate syntax");
    const auto classified = ParseMathSyntax(L"a+b=c,(d)");
    const std::vector<MathAtomClass> classes{MathAtomClass::Ord, MathAtomClass::Bin, MathAtomClass::Ord,
        MathAtomClass::Rel, MathAtomClass::Ord, MathAtomClass::Punct, MathAtomClass::Open, MathAtomClass::Ord, MathAtomClass::Close};
    bool classes_ok = classified && classified.root.children.size() == classes.size();
    if (classes_ok) for (size_t i = 0; i < classes.size(); ++i)
        classes_ok = classes_ok && classified.root.children[i].atom_class == classes[i];
    Check(classes_ok, "ordinary binary relation punctuation and fence atoms are classified");
    const auto env = ParseMathSyntax(LR"(\begin{array}{lcr}a&bb&ccc\\dddd&e&f\end{array})");
    const auto* environment = Find(env.root, MathNodeKind::Environment);
    Check(env && environment && environment->children.size() == 2 && environment->children[0].children.size() == 3 &&
        environment->children[1].children.size() == 3 && environment->auxiliary == L"lcr",
        "array AST preserves alignment and separate rows and cells");
    const auto arrow = ParseMathSyntax(LR"(\xrightarrow[below]{above})");
    const auto* annotated = Find(arrow.root, MathNodeKind::Arrow);
    Check(arrow && annotated && annotated->children.size() == 2 && !annotated->children[0].children.empty() &&
        !annotated->children[1].children.empty(), "arrow keeps above and below annotations independently");
    const auto prime = ParseMathSyntax(L"x_i'^2");
    const auto* prime_script = Find(prime.root, MathNodeKind::Script);
    Check(prime && prime_script && prime_script->has_superscript && prime_script->has_subscript,
        "prime and explicit exponent retain a single superscript node");
    const std::wstring surrogate{static_cast<wchar_t>(0xd835), static_cast<wchar_t>(0xdd38)};
    const auto unicode = ParseMathSyntax(surrogate + L"+x");
    Check(unicode && unicode.root.children.size() == 3 && unicode.root.children[0].source_length == 2 &&
        unicode.root.children[1].source_start == 2 && RangesValid(unicode.root, 4),
        "supplementary glyph offsets use UTF-16 code units");
    const std::wstring unknown = LR"(a+\notacommand{x})";
    const auto error = ParseMathSyntax(unknown);
    Check(!error && error.error.code == MathParseErrorCode::UnknownCommand && error.error.offset == 2,
        "unknown command diagnoses its original backslash offset");
    ExpectError(L"", MathParseErrorCode::EmptyInput, "empty input diagnostic");
    ExpectError(std::wstring(4097, L'x'), MathParseErrorCode::SourceLimit, "source budget fails before parsing");
    ExpectError(std::wstring(40, L'{') + L"x" + std::wstring(40, L'}'), MathParseErrorCode::DepthLimit,
        "deeply nested groups stop at depth budget");
    ExpectError(std::wstring(1500, L'x'), MathParseErrorCode::NodeLimit, "flat expression stops at node budget");
    ExpectError(L"x^2^3", MathParseErrorCode::DuplicateScript, "duplicate explicit superscript diagnostic");
    ExpectError(LR"(\begin{matrix}x\end{pmatrix})", MathParseErrorCode::MismatchedEnvironment,
        "mismatched environment diagnostic");
    std::wstring cells = LR"(\begin{array}{lllll})";
    for (int i = 0; i < 26; ++i) cells += i ? LR"(\\a&a&a&a&a)" : L"a&a&a&a&a";
    cells += LR"(\end{array})";
    ExpectError(cells, MathParseErrorCode::CellLimit, "environment total cell budget stops nested layout growth");
    for (const auto& malformed : {LR"(\frac{a})", LR"(\left(x)", LR"(\begin{array}{|||c}x\end{array})",
        LR"(\xrightarrow[a])", LR"(\substack{a&b})", LR"(x'_i^2)"}) {
        const auto result = ParseMathSyntax(malformed);
        Check(!result && result.error.offset <= std::wstring_view(malformed).size(),
            "malformed formula returns a bounded diagnostic without font work");
    }
    for (const auto& valid : {LR"(\boxed{})", LR"(\xrightarrow[]{})", LR"(\operatornamewithlimits{custom}_{x})",
        LR"(\begin{Bmatrix}a&b\\c&d\end{Bmatrix})", LR"(\sum_{\substack{i=1\\j=2}}^{n}a_{ij})"}) {
        const auto result = ParseMathSyntax(valid);
        Check(result && RangesValid(result.root, std::wstring_view(valid).size()),
            "migrated rich syntax parses without DirectWrite or a graphics device");
    }
    std::printf("Math syntax tests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
