#pragma once

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace woby {

// SDL reports display scale in physical pixels; ImGui lays out in window units.
inline float logicalUiScale(float preference, float displayScale, float pixelDensity)
{
    const auto positive = [](float value) { return std::isfinite(value) && value > 0.0f; };
    preference = std::clamp(positive(preference) ? preference : 1.0f, 1.0f, 2.0f);
    const float monitor = positive(displayScale) && positive(pixelDensity)
        ? displayScale / pixelDensity : 1.0f;
    return preference * std::clamp(monitor, 0.5f, 4.0f);
}

// Build from the unscaled style each time: ImGui's integer rounding is lossy.
inline ImGuiStyle scaledUiStyle(const ImGuiStyle& baseStyle, float scale)
{
    auto style = baseStyle;
    style.ScaleAllSizes(scale);
    // ImGui 1.92.7 rounds a one-pixel separator to zero below 100%, but
    // SeparatorEx requires a strictly positive thickness.
    style.SeparatorSize = std::max(1.0f, style.SeparatorSize);
    style.FontScaleMain = scale;
    return style;
}

} // namespace woby
