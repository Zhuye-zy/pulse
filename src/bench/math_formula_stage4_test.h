#pragma once
#include <dwrite_2.h>
#include <string>
#include <vector>

namespace pulse_test {
int TestMathStage4(IDWriteFactory2* factory);
const std::wstring& MathStage4Source();
const std::vector<std::wstring>& MathStage4Formulas();
}
