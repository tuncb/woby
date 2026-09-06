#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace woby {

// Runtime layout, in drawable pixels. Panel widths arrive in window coordinates.
struct SceneViewport {
    uint32_t x = 0;
    uint32_t width = 1;
    uint32_t height = 1;
};

inline SceneViewport sceneViewport(uint32_t width, uint32_t height,
    float windowWidth, float leftWidth, float rightWidth)
{
    width = std::max(width, 1u);
    const float scale = static_cast<float>(width) / std::max(windowWidth, 1.0f);
    const auto left = static_cast<uint32_t>(std::clamp(std::ceil(leftWidth * scale), 0.0f, static_cast<float>(width - 1)));
    const auto right = static_cast<uint32_t>(std::clamp(std::floor((windowWidth - rightWidth) * scale),
        static_cast<float>(left + 1), static_cast<float>(width)));
    return {left, right - left, std::max(height, 1u)};
}

inline bool contains(const SceneViewport& viewport, float x, float y)
{
    return x >= static_cast<float>(viewport.x) && x < static_cast<float>(viewport.x + viewport.width)
        && y >= 0.0f && y < static_cast<float>(viewport.height);
}

} // namespace woby
