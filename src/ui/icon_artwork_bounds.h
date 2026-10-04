#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace pulse::ui {

struct IconArtworkBounds {
    float left = 0.0f;
    float top = 0.0f;
    float right = 1.0f;
    float bottom = 1.0f;
};

// Pixel edges, not pixel centers: a fully opaque image occupies [0, 1].
inline IconArtworkBounds MeasureIconArtwork(std::span<const uint8_t> bgra,
                                           size_t width, size_t height,
                                           size_t stride = 0) noexcept {
    if (!width || !height || width > bgra.size() / 4 / height) return {};
    if (!stride) stride = width * 4;
    if (stride < width * 4 || height > bgra.size() / stride) return {};
    size_t left = width, top = height, right = 0, bottom = 0;
    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            if (!bgra[y * stride + x * 4 + 3]) continue;
            if (x < left) left = x;
            if (y < top) top = y;
            if (x + 1 > right) right = x + 1;
            if (y + 1 > bottom) bottom = y + 1;
        }
    }
    if (!right || !bottom) return {};
    return { static_cast<float>(left) / static_cast<float>(width),
             static_cast<float>(top) / static_cast<float>(height),
             static_cast<float>(right) / static_cast<float>(width),
             static_cast<float>(bottom) / static_cast<float>(height) };
}

} // namespace pulse::ui
