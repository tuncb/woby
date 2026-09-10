#pragma once

#include "scene_pick.h"

namespace woby {

using DimensionPoint = std::array<double, 3>;

struct SceneDimensions {
    // Corner bits select max along each axis, as in the selection boxes.
    std::array<DimensionPoint, 8> corners{};
    DimensionPoint lengths{};
    bool objectAxes = false;
};

// Single visible part: its local box transformed into the scene. Multiple
// visible parts: tight world-axis bounds of their transformed mesh vertices.
[[nodiscard]] std::optional<SceneDimensions> sceneDimensions(std::span<const ScenePickPart> parts);

struct DimensionPartKey {
    const Vertex* vertices = nullptr;
    const uint32_t* indices = nullptr;
    size_t vertexCount = 0, indexCount = 0, begin = 0, count = 0;
    PickMatrix model{};
    friend bool operator==(const DimensionPartKey&, const DimensionPartKey&) = default;
};

// Runtime cache; no borrowed geometry is dereferenced after replacement.
struct SceneDimensionsCache {
    uint64_t generation = 0, revision = 0;
    std::vector<DimensionPartKey> keys;
    std::optional<SceneDimensions> dimensions;
};
const std::optional<SceneDimensions>& updateSceneDimensions(SceneDimensionsCache& cache,
    std::span<const ScenePickPart> parts, uint64_t generation, uint64_t revision);

struct SceneGrid {
    float spacing = 1, extent = 10;
    int radius = 10;
};
// Shared by grid geometry and labels so their values cannot diverge.
[[nodiscard]] SceneGrid sceneGrid(const Bounds& bounds, SceneUpAxis upAxis);
[[nodiscard]] std::optional<PickPoint> projectDimensionPoint(const DimensionPoint& point, const ScenePickView& view);

} // namespace woby
