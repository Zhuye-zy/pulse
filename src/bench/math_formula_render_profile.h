#pragma once
#include <string>
#include <vector>

namespace pulse_test {
struct MathRenderProfile {
    std::wstring title;
    std::wstring file_prefix;
    std::vector<std::wstring> formulas;
    std::wstring markdown;
    std::wstring copy_probe;
    float markdown_height = 1500;
};
}
