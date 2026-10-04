#pragma once

#include <cstdint>
#include <span>

struct IDWriteFontFace;

namespace pulse::ui::math {

// Distances are em units; fallback values keep layout available without MATH.
struct FontMathMetrics {
    bool from_font = false;
    float script_scale = 0.7f;
    float script_script_scale = 0.5f;
    float axis_height = 0.25f;
    float subscript_shift_down = 0.24f;
    float superscript_shift_up = 0.5f;
    float sub_superscript_gap = 0.16f;
    float space_after_script = 0.04f;
    float upper_limit_gap = 0.12f;
    float lower_limit_gap = 0.12f;
    float fraction_numerator_shift_up = 0.5f;
    float fraction_display_numerator_shift_up = 0.65f;
    float fraction_denominator_shift_down = 0.5f;
    float fraction_display_denominator_shift_down = 0.65f;
    float fraction_numerator_gap = 0.15f;
    float fraction_display_numerator_gap = 0.15f;
    float fraction_denominator_gap = 0.15f;
    float fraction_display_denominator_gap = 0.15f;
    float fraction_rule_thickness = 0.055f;
    float radical_gap = 0.1f;
    float radical_display_gap = 0.1f;
    float radical_rule_thickness = 0.055f;
    float overbar_gap = 0.13f;
    float overbar_rule_thickness = 0.05f;
    float underbar_gap = 0.13f;
    float underbar_rule_thickness = 0.05f;
};

// Reads only the constant records, never device or glyph offsets. On failure
// output is unchanged. Device corrections are deliberately ignored for DPI stability.
bool ParseMathFontTable(std::span<const uint8_t> table, uint16_t units_per_em,
    FontMathMetrics& output) noexcept;
FontMathMetrics ReadFontMathMetrics(IDWriteFontFace* face) noexcept;

}
