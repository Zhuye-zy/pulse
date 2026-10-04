#pragma once
#include "icon_artwork_bounds.h"
#include <d2d1.h>
#include <d2d1helper.h>
#include <algorithm>
#include <cstdint>

namespace pulse::ui {
inline D2D1_RECT_F ThumbnailArtworkRect(const D2D1_RECT_F& target,
                                      const IconArtworkBounds& bounds) {
    const float width = target.right - target.left;
    const float height = target.bottom - target.top;
    return D2D1::RectF(target.left + width * bounds.left,
        target.top + height * bounds.top, target.left + width * bounds.right,
        target.top + height * bounds.bottom);
}

inline D2D1_RECT_F ContainedThumbnailRect(const D2D1_RECT_F& dest, uint32_t w, uint32_t h,
    const IconArtworkBounds& bounds, bool align_artwork_bottom) {
    const float dest_width = (std::max)(1.0f, dest.right - dest.left);
    const float dest_height = (std::max)(1.0f, dest.bottom - dest.top);
    D2D1_RECT_F fitted = dest;
    const float source_aspect = static_cast<float>(w) / static_cast<float>((std::max)(1u, h));
    const float dest_aspect = dest_width / dest_height;
    if (source_aspect > dest_aspect) {
        const float height = dest_width / source_aspect;
        const float center = (dest.top + dest.bottom) * 0.5f;
        fitted.top = center - height * 0.5f;
        fitted.bottom = center + height * 0.5f;
    } else if (source_aspect < dest_aspect) {
        const float width = dest_height * source_aspect;
        const float center = (dest.left + dest.right) * 0.5f;
        fitted.left = center - width * 0.5f;
        fitted.right = center + width * 0.5f;
    }
    if (align_artwork_bottom) {
        const float offset = dest.bottom - ThumbnailArtworkRect(fitted, bounds).bottom;
        fitted.top += offset;
        fitted.bottom += offset;
    }
    return fitted;
}
} // namespace pulse::ui
