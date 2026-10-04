#pragma once
#include <dwrite_2.h>
#include <string>
#include <vector>

namespace pulse_test {
int TestMathStage3(IDWriteFactory2* factory);
const std::wstring& MathStage3Source();
const std::vector<std::wstring>& MathStage3Formulas();
}
