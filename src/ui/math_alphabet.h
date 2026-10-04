#pragma once

#include <string>
#include <string_view>

namespace pulse::ui::math {
enum class MathAlphabet { Default, Script, Fraktur, SansSerif, Monospace, BoldItalic };

// Maps mathematical letters to Unicode, including Letterlike Symbols holes.
// Symbols outside an alphabet retain their identity. Invalid UTF-16 fails
// without changing output; font coverage must be checked separately.
bool MapMathAlphabet(std::wstring_view source, MathAlphabet alphabet, std::wstring& output, bool bold = false);
} // namespace pulse::ui::math
