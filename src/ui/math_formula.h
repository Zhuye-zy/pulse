#pragma once

#include <d2d1_1.h>
#include <dwrite_2.h>
#include <string_view>

namespace pulse::ui {

// A deliberately bounded TeX subset. Failure leaves the text layout unchanged.
bool ApplyMathInline(IDWriteFactory2* factory, IDWriteTextLayout* layout, DWRITE_TEXT_RANGE range,
                     std::wstring_view source, float font_size, bool display, float max_width);

// Uses the same drawing effects as DrawTextLayout, and renders native math shapes.
void DrawMathTextLayout(ID2D1DeviceContext* dc, IDWriteTextLayout* layout, D2D1_POINT_2F origin,
                        ID2D1SolidColorBrush* brush);

} // namespace pulse::ui
