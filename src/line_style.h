#pragma once

#include <algorithm>
#include <cmath>

namespace woby {
inline constexpr float minLineWidth = 1.0f, maxLineWidth = 12.0f;
struct LineStyle {
    float width = 2.0f; // Drawable pixels, independent of world scale.
    bool depthTest = true;
    friend bool operator==(const LineStyle&, const LineStyle&) = default;
};
[[nodiscard]] inline LineStyle normalizedLineStyle(LineStyle style)
{
    style.width = std::isfinite(style.width) ? std::clamp(style.width, minLineWidth, maxLineWidth) : 2.0f;
    return style;
}
} // namespace woby
