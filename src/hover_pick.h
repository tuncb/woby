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
    uint64_t sceneSignature = 0, sceneBuilds = 0;
};

// Revisions replace hierarchy/group hashing. Resource owners remain independent
// and are checked live; no mesh vertices or hierarchy nodes are visited on hits.
[[nodiscard]] uint64_t hoverSceneSignature(const UiState& state,
    const std::vector<LoadedModelRuntime>& runtimes);
[[nodiscard]] uint64_t hoverPickSignature(HoverPickCache& cache, const UiState& state,
    const std::vector<LoadedModelRuntime>& runtimes, MousePosition mouse, bool inside,
    uint32_t width, uint32_t height, bool homogeneousDepth);

// Runtime debounce state, outside the persisted logical scene.
struct HoverNavigationState {
    SceneCamera camera;
    SceneUpAxis upAxis = SceneUpAxis::z;
    double resumeAtSeconds = 0;
};

[[nodiscard]] bool hoverNavigationPaused(HoverNavigationState& state, const SceneCamera& camera,
    SceneUpAxis upAxis, const CameraInput& input, double nowSeconds);

[[nodiscard]] uint64_t hoverPickSignature(
    const std::vector<UiFileState>& files,
    const std::vector<UiSceneNode>& sceneNodes,
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
    const UiState& state, std::span<const ScenePickPart> parts, const std::vector<LoadedModelRuntime>& runtimes,
    MousePosition mouse, const ScenePickView& view);

[[nodiscard]] std::optional<HoveredVertex> findHoveredVertex(
    const std::vector<UiFileState>& files,
    const std::vector<UiSceneNode>& sceneNodes,
    const std::vector<LoadedModelRuntime>& runtimes,
    const MousePosition& mouse,
    float masterVertexPointSize,
    const float* view,
    const float* projection,
    uint32_t viewportWidth,
    uint32_t viewportHeight,
    bool homogeneousDepth);

} // namespace woby
