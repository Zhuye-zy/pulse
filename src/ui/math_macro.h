#pragma once

#include "math_syntax.h"

namespace pulse::ui {

struct MathSourceOrigin {
    size_t start = 0;
    size_t length = 0;
};

struct MathMacroExpansion {
    std::wstring source;
    std::vector<MathSourceOrigin> origins;
    MathParseError error;
};

// Formula-local token expansion. Limits apply to raw/working/output UTF-16
// units (4096), definition instructions (64), calls (256), and call depth (32).
// The caller supplies its own command registry so built-ins cannot be replaced.
MathMacroExpansion ExpandMathMacros(std::wstring_view source, bool (*is_builtin)(std::wstring_view));

} // namespace pulse::ui
