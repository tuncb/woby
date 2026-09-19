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
    uint32_t y = 0;
};

inline SceneViewport sceneViewport(uint32_t width, uint32_t height,
    float windowWidth, float leftWidth, float rightWidth, float topHeight = 0.0f, float windowHeight = 0.0f)
{
    width = std::max(width, 1u);
    const float scale = static_cast<float>(width) / std::max(windowWidth, 1.0f);
    const auto left = static_cast<uint32_t>(std::clamp(std::ceil(leftWidth * scale), 0.0f, static_cast<float>(width - 1)));
    const auto right = static_cast<uint32_t>(std::clamp(std::floor((windowWidth - rightWidth) * scale),
        static_cast<float>(left + 1), static_cast<float>(width)));
    height = std::max(height, 1u);
    const float verticalScale = windowHeight > 0.0f ? static_cast<float>(height) / windowHeight : scale;
    const auto top = static_cast<uint32_t>(std::clamp(std::ceil(topHeight * verticalScale), 0.0f, static_cast<float>(height - 1)));
    return {left, right - left, height - top, top};
}

inline bool contains(const SceneViewport& viewport, float x, float y)
{
    return x >= static_cast<float>(viewport.x) && x < static_cast<float>(viewport.x + viewport.width)
        && y >= static_cast<float>(viewport.y) && y < static_cast<float>(viewport.y + viewport.height);
}

} // namespace woby
