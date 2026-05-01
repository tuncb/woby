#pragma once

#include "camera.h"
#include "scene_renderer.h"
#include "ui_state.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace woby {

struct MousePosition {
    float x = 0.0f;
    float y = 0.0f;
};

struct HoveredVertex {
    std::array<float, 3> localPosition{};
    std::array<float, 3> transformedPosition{};
    float depth = 0.0f;
    float distanceSquared = 0.0f;
};

struct HoverPickCache {
    bool valid = false;
    uint64_t signature = 0u;
    std::optional<HoveredVertex> hoveredVertex;
};

[[nodiscard]] uint64_t hoverPickSignature(
    const std::vector<UiFileState>& files,
    const std::vector<LoadedModelRuntime>& runtimes,
    const MousePosition& mouse,
    bool mouseInsideViewport,
    float masterVertexPointSize,
    const SceneCamera& camera,
    SceneUpAxis upAxis,
    const Bounds& sceneBounds,
    uint32_t viewportWidth,
    uint32_t viewportHeight,
    bool homogeneousDepth);

[[nodiscard]] std::optional<HoveredVertex> findHoveredVertex(
    const std::vector<UiFileState>& files,
    const std::vector<LoadedModelRuntime>& runtimes,
    const MousePosition& mouse,
    float masterVertexPointSize,
    const float* view,
    const float* projection,
    uint32_t viewportWidth,
    uint32_t viewportHeight,
    bool homogeneousDepth);

} // namespace woby
