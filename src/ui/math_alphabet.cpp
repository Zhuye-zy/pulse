#include "math_alphabet.h"

#include <cstdint>
#include <utility>

namespace pulse::ui::math {
namespace {
uint32_t Letter(uint32_t scalar, MathAlphabet alphabet, bool bold) {
    const bool upper = scalar >= 'A' && scalar <= 'Z';
    const bool lower = scalar >= 'a' && scalar <= 'z';
    const bool digit = scalar >= '0' && scalar <= '9';
    const uint32_t index = upper ? scalar - 'A' : lower ? scalar - 'a' : digit ? scalar - '0' : 0;
    switch (alphabet) {
    case MathAlphabet::Script:
        if (bold)
            return upper ? 0x1d4d0 + index : lower ? 0x1d4ea + index : scalar;
        // These characters predate the supplementary mathematical alphabet.
        switch (scalar) {
        case 'B':
            return 0x212c;
        case 'E':
            return 0x2130;
        case 'F':
            return 0x2131;
        case 'H':
            return 0x210b;
        case 'I':
            return 0x2110;
        case 'L':
            return 0x2112;
        case 'M':
            return 0x2133;
        case 'R':
            return 0x211b;
        case 'e':
            return 0x212f;
        case 'g':
            return 0x210a;
        case 'o':
            return 0x2134;
        default:
            return upper ? 0x1d49c + index : lower ? 0x1d4b6 + index : scalar;
        }
    case MathAlphabet::Fraktur:
        if (bold)
            return upper ? 0x1d56c + index : lower ? 0x1d586 + index : scalar;
        switch (scalar) {
        case 'C':
            return 0x212d;
        case 'H':
            return 0x210c;
        case 'I':
            return 0x2111;
        case 'R':
            return 0x211c;
        case 'Z':
            return 0x2128;
        default:
            return upper ? 0x1d504 + index : lower ? 0x1d51e + index : scalar;
        }
    case MathAlphabet::SansSerif:
        return upper   ? (bold ? 0x1d5d4 : 0x1d5a0) + index
               : lower ? (bold ? 0x1d5ee : 0x1d5ba) + index
               : digit ? (bold ? 0x1d7ec : 0x1d7e2) + index
                       : scalar;
    case MathAlphabet::Monospace:
        return upper ? 0x1d670 + index : lower ? 0x1d68a + index : digit ? 0x1d7f6 + index : scalar;
    case MathAlphabet::BoldItalic:
        if (upper)
            return 0x1d468 + index;
        if (lower)
            return 0x1d482 + index;
        if (digit)
            return 0x1d7ce + index;
        if (scalar >= 0x391 && scalar <= 0x3a9 && scalar != 0x3a2)
            return 0x1d71c + scalar - 0x391;
        if (scalar >= 0x3b1 && scalar <= 0x3c9)
            return 0x1d736 + scalar - 0x3b1;
        switch (scalar) {
        case 0x3f4:
            return 0x1d72d; // Capital theta symbol.
        case 0x2207:
            return 0x1d735;
        case 0x2202:
            return 0x1d74f;
        case 0x3f5:
            return 0x1d750;
        case 0x3d1:
            return 0x1d751;
        case 0x3f0:
            return 0x1d752;
        case 0x3d5:
            return 0x1d753;
        case 0x3f1:
            return 0x1d754;
        case 0x3d6:
            return 0x1d755;
        default:
            return scalar;
        }
    case MathAlphabet::Default:
        return scalar;
    }
    return scalar;
}
void Append(std::wstring& result, uint32_t scalar) {
    if (scalar <= 0xffff)
        result.push_back(static_cast<wchar_t>(scalar));
    else {
        scalar -= 0x10000;
        result.push_back(static_cast<wchar_t>(0xd800 + (scalar >> 10)));
        result.push_back(static_cast<wchar_t>(0xdc00 + (scalar & 0x3ff)));
    }
}
} // namespace
bool MapMathAlphabet(std::wstring_view source, MathAlphabet alphabet, std::wstring& output, bool bold) {
    std::wstring result;
    result.reserve(source.size() * 2);
    for (size_t i = 0; i < source.size(); ++i) {
        uint32_t scalar = static_cast<uint32_t>(source[i]);
        if (scalar >= 0xd800 && scalar <= 0xdbff) {
            if (++i == source.size() || source[i] < 0xdc00 || source[i] > 0xdfff)
                return false;
            scalar = 0x10000 + ((scalar - 0xd800) << 10) + (static_cast<uint32_t>(source[i]) - 0xdc00);
        } else if (scalar >= 0xdc00 && scalar <= 0xdfff)
            return false;
        if (scalar > 0x10ffff)
            return false;
        Append(result, Letter(scalar, alphabet, bold));
    }
    output = std::move(result);
    return true;
}
} // namespace pulse::ui::math
