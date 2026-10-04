#include "math_syntax.h"
#include "math_macro.h"

#include <algorithm>
#include <cwctype>
#include <utility>

namespace pulse::ui {
namespace {
using Kind = MathNodeKind;
using Class = MathAtomClass;
using Error = MathParseErrorCode;
struct Symbol {
    std::wstring_view name, value;
    Class atom_class = Class::Ord;
};
constexpr Symbol delimiters[] = {{L"lparen", L"(", Class::Open},
                                 {L"rparen", L")", Class::Close},
                                 {L"lbrack", L"[", Class::Open},
                                 {L"rbrack", L"]", Class::Close},
                                 {L"{", L"{", Class::Open},
                                 {L"}", L"}", Class::Close},
                                 {L"lbrace", L"{", Class::Open},
                                 {L"rbrace", L"}", Class::Close},
                                 {L"langle", L"⟨", Class::Open},
                                 {L"rangle", L"⟩", Class::Close},
                                 {L"vert", L"|"},
                                 {L"lvert", L"|", Class::Open},
                                 {L"rvert", L"|", Class::Close},
                                 {L"Vert", L"‖"},
                                 {L"lVert", L"‖", Class::Open},
                                 {L"rVert", L"‖", Class::Close},
                                 {L"|", L"‖"},
                                 {L"lfloor", L"⌊", Class::Open},
                                 {L"rfloor", L"⌋", Class::Close},
                                 {L"lceil", L"⌈", Class::Open},
                                 {L"rceil", L"⌉", Class::Close},
                                 {L"backslash", L"\\"}};
constexpr Symbol symbols[] = {{L"alpha", L"α"},
                              {L"beta", L"β"},
                              {L"gamma", L"γ"},
                              {L"delta", L"δ"},
                              {L"epsilon", L"ϵ"},
                              {L"varepsilon", L"ε"},
                              {L"zeta", L"ζ"},
                              {L"eta", L"η"},
                              {L"theta", L"θ"},
                              {L"vartheta", L"ϑ"},
                              {L"iota", L"ι"},
                              {L"kappa", L"κ"},
                              {L"lambda", L"λ"},
                              {L"mu", L"μ"},
                              {L"nu", L"ν"},
                              {L"xi", L"ξ"},
                              {L"pi", L"π"},
                              {L"varpi", L"ϖ"},
                              {L"rho", L"ρ"},
                              {L"varrho", L"ϱ"},
                              {L"sigma", L"σ"},
                              {L"varsigma", L"ς"},
                              {L"tau", L"τ"},
                              {L"upsilon", L"υ"},
                              {L"phi", L"ϕ"},
                              {L"varphi", L"φ"},
                              {L"chi", L"χ"},
                              {L"psi", L"ψ"},
                              {L"omega", L"ω"},
                              {L"Gamma", L"Γ"},
                              {L"Delta", L"Δ"},
                              {L"Theta", L"Θ"},
                              {L"Lambda", L"Λ"},
                              {L"Xi", L"Ξ"},
                              {L"Pi", L"Π"},
                              {L"Sigma", L"Σ"},
                              {L"Upsilon", L"Υ"},
                              {L"Phi", L"Φ"},
                              {L"Psi", L"Ψ"},
                              {L"Omega", L"Ω"},
                              {L"times", L"×", Class::Bin},
                              {L"cdot", L"⋅", Class::Bin},
                              {L"div", L"÷", Class::Bin},
                              {L"pm", L"±", Class::Bin},
                              {L"mp", L"∓", Class::Bin},
                              {L"le", L"≤", Class::Rel},
                              {L"leq", L"≤", Class::Rel},
                              {L"ge", L"≥", Class::Rel},
                              {L"geq", L"≥", Class::Rel},
                              {L"ne", L"≠", Class::Rel},
                              {L"neq", L"≠", Class::Rel},
                              {L"approx", L"≈", Class::Rel},
                              {L"equiv", L"≡", Class::Rel},
                              {L"sim", L"∼", Class::Rel},
                              {L"simeq", L"≃", Class::Rel},
                              {L"propto", L"∝", Class::Rel},
                              {L"ll", L"≪", Class::Rel},
                              {L"gg", L"≫", Class::Rel},
                              {L"in", L"∈", Class::Rel},
                              {L"notin", L"∉", Class::Rel},
                              {L"ni", L"∋", Class::Rel},
                              {L"subset", L"⊂", Class::Rel},
                              {L"supset", L"⊃", Class::Rel},
                              {L"subseteq", L"⊆", Class::Rel},
                              {L"supseteq", L"⊇", Class::Rel},
                              {L"cup", L"∪", Class::Bin},
                              {L"cap", L"∩", Class::Bin},
                              {L"land", L"∧", Class::Bin},
                              {L"wedge", L"∧", Class::Bin},
                              {L"lor", L"∨", Class::Bin},
                              {L"vee", L"∨", Class::Bin},
                              {L"neg", L"¬"},
                              {L"forall", L"∀"},
                              {L"exists", L"∃"},
                              {L"emptyset", L"∅"},
                              {L"infty", L"∞"},
                              {L"partial", L"∂"},
                              {L"nabla", L"∇"},
                              {L"ell", L"ℓ"},
                              {L"hbar", L"ℏ"},
                              {L"prime", L"′"},
                              {L"to", L"→", Class::Rel},
                              {L"rightarrow", L"→", Class::Rel},
                              {L"leftarrow", L"←", Class::Rel},
                              {L"leftrightarrow", L"↔", Class::Rel},
                              {L"Rightarrow", L"⇒", Class::Rel},
                              {L"Leftarrow", L"⇐", Class::Rel},
                              {L"Leftrightarrow", L"⇔", Class::Rel},
                              {L"mapsto", L"↦", Class::Rel},
                              {L"uparrow", L"↑", Class::Rel},
                              {L"downarrow", L"↓", Class::Rel},
                              {L"longrightarrow", L"⟶", Class::Rel},
                              {L"longleftarrow", L"⟵", Class::Rel},
                              {L"longleftrightarrow", L"⟷", Class::Rel},
                              {L"dots", L"…", Class::Inner},
                              {L"ldots", L"…", Class::Inner},
                              {L"cdots", L"⋯", Class::Inner},
                              {L"vdots", L"⋮", Class::Inner},
                              {L"ddots", L"⋱", Class::Inner},
                              {L"angle", L"∠"},
                              {L"perp", L"⊥", Class::Rel},
                              {L"parallel", L"∥", Class::Rel},
                              {L"mid", L"∣", Class::Rel},
                              {L"circ", L"∘", Class::Bin},
                              {L"sum", L"∑", Class::Op},
                              {L"prod", L"∏", Class::Op},
                              {L"int", L"∫", Class::Op},
                              {L"iint", L"∬", Class::Op},
                              {L"oint", L"∮", Class::Op},
                              {L"bigcup", L"⋃", Class::Op},
                              {L"bigcap", L"⋂", Class::Op}};

Class LiteralClass(wchar_t ch) {
    if (std::wstring_view(L"+-*×÷±∓⋅∪∩∧∨∘").find(ch) != std::wstring_view::npos)
        return Class::Bin;
    if (std::wstring_view(L"=<>:≤≥≠≈≡∼∈∉⊂⊃⊆⊇→←↔⇒⇐⇔↦⊥∥").find(ch) != std::wstring_view::npos)
        return Class::Rel;
    if (std::wstring_view(L"([⟨⌊⌈").find(ch) != std::wstring_view::npos)
        return Class::Open;
    if (std::wstring_view(L")]⟩⌋⌉").find(ch) != std::wstring_view::npos)
        return Class::Close;
    if (ch == L',' || ch == L';')
        return Class::Punct;
    return Class::Ord;
}
bool In(std::wstring_view value, std::initializer_list<std::wstring_view> values) {
    return std::find(values.begin(), values.end(), value) != values.end();
}
bool BuiltinCommand(std::wstring_view command) {
    for (const auto& symbol : symbols)
        if (symbol.name == command)
            return true;
    for (const auto& delimiter : delimiters)
        if (delimiter.name == command)
            return true;
    auto sized = command;
    if (!sized.empty() && std::wstring_view(L"lrm").find(sized.back()) != std::wstring_view::npos)
        sized.remove_suffix(1);
    if (In(sized, {L"big", L"Big", L"bigg", L"Bigg"}))
        return true;
    return In(command, {
        L"displaystyle", L"textstyle", L"scriptstyle", L"scriptscriptstyle", L"frac", L"dfrac", L"tfrac",
        L"binom", L"dbinom", L"tbinom", L"sqrt", L"left", L"middle", L"right", L"begin", L"end", L"substack",
        L"hspace", L"hline", L"phantom", L"hphantom", L"vphantom", L"overbrace", L"underbrace", L"boxed",
        L"cancel", L"bcancel", L"xcancel", L"xrightarrow", L"xleftarrow", L"bmod", L"pmod", L"overset",
        L"underset", L"mathbb", L"text", L"textrm", L"textbf", L"operatorname", L"operatornamewithlimits",
        L"mathrm", L"mathbf", L"mathit", L"mathcal", L"mathscr", L"mathfrak", L"mathsf", L"mathtt",
        L"boldsymbol", L"bm", L"overline", L"bar", L"underline", L"hat", L"widehat", L"vec", L"dot",
        L"ddot", L"tilde", L"widetilde", L"acute", L"grave", L"breve", L"check", L"mathring",
        L"overrightarrow", L"overleftarrow", L"overleftrightarrow", L"underrightarrow", L"underleftarrow",
        L"underleftrightarrow", L"quad", L"qquad", L"enspace", L"sin", L"cos", L"tan", L"cot", L"sec", L"csc",
        L"arcsin", L"arccos", L"arctan", L"sinh", L"cosh", L"tanh", L"log", L"ln", L"exp", L"lim", L"min",
        L"max", L"det", L"gcd", L"arg", L"deg", L"dim", L"hom", L"ker", L"coth", L"lg", L"inf", L"sup",
        L"liminf", L"limsup", L"Pr", L"argmax", L"argmin", L"limits", L"nolimits", L"newcommand",
        L"renewcommand", L"providecommand", L"def", L"gdef", L"edef", L"xdef", L"let", L"futurelet", L"catcode",
        L"global", L"input", L"include", L"newenvironment", L"renewenvironment"
    });
}
class SyntaxParser {
public:
    explicit SyntaxParser(std::wstring_view source) : source_(source) {}
    MathParseResult Parse() {
        if (source_.size() > 4096)
            Fail(Error::SourceLimit, 4096);
        if (source_.empty())
            Fail(Error::EmptyInput, 0);
        MathNode root = Row(0, 0);
        Space();
        if (pos_ != source_.size())
            Fail(Error::UnexpectedToken, pos_);
        if (root.children.empty())
            Fail(Error::EmptyInput, 0);
        size_t total_nodes = 0;
        CheckStructure(root, total_nodes);
        return {std::move(root), error_};
    }

private:
    std::wstring_view source_;
    size_t pos_ = 0, source_atoms_ = 0, structural_nodes_ = 0, cells_ = 0;
    MathParseError error_;
    void CheckStructure(const MathNode& node, size_t& total) {
        if (!Good())
            return;
        if (++total > 4096) {
            Fail(Error::NodeLimit, node.source_start);
            return;
        }
        for (const auto& child : node.children)
            CheckStructure(child, total);
    }
    bool Good() const { return error_.code == Error::None; }
    void Fail(Error code, size_t offset) {
        if (Good())
            error_ = {code, std::min(offset, source_.size())};
    }
    void Space() {
        while (pos_ < source_.size() && iswspace(source_[pos_]))
            ++pos_;
    }
    bool Starts(std::wstring_view value) const { return source_.substr(pos_, value.size()) == value; }
    bool Ahead(std::wstring_view value) const {
        return Starts(value) && (pos_ + value.size() == source_.size() || !iswalpha(source_[pos_ + value.size()]));
    }
    MathNode Node(Kind kind, size_t start, int depth) {
        if (++structural_nodes_ > 4096)
            Fail(Error::NodeLimit, start);
        if (depth > 32)
            Fail(Error::DepthLimit, start);
        MathNode node;
        node.kind = kind;
        node.source_start = start;
        return node;
    }
    MathNode Finish(MathNode node) {
        node.source_length = pos_ - node.source_start;
        return node;
    }
    MathNode Empty(size_t at) {
        MathNode node;
        node.source_start = at;
        return node;
    }
    std::wstring_view Command() {
        const size_t slash = pos_;
        if (pos_ == source_.size() || source_[pos_++] != L'\\') {
            Fail(Error::UnexpectedToken, slash);
            return {};
        }
        const size_t start = pos_;
        if (pos_ < source_.size() && iswalpha(source_[pos_]))
            while (pos_ < source_.size() && iswalpha(source_[pos_]))
                ++pos_;
        else if (pos_ < source_.size())
            ++pos_;
        else
            Fail(Error::UnexpectedEnd, pos_);
        return source_.substr(start, pos_ - start);
    }
    std::wstring RawGroup() {
        Space();
        if (pos_ == source_.size()) {
            Fail(Error::UnexpectedEnd, pos_);
            return {};
        }
        if (source_[pos_] != L'{') {
            Fail(Error::InvalidArgument, pos_);
            return {};
        }
        const size_t start = ++pos_;
        while (pos_ < source_.size() && source_[pos_] != L'}') {
            if (source_[pos_] == L'{' || source_[pos_] == L'\\') {
                Fail(Error::InvalidArgument, pos_);
                return {};
            }
            ++pos_;
        }
        if (pos_ == source_.size()) {
            Fail(Error::UnexpectedEnd, pos_);
            return {};
        }
        std::wstring value(source_.substr(start, pos_ - start));
        ++pos_;
        return value;
    }
    MathNode Group(int depth, bool required = false) {
        Space();
        if (!Good())
            return Empty(pos_);
        if (pos_ == source_.size()) {
            Fail(Error::UnexpectedEnd, pos_);
            return Empty(pos_);
        }
        if (source_[pos_] != L'{') {
            if (required) {
                Fail(Error::InvalidArgument, pos_);
                return Empty(pos_);
            }
            return Atom(depth + 1);
        }
        const size_t start = pos_++;
        auto node = Row(depth + 1, L'}');
        if (pos_ == source_.size())
            Fail(Error::UnexpectedEnd, pos_);
        else if (source_[pos_] != L'}')
            Fail(Error::UnexpectedToken, pos_);
        else
            ++pos_;
        node.source_start = start;
        return Finish(std::move(node));
    }
    MathLength Length(wchar_t open, wchar_t close, bool nonnegative) {
        Space();
        const size_t start = pos_;
        MathLength result;
        if (pos_ == source_.size() || source_[pos_] != open) {
            Fail(Error::InvalidArgument, pos_);
            return result;
        }
        ++pos_;
        Space();
        bool negative = false;
        if (pos_ < source_.size() && (source_[pos_] == L'+' || source_[pos_] == L'-'))
            negative = source_[pos_++] == L'-';
        double value = 0, fraction = .1;
        bool digit = false, decimal = false;
        while (pos_ < source_.size()) {
            const wchar_t ch = source_[pos_];
            if (ch == L'.' && !decimal) {
                decimal = true;
                ++pos_;
            } else if (ch >= L'0' && ch <= L'9') {
                digit = true;
                if (decimal) {
                    value += static_cast<double>(ch - L'0') * fraction;
                    fraction *= .1;
                } else
                    value = value * 10 + static_cast<double>(ch - L'0');
                ++pos_;
                if (value > 200) {
                    Fail(Error::InvalidArgument, start);
                    return result;
                }
            } else
                break;
        }
        Space();
        const size_t unit_start = pos_;
        while (pos_ < source_.size() && iswalpha(source_[pos_]))
            ++pos_;
        const auto unit = source_.substr(unit_start, pos_ - unit_start);
        double maximum = 0;
        if (unit == L"em") {
            result.unit = MathLengthUnit::Em;
            maximum = 20;
        } else if (unit == L"ex") {
            result.unit = MathLengthUnit::Ex;
            maximum = 40;
        } else if (unit == L"pt") {
            result.unit = MathLengthUnit::Pt;
            maximum = 200;
        }
        Space();
        if (!digit || maximum == 0 || value > maximum || (nonnegative && negative && value > 0) ||
            pos_ == source_.size() || source_[pos_] != close) {
            Fail(Error::InvalidArgument, start);
            return result;
        }
        ++pos_;
        result.value = static_cast<float>(negative ? -value : value);
        return result;
    }
    MathNode Row(int depth, wchar_t end, bool environment = false, bool paired = false) {
        auto row = Node(Kind::Row, pos_, depth);
        while (Good()) {
            Space();
            if (pos_ == source_.size() || (end && source_[pos_] == end) ||
                (environment && (source_[pos_] == L'&' || Starts(L"\\\\") || Ahead(L"\\end"))) ||
                (paired && Ahead(L"\\right")))
                break;
            if (paired && Ahead(L"\\middle")) {
                const size_t start = pos_;
                Command();
                if (++source_atoms_ > 1024)
                    Fail(Error::NodeLimit, start);
                auto middle = Node(Kind::Middle, start, depth + 1);
                middle.atom_class = Class::Rel;
                middle.text = Delimiter();
                row.children.push_back(Finish(std::move(middle)));
                continue;
            }
            auto base = Atom(depth + 1);
            if (!Good())
                break;
            if (base.kind == Kind::Space || (base.kind == Kind::Style && base.children.empty())) {
                row.children.push_back(std::move(base));
                continue;
            }
            Space();
            MathLimits limits = MathLimits::Default;
            if (Ahead(L"\\limits") || Ahead(L"\\nolimits")) {
                const size_t modifier = pos_;
                const auto command = Command();
                if (base.kind != Kind::Operator)
                    Fail(Error::InvalidArgument, modifier);
                limits = command == L"limits" ? MathLimits::AboveBelow : MathLimits::Side;
                Space();
            }
            MathNode sup = Empty(pos_), sub = Empty(pos_);
            bool has_sup = false, has_sub = false;
            while (Good() && pos_ < source_.size() &&
                   (source_[pos_] == L'^' || source_[pos_] == L'_' || source_[pos_] == L'\'')) {
                const size_t marker = pos_;
                if (source_[pos_] == L'\'') {
                    if (has_sup) {
                        Fail(Error::DuplicateScript, marker);
                        break;
                    }
                    sup = Node(Kind::Row, marker, depth + 1);
                    auto primes = Node(Kind::Glyph, marker, depth + 2);
                    primes.upright = true;
                    while (pos_ < source_.size() && source_[pos_] == L'\'') {
                        primes.text.push_back(L'′');
                        ++pos_;
                        if (++source_atoms_ > 1024) {
                            Fail(Error::NodeLimit, pos_);
                            break;
                        }
                    }
                    sup.children.push_back(Finish(std::move(primes)));
                    has_sup = true;
                    Space();
                    if (pos_ < source_.size() && source_[pos_] == L'^') {
                        ++pos_;
                        sup.children.push_back(Group(depth + 1));
                    }
                    sup = Finish(std::move(sup));
                    Space();
                    continue;
                }
                const bool upper = source_[pos_++] == L'^';
                if ((upper && has_sup) || (!upper && has_sub)) {
                    Fail(Error::DuplicateScript, marker);
                    break;
                }
                auto argument = Group(depth + 1);
                // Script children are always rows, including unbraced atoms.
                if (argument.kind != Kind::Row) {
                    auto wrapped = Node(Kind::Row, argument.source_start, depth + 1);
                    wrapped.source_length = argument.source_length;
                    wrapped.children.push_back(std::move(argument));
                    argument = std::move(wrapped);
                }
                if (upper) {
                    sup = std::move(argument);
                    has_sup = true;
                } else {
                    sub = std::move(argument);
                    has_sub = true;
                }
                Space();
            }
            if (has_sup || has_sub) {
                auto script = Node(Kind::Script, base.source_start, depth + 1);
                script.atom_class = base.atom_class;
                script.limits = limits;
                script.has_superscript = has_sup;
                script.has_subscript = has_sub;
                script.children.push_back(std::move(base));
                script.children.push_back(std::move(sup));
                script.children.push_back(std::move(sub));
                base = Finish(std::move(script));
            }
            row.children.push_back(std::move(base));
        }
        return Finish(std::move(row));
    }
    std::wstring Delimiter() {
        Space();
        const size_t at = pos_;
        if (pos_ == source_.size()) {
            Fail(Error::MissingDelimiter, at);
            return {};
        }
        if (source_[pos_] == L'\\') {
            const auto command = Command();
            for (const auto& value : delimiters)
                if (value.name == command)
                    return std::wstring(value.value);
            Fail(Error::MissingDelimiter, at);
            return {};
        }
        const wchar_t ch = source_[pos_++];
        if (ch == L'.')
            return {};
        if (ch == L'<')
            return L"⟨";
        if (ch == L'>')
            return L"⟩";
        if (std::wstring_view(L"()[]|/⟨⟩⌊⌋⌈⌉‖").find(ch) == std::wstring_view::npos)
            Fail(Error::MissingDelimiter, at);
        return std::wstring(1, ch);
    }
    MathNode Environment(size_t start, int depth, bool stack = false) {
        auto node = Node(Kind::Environment, start, depth);
        node.atom_class = Class::Inner;
        if (stack) {
            node.text = L"substack";
            node.style = MathStyle::Script;
            Space();
            if (pos_ == source_.size() || source_[pos_] != L'{') {
                Fail(Error::InvalidArgument, pos_);
                return node;
            }
            ++pos_;
        } else {
            node.text = RawGroup();
            if (!In(node.text, {L"matrix", L"pmatrix", L"bmatrix", L"Bmatrix", L"vmatrix", L"Vmatrix", L"cases",
                                L"aligned", L"smallmatrix", L"gathered", L"array"})) {
                Fail(Error::InvalidArgument, start);
                return node;
            }
            if (node.text == L"smallmatrix")
                node.style = MathStyle::Script;
            if (node.text == L"array") {
                node.style = MathStyle::Text;
                Space();
                const size_t columns_start = pos_ + 1;
                const auto columns = RawGroup();
                node.column_rules.push_back(0);
                for (size_t i = 0; i < columns.size(); ++i) {
                    const wchar_t c = columns[i];
                    if (iswspace(c))
                        continue;
                    if (c == L'|') {
                        if (++node.column_rules.back() > 2) {
                            Fail(Error::InvalidArgument, columns_start + i);
                            break;
                        }
                        continue;
                    }
                    if (c != L'l' && c != L'c' && c != L'r') {
                        Fail(Error::InvalidArgument, columns_start + i);
                        break;
                    }
                    node.auxiliary.push_back(c);
                    node.column_rules.push_back(0);
                    if (node.auxiliary.size() > 16) {
                        Fail(Error::InvalidArgument, columns_start + i);
                        break;
                    }
                }
                if (node.auxiliary.empty() || node.auxiliary.size() > 16)
                    Fail(Error::InvalidArgument, start);
            }
        }
        const bool array = node.text == L"array";
        const auto boundary_rules = [&]() {
            unsigned char count = 0;
            Space();
            while (Good() && Ahead(L"\\hline")) {
                const size_t at = pos_;
                Command();
                if (!array || ++count > 2)
                    Fail(Error::InvalidArgument, at);
                if (++source_atoms_ > 1024)
                    Fail(Error::NodeLimit, at);
                Space();
            }
            if (array)
                node.row_rules.push_back(count);
        };
        bool ended = false;
        boundary_rules();
        node.row_gaps.push_back({});
        auto row = Node(Kind::Row, pos_, depth + 1);
        while (Good()) {
            if (++cells_ > 128) {
                Fail(Error::CellLimit, pos_);
                break;
            }
            row.children.push_back(Row(depth + 2, stack ? L'}' : 0, true));
            if (row.children.size() > 16 || ((stack || node.text == L"gathered") && row.children.size() > 1) ||
                (node.text == L"cases" && row.children.size() > 2) ||
                (!node.auxiliary.empty() && row.children.size() > node.auxiliary.size()))
                Fail(Error::InvalidArgument, pos_);
            Space();
            row.source_length = pos_ - row.source_start;
            if (stack && pos_ < source_.size() && source_[pos_] == L'}') {
                ++pos_;
                ended = true;
                break;
            }
            if (!stack && Ahead(L"\\end")) {
                const size_t close = pos_;
                Command();
                if (RawGroup() != node.text)
                    Fail(Error::MismatchedEnvironment, close);
                ended = true;
                break;
            }
            if (Starts(L"\\\\")) {
                pos_ += 2;
                Space();
                if (pos_ < source_.size() && source_[pos_] == L'[')
                    node.row_gaps.back() = Length(L'[', L']', true);
                boundary_rules();
                if ((stack && pos_ < source_.size() && source_[pos_] == L'}') || (!stack && Ahead(L"\\end"))) {
                    if (stack)
                        ++pos_;
                    else {
                        const size_t close = pos_;
                        Command();
                        if (RawGroup() != node.text)
                            Fail(Error::MismatchedEnvironment, close);
                    }
                    ended = true;
                    break;
                }
                node.children.push_back(std::move(row));
                if (node.children.size() >= 32) {
                    Fail(Error::CellLimit, pos_);
                    break;
                }
                row = Node(Kind::Row, pos_, depth + 1);
                node.row_gaps.push_back({});
            } else if (!stack && pos_ < source_.size() && source_[pos_] == L'&')
                ++pos_;
            else {
                Fail(pos_ == source_.size() ? Error::UnexpectedEnd : Error::UnexpectedToken, pos_);
                break;
            }
        }
        if (!ended && Good())
            Fail(Error::UnexpectedEnd, pos_);
        node.children.push_back(std::move(row));
        if (array)
            node.row_rules.resize(node.children.size() + 1, 0);
        return Finish(std::move(node));
    }
    MathNode Atom(int depth) {
        Space();
        const size_t start = pos_;
        if (++source_atoms_ > 1024) {
            Fail(Error::NodeLimit, start);
            return Empty(start);
        }
        if (depth > 32) {
            Fail(Error::DepthLimit, start);
            return Empty(start);
        }
        if (pos_ == source_.size()) {
            Fail(Error::UnexpectedEnd, pos_);
            return Empty(pos_);
        }
        if (source_[pos_] == L'{')
            return Group(depth);
        auto node = Node(Kind::Glyph, start, depth);
        if (source_[pos_] != L'\\') {
            const wchar_t ch = source_[pos_++];
            if (ch >= 0xd800 && ch <= 0xdbff) {
                if (pos_ == source_.size() || source_[pos_] < 0xdc00 || source_[pos_] > 0xdfff)
                    Fail(Error::InvalidArgument, start);
                else {
                    node.text.push_back(ch);
                    node.text.push_back(source_[pos_++]);
                    node.require_glyph = true;
                    node.upright = true;
                }
            } else if (std::wstring_view(L"}^_&$#%~'").find(ch) != std::wstring_view::npos || ch < 32 ||
                       (ch >= 0xdc00 && ch <= 0xdfff))
                Fail(Error::UnexpectedToken, start);
            else {
                node.text.push_back(ch);
                node.atom_class = LiteralClass(ch);
                node.upright = !iswalpha(ch) || std::wstring_view(L"ℂℍℕℙℚℝℤ").find(ch) != std::wstring_view::npos;
                for (const auto& symbol : symbols) {
                    if (symbol.value.size() != 1 || symbol.value[0] != ch)
                        continue;
                    node.atom_class = symbol.atom_class;
                    if (symbol.atom_class == Class::Op) {
                        node.kind = Kind::Operator;
                        node.large_operator = true;
                        node.default_limits = !In(symbol.name, {L"int", L"iint", L"oint"});
                    }
                    break;
                }
            }
            return Finish(std::move(node));
        }
        const auto cmd = Command();
        for (const auto& symbol : delimiters)
            if (symbol.name == cmd) {
                node.text = symbol.value;
                node.atom_class = symbol.atom_class;
                node.upright = true;
                return Finish(std::move(node));
            }
        auto delimiter_size = cmd;
        wchar_t delimiter_class = 0;
        if (!delimiter_size.empty() &&
            std::wstring_view(L"lrm").find(delimiter_size.back()) != std::wstring_view::npos) {
            delimiter_class = delimiter_size.back();
            delimiter_size.remove_suffix(1);
        }
        if (In(delimiter_size, {L"big", L"Big", L"bigg", L"Bigg"})) {
            node.text = Delimiter();
            node.upright = true;
            node.space_em = delimiter_size == L"big"    ? 1.2f
                            : delimiter_size == L"Big"  ? 1.8f
                            : delimiter_size == L"bigg" ? 2.4f
                                                        : 3.0f;
            node.auxiliary = L"sized-delimiter";
            node.atom_class = delimiter_class == L'l'   ? Class::Open
                              : delimiter_class == L'r' ? Class::Close
                              : delimiter_class == L'm' ? Class::Rel
                                                        : Class::Ord;
            return Finish(std::move(node));
        }
        if (In(cmd, {L"displaystyle", L"textstyle", L"scriptstyle", L"scriptscriptstyle"})) {
            node.kind = Kind::Style;
            node.style = cmd == L"displaystyle"  ? MathStyle::Display
                         : cmd == L"textstyle"   ? MathStyle::Text
                         : cmd == L"scriptstyle" ? MathStyle::Script
                                                 : MathStyle::ScriptScript;
        } else if (In(cmd, {L"frac", L"dfrac", L"tfrac", L"binom", L"dbinom", L"tbinom"})) {
            node.kind = Kind::Fraction;
            node.atom_class = Class::Inner;
            node.text = cmd;
            node.style = (cmd == L"dfrac" || cmd == L"dbinom")   ? MathStyle::Display
                         : (cmd == L"tfrac" || cmd == L"tbinom") ? MathStyle::Text
                                                                 : MathStyle::Inherit;
            node.children.push_back(Group(depth + 1));
            node.children.push_back(Group(depth + 1));
        } else if (cmd == L"sqrt") {
            node.kind = Kind::Radical;
            auto index = Empty(pos_);
            Space();
            if (pos_ < source_.size() && source_[pos_] == L'[') {
                ++pos_;
                index = Row(depth + 1, L']');
                if (pos_ == source_.size() || source_[pos_] != L']')
                    Fail(Error::UnexpectedEnd, pos_);
                else
                    ++pos_;
                node.has_superscript = true;
            }
            node.children.push_back(Group(depth + 1));
            node.children.push_back(std::move(index));
        } else if (cmd == L"left") {
            node.kind = Kind::Fence;
            node.atom_class = Class::Inner;
            node.text = Delimiter();
            node.children.push_back(Row(depth + 1, 0, false, true));
            if (!Ahead(L"\\right"))
                Fail(Error::MissingDelimiter, pos_);
            else {
                Command();
                node.auxiliary = Delimiter();
            }
        } else if (cmd == L"begin" || cmd == L"substack")
            return Environment(start, depth + 1, cmd == L"substack");
        else if (cmd == L"hspace") {
            node.kind = Kind::Space;
            node.text = cmd;
            node.length = Length(L'{', L'}', false);
        } else if (In(cmd, {L"phantom", L"hphantom", L"vphantom", L"overbrace", L"underbrace"})) {
            node.kind = cmd == L"overbrace" || cmd == L"underbrace" ? Kind::Brace : Kind::Phantom;
            node.text = cmd;
            node.children.push_back(Group(depth + 1, true));
        } else if (In(cmd, {L"boxed", L"cancel", L"bcancel", L"xcancel"})) {
            node.kind = Kind::Decoration;
            node.text = cmd;
            node.children.push_back(Group(depth + 1));
            if (cmd == L"boxed")
                node.style = MathStyle::Display;
        } else if (cmd == L"xrightarrow" || cmd == L"xleftarrow") {
            node.kind = Kind::Arrow;
            node.atom_class = Class::Rel;
            node.text = cmd;
            auto below = Empty(pos_);
            Space();
            if (pos_ < source_.size() && source_[pos_] == L'[') {
                ++pos_;
                below = Row(depth + 1, L']');
                if (pos_ == source_.size() || source_[pos_] != L']')
                    Fail(Error::UnexpectedEnd, pos_);
                else
                    ++pos_;
            }
            node.children.push_back(Group(depth + 1, true));
            node.children.push_back(std::move(below));
        } else if (cmd == L"bmod" || cmd == L"pmod") {
            node.kind = Kind::Operator;
            node.text = cmd;
            node.atom_class = cmd == L"bmod" ? Class::Bin : Class::Inner;
            if (cmd == L"pmod")
                node.children.push_back(Group(depth + 1));
        } else if (cmd == L"overset" || cmd == L"underset") {
            node.kind = Kind::Accent;
            node.text = cmd;
            node.children.push_back(Group(depth + 1));
            node.children.push_back(Group(depth + 1));
            const MathNode* base = &node.children.back();
            while (base->kind == Kind::Row && base->children.size() == 1)
                base = &base->children.front();
            node.atom_class =
                base->atom_class == Class::Rel || base->atom_class == Class::Bin ? base->atom_class : Class::Ord;
        } else if (cmd == L"mathbb") {
            const auto raw = RawGroup();
            node.upright = true;
            node.require_glyph = true;
            node.auxiliary = L"mathbb";
            for (wchar_t ch : raw) {
                if (iswspace(ch))
                    continue;
                if (++source_atoms_ > 1024) {
                    Fail(Error::NodeLimit, start);
                    break;
                }
                unsigned int scalar = 0;
                if (ch >= L'A' && ch <= L'Z') {
                    scalar = 0x1d538 + static_cast<unsigned int>(ch - L'A');
                    switch (ch) {
                    case L'C':
                        scalar = 0x2102;
                        break;
                    case L'H':
                        scalar = 0x210d;
                        break;
                    case L'N':
                        scalar = 0x2115;
                        break;
                    case L'P':
                        scalar = 0x2119;
                        break;
                    case L'Q':
                        scalar = 0x211a;
                        break;
                    case L'R':
                        scalar = 0x211d;
                        break;
                    case L'Z':
                        scalar = 0x2124;
                        break;
                    default:
                        break;
                    }
                } else if (ch >= L'a' && ch <= L'z')
                    scalar = 0x1d552 + static_cast<unsigned int>(ch - L'a');
                else if (ch >= L'0' && ch <= L'9')
                    scalar = 0x1d7d8 + static_cast<unsigned int>(ch - L'0');
                else {
                    Fail(Error::InvalidArgument, start);
                    break;
                }
                if (scalar <= 0xffff)
                    node.text.push_back(static_cast<wchar_t>(scalar));
                else {
                    scalar -= 0x10000;
                    node.text.push_back(static_cast<wchar_t>(0xd800 + (scalar >> 10)));
                    node.text.push_back(static_cast<wchar_t>(0xdc00 + (scalar & 0x3ff)));
                }
            }
            if (node.text.empty())
                Fail(Error::InvalidArgument, start);
        } else if (In(cmd, {L"text", L"textrm", L"textbf", L"operatorname", L"operatornamewithlimits"})) {
            const bool op = cmd == L"operatorname" || cmd == L"operatornamewithlimits";
            Space();
            bool starred = cmd == L"operatornamewithlimits";
            if (cmd == L"operatorname" && pos_ < source_.size() && source_[pos_] == L'*') {
                starred = true;
                ++pos_;
            }
            node.text = RawGroup();
            node.upright = true;
            node.bold = cmd == L"textbf";
            if (!op)
                node.auxiliary = L"text";
            if (op) {
                node.kind = Kind::Operator;
                node.atom_class = Class::Op;
                node.default_limits = starred;
            }
        } else if (In(cmd, {L"mathrm", L"mathbf", L"mathit", L"mathcal", L"mathscr", L"mathfrak", L"mathsf",
                            L"mathtt", L"boldsymbol", L"bm"})) {
            node.kind = Kind::Style;
            node.text = cmd;
            node.children.push_back(Group(depth + 1));
        } else if (In(cmd, {L"overline",
                            L"bar",
                            L"underline",
                            L"hat",
                            L"widehat",
                            L"vec",
                            L"dot",
                            L"ddot",
                            L"tilde",
                            L"widetilde",
                            L"acute",
                            L"grave",
                            L"breve",
                            L"check",
                            L"mathring",
                            L"overrightarrow",
                            L"overleftarrow",
                            L"overleftrightarrow",
                            L"underrightarrow",
                            L"underleftarrow",
                            L"underleftrightarrow"})) {
            node.kind = Kind::Accent;
            node.text = cmd;
            node.children.push_back(Group(depth + 1));
        } else if (In(cmd, {L",", L":", L";", L" ", L"quad", L"qquad", L"enspace", L"!"})) {
            node.kind = Kind::Space;
            node.space_em = cmd == L"!"         ? -1.0f / 6
                            : cmd == L"qquad"   ? 2
                            : cmd == L"quad"    ? 1
                            : cmd == L"enspace" ? .5f
                            : cmd == L","       ? 1.0f / 6
                            : cmd == L";"       ? 5.0f / 18
                                                : 2.0f / 9;
        } else if (In(cmd, {L"sin",  L"cos",  L"tan",    L"cot",    L"sec", L"csc",    L"arcsin", L"arccos", L"arctan",
                            L"sinh", L"cosh", L"tanh",   L"log",    L"ln",  L"exp",    L"lim",    L"min",    L"max",
                            L"det",  L"gcd",  L"arg",    L"deg",    L"dim", L"hom",    L"ker",    L"coth",   L"lg",
                            L"inf",  L"sup",  L"liminf", L"limsup", L"Pr",  L"argmax", L"argmin"})) {
            node.kind = Kind::Operator;
            node.atom_class = Class::Op;
            node.upright = true;
            node.text = cmd == L"liminf"   ? L"lim inf"
                        : cmd == L"limsup" ? L"lim sup"
                        : cmd == L"argmax" ? L"arg max"
                        : cmd == L"argmin" ? L"arg min"
                                           : cmd;
            node.default_limits = In(cmd, {L"lim", L"min", L"max", L"det", L"gcd", L"inf", L"sup", L"liminf", L"limsup",
                                           L"Pr", L"argmax", L"argmin"});
        } else if (In(cmd, {L"_", L"%", L"#", L"&", L"$"})) {
            node.text = cmd;
            node.upright = true;
        } else {
            bool found = false;
            for (const auto& symbol : symbols)
                if (symbol.name == cmd) {
                    found = true;
                    node.text = symbol.value;
                    node.atom_class = symbol.atom_class;
                    node.upright = true;
                    if (symbol.atom_class == Class::Op) {
                        node.kind = Kind::Operator;
                        node.large_operator = true;
                        node.default_limits = !In(cmd, {L"int", L"iint", L"oint"});
                    }
                    break;
                }
            if (!found)
                Fail(Error::UnknownCommand, start);
        }
        return Finish(std::move(node));
    }
};
} // namespace

MathParseResult ParseMathSyntax(std::wstring_view source) {
    if (source.size() > 4096)
        return SyntaxParser(source).Parse();
    bool has_definition = false;
    for (size_t i = 0; i < source.size() && !has_definition;) {
        if (source[i++] != L'\\')
            continue;
        const size_t start = i;
        if (i < source.size() && iswalpha(source[i]))
            while (i < source.size() && iswalpha(source[i]))
                ++i;
        else if (i < source.size())
            ++i;
        has_definition = In(source.substr(start, i - start), {L"newcommand", L"renewcommand", L"providecommand"});
    }
    if (!has_definition)
        return SyntaxParser(source).Parse();
    auto expanded = ExpandMathMacros(source, BuiltinCommand);
    if (expanded.error.code != Error::None)
        return {{}, expanded.error};
    auto parsed = SyntaxParser(expanded.source).Parse();
    if (parsed.error.code != Error::None)
        parsed.error.offset = parsed.error.offset < expanded.origins.size()
                                  ? expanded.origins[parsed.error.offset].start
                                  : source.size();
    const auto map_ranges = [&](auto&& self, MathNode& node) -> void {
        const size_t end = std::min(node.source_start + node.source_length, expanded.origins.size());
        size_t original_start = source.size(), original_end = 0;
        for (size_t i = node.source_start; i < end; ++i) {
            original_start = std::min(original_start, expanded.origins[i].start);
            original_end = std::max(original_end, expanded.origins[i].start + expanded.origins[i].length);
        }
        if (original_end < original_start) {
            original_start = node.source_start < expanded.origins.size()
                                 ? expanded.origins[node.source_start].start
                                 : source.size();
            original_end = original_start;
        }
        node.source_start = original_start;
        node.source_length = original_end - original_start;
        for (auto& child : node.children)
            self(self, child);
    };
    map_ranges(map_ranges, parsed.root);
    return parsed;
}

void NormalizeMathAtomClasses(std::vector<MathAtomClass>& classes) {
    for (size_t i = 0; i < classes.size(); ++i) {
        if (classes[i] != Class::Bin)
            continue;
        if (i == 0) { // Start-of-list binaries are unary.
            classes[i] = Class::Ord;
            continue;
        }
        const Class previous = classes[i - 1];
        if (previous == Class::Bin || previous == Class::Op || previous == Class::Rel || previous == Class::Open ||
            previous == Class::Punct)
            classes[i] = Class::Ord;
    }
    for (size_t i = 0; i < classes.size(); ++i)
        if (classes[i] == Class::Bin && (i + 1 == classes.size() || classes[i + 1] == Class::Rel ||
                                         classes[i + 1] == Class::Close || classes[i + 1] == Class::Punct))
            classes[i] = Class::Ord;
}

float MathAtomSpacing(MathAtomClass left, MathAtomClass right, MathStyle style) {
    if (style == MathStyle::Script || style == MathStyle::ScriptScript) {
        return ((right == Class::Op &&
                 (left == Class::Ord || left == Class::Op || left == Class::Close || left == Class::Inner)) ||
                (left == Class::Op && right == Class::Ord))
                   ? 1.0f / 6
                   : 0;
    }
    // Entries are mu: thin=3, medium=4, thick=5. Invalid binary contexts are
    // normalized before lookup, rather than adding punctuation heuristics.
    constexpr unsigned char table[8][8] = {{0, 3, 4, 5, 0, 0, 0, 3}, {3, 3, 0, 5, 0, 0, 0, 3}, {4, 4, 0, 0, 4, 0, 0, 4},
                                           {5, 5, 0, 0, 5, 0, 0, 5}, {0, 0, 0, 0, 0, 0, 0, 0}, {0, 3, 4, 5, 0, 0, 0, 3},
                                           {3, 3, 0, 5, 3, 3, 3, 3}, {3, 3, 4, 5, 3, 0, 3, 3}};
    return static_cast<float>(table[static_cast<size_t>(left)][static_cast<size_t>(right)]) / 18;
}
} // namespace pulse::ui
