#include "math_font_metrics.h"

#include <cstddef>

namespace pulse::ui::math {
namespace {
uint16_t U16(std::span<const uint8_t> data, size_t at) noexcept {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[at]) << 8) | data[at + 1]);
}
int Signed16(uint16_t value) noexcept {
    return value < 0x8000 ? value : static_cast<int>(value) - 0x10000;
}
}

bool ParseMathFontTable(std::span<const uint8_t> table, uint16_t units_per_em,
    FontMathMetrics& output) noexcept {
    constexpr size_t constants_size = 214;
    if (units_per_em < 16 || units_per_em > 16384 || table.size() < 10 ||
        U16(table, 0) != 1 || U16(table, 2) != 0) return false;
    const size_t offset = U16(table, 4);
    if (offset < 10 || offset > table.size() || table.size() - offset < constants_size) return false;
    const auto constants = table.subspan(offset, constants_size);
    const auto script = Signed16(U16(constants, 0));
    const auto script_script = Signed16(U16(constants, 2));
    if (script < 20 || script > 100 || script_script < 10 || script_script > script) return false;

    FontMathMetrics result;
    result.script_scale = static_cast<float>(script) / 100.0f;
    result.script_script_scale = static_cast<float>(script_script) / 100.0f;
    // Indices are MathValueRecord positions in the OpenType 1.0 MathConstants
    // table. Each is a signed design-unit value followed by an unused device offset.
    struct Field { size_t index; float FontMathMetrics::* member; bool thickness; };
    constexpr Field fields[] = {
        {1, &FontMathMetrics::axis_height, false},
        {4, &FontMathMetrics::subscript_shift_down, false},
        {7, &FontMathMetrics::superscript_shift_up, false},
        {11, &FontMathMetrics::sub_superscript_gap, false},
        {13, &FontMathMetrics::space_after_script, false},
        {14, &FontMathMetrics::upper_limit_gap, false},
        {16, &FontMathMetrics::lower_limit_gap, false},
        {28, &FontMathMetrics::fraction_numerator_shift_up, false},
        {29, &FontMathMetrics::fraction_display_numerator_shift_up, false},
        {30, &FontMathMetrics::fraction_denominator_shift_down, false},
        {31, &FontMathMetrics::fraction_display_denominator_shift_down, false},
        {32, &FontMathMetrics::fraction_numerator_gap, false},
        {33, &FontMathMetrics::fraction_display_numerator_gap, false},
        {34, &FontMathMetrics::fraction_rule_thickness, true},
        {35, &FontMathMetrics::fraction_denominator_gap, false},
        {36, &FontMathMetrics::fraction_display_denominator_gap, false},
        {39, &FontMathMetrics::overbar_gap, false},
        {40, &FontMathMetrics::overbar_rule_thickness, true},
        {42, &FontMathMetrics::underbar_gap, false},
        {43, &FontMathMetrics::underbar_rule_thickness, true},
        {45, &FontMathMetrics::radical_gap, false},
        {46, &FontMathMetrics::radical_display_gap, false},
        {47, &FontMathMetrics::radical_rule_thickness, true}
    };
    for (const auto& field : fields) {
        const float value = static_cast<float>(Signed16(U16(constants, 8 + 4 * field.index))) /
            static_cast<float>(units_per_em);
        if (value < 0 || value > 4 || (field.thickness && (value <= 0 || value > 0.5f))) return false;
        result.*(field.member) = value;
    }
    result.from_font = true;
    output = result;
    return true;
}
}
