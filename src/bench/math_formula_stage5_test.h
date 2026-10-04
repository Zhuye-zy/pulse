#pragma once
#include <dwrite_2.h>
#include <string>
#include <vector>

namespace pulse_test {
int TestMathStage5(IDWriteFactory2* factory);
const std::wstring& MathStage5Source();
const std::vector<std::wstring>& MathStage5Formulas();
}
