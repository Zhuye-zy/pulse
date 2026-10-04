#include "math_font_metrics.h"

#include <dwrite.h>

namespace pulse::ui::math {

FontMathMetrics ReadFontMathMetrics(IDWriteFontFace* face) noexcept {
    FontMathMetrics result;
    if (!face) return result;
    const void* data = nullptr;
    UINT32 bytes = 0;
    void* context = nullptr;
    BOOL exists = FALSE;
    if (FAILED(face->TryGetFontTable(DWRITE_MAKE_OPENTYPE_TAG('M', 'A', 'T', 'H'),
        &data, &bytes, &context, &exists)) || !exists) return result;
    struct TableLease {
        IDWriteFontFace* face;
        void* context;
        ~TableLease() { face->ReleaseFontTable(context); }
    } lease{face, context};
    DWRITE_FONT_METRICS metrics{};
    face->GetMetrics(&metrics);
    if (data && bytes <= 1024 * 1024)
        ParseMathFontTable({static_cast<const uint8_t*>(data), bytes}, metrics.designUnitsPerEm, result);
    return result;
}
}
