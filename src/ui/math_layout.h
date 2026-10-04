#pragma once

#include "math_font_metrics.h"
#include "math_syntax.h"
#include <dwrite_2.h>
#include <vector>
#include <wrl/client.h>

namespace pulse::ui::math {
struct FormulaGlyph {
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    float x = 0, y = 0;
};
struct FormulaRule {
    float x1, y1, x2, y2, width;
};
struct FormulaLayout {
    float width = 0, ascent = 0, descent = 0;
    // An intentional phantom is meaningful even when it has no painted ink.
    bool intentional_space = false;
    std::vector<FormulaGlyph> glyphs;
    std::vector<FormulaRule> rules;
};

// Consumes syntax nodes only; there is no source-string parsing in layout/draw.
bool BuildMathLayout(IDWriteFactory2 *factory, IDWriteFontFace *face, const FontMathMetrics &metrics,
                     const MathNode &root, float font_size, bool display, FormulaLayout &result);
} // namespace pulse::ui::math
