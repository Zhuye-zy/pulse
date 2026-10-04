#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::ui {

enum class MathAtomClass {
    Ord,
    Op,
    Bin,
    Rel,
    Open,
    Close,
    Punct,
    Inner
};
enum class MathNodeKind {
    Row,
    Glyph,
    Space,
    Operator,
    Script,
    Fraction,
    Radical,
    Fence,
    Environment,
    Accent,
    Decoration,
    Arrow,
    Style,
    Middle,
    Brace,
    Phantom
};

enum class MathLengthUnit { Em, Ex, Pt };

struct MathLength {
    float value = 0;
    MathLengthUnit unit = MathLengthUnit::Em;
};
enum class MathStyle {
    Inherit,
    Display,
    Text,
    Script,
    ScriptScript
};
enum class MathLimits {
    Default,
    AboveBelow,
    Side
};
enum class MathParseErrorCode {
    None,
    EmptyInput,
    SourceLimit,
    NodeLimit,
    DepthLimit,
    CellLimit,
    UnexpectedEnd,
    UnexpectedToken,
    UnknownCommand,
    InvalidArgument,
    DuplicateScript,
    MissingDelimiter,
    MismatchedEnvironment
};

struct MathParseError {
    MathParseErrorCode code = MathParseErrorCode::None;
    size_t offset = 0;
};

// Children: Script=(base, superscript row, subscript row); Fraction=(numerator,
// denominator); Radical=(body, index row); Arrow=(above, below). Environment
// children are rows of cells. Style nodes with no children are row declarations.
// Fence owns one body row with directly scoped Middle nodes (text=delimiter).
// Brace/Phantom own one body; text specifies their command. Explicit lengths
// permit at most 20em, 40ex or 200pt in magnitude; row gaps are nonnegative.
// All ranges refer to UTF-16 code units in the original source, excluding $ fences.
struct MathNode {
    MathNodeKind kind = MathNodeKind::Row;
    MathAtomClass atom_class = MathAtomClass::Ord;
    size_t source_start = 0;
    size_t source_length = 0;
    std::wstring text;
    std::wstring auxiliary;
    std::vector<MathNode> children;
    MathStyle style = MathStyle::Inherit;
    MathLimits limits = MathLimits::Default;
    float space_em = 0;
    MathLength length; // Explicit hspace length; regular spaces use space_em.
    // Array boundary rules have columns+1 / rows+1 entries, each at most two.
    std::vector<unsigned char> column_rules;
    std::vector<unsigned char> row_rules;
    std::vector<MathLength> row_gaps; // Extra space after each environment row.
    bool large_operator = false;
    bool default_limits = false;
    bool upright = false;
    bool bold = false;
    bool require_glyph = false;
    bool has_superscript = false;
    bool has_subscript = false;
};

struct MathParseResult {
    MathNode root;
    MathParseError error;
    explicit operator bool() const { return error.code == MathParseErrorCode::None; }
};

// Deterministic, bounded, platform-independent parsing; no font or graphics work.
// Budgets: 4096 UTF-16 units, 1024 source atoms, 4096 structural nodes, depth 32,
// and 128 environment cells. Row/script wrappers do not consume source atoms.
// Formula-local new/renew/providecommand expansion additionally limits working
// text to 4096 units, definitions to 64, invocations to 256 and expansion depth
// to 32. Expanded nodes and errors retain ranges in the original source.
MathParseResult ParseMathSyntax(std::wstring_view source);

// TeX atom spacing in em; script styles retain only operator thin spaces.
float MathAtomSpacing(MathAtomClass left, MathAtomClass right, MathStyle style);
void NormalizeMathAtomClasses(std::vector<MathAtomClass>& classes);

} // namespace pulse::ui
