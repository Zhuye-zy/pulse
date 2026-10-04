#pragma once
#include "math_formula_render_profile.h"
#include <dwrite_2.h>

namespace pulse_test {
int TestMathStructures(IDWriteFactory2* factory);
const MathRenderProfile& MathStructuresProfile();
}
