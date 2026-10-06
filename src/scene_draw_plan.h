#pragma once
#include "ui_state.h"

namespace woby {
struct SceneQueryRuntime;

// Owned logical draw data: no borrowed mesh pointers or backend resources.
struct SceneDrawItem {
    size_t fileIndex = 0, groupIndex = 0;
    SceneObjectId fileId = 0;
    std::array<float, 16> model{};
    std::array<float, 4> color{}, uvGrid{}; // Color alpha applies to surfaces/lines; points are opaque.
    float pointSize = 4, lineWidth = 1;
    uint32_t lineIndexOffset = 0, lineIndexCount = 0;
    bool solid = false, edges = false, points = false;
    bool importedLines = false, lineDepthTest = true;
};
struct SceneDrawPlan {
    std::vector<SceneDrawItem> items;
    bool triangleEdgeXray = false;
};
struct SceneDrawCache {
    const UiState* owner = nullptr;
    SceneDrawPlan plan;
    uint64_t generation = 0, geometry = 0, appearance = 0, visibility = 0;
    bool valid = false;
};
[[nodiscard]] SceneDrawPlan buildSceneDrawPlan(const UiState& state);
// Camera/selection changes do not rebuild geometry transforms or appearance.
// Returns true only when the owned snapshot changed.
bool updateSceneDrawPlan(SceneDrawCache& cache, const UiState& state, SceneQueryRuntime* queries = nullptr);

} // namespace woby
