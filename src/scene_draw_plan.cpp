#include "scene_draw_plan.h"
#include "scene_pick.h"

namespace woby {

SceneDrawPlan buildSceneDrawPlan(const UiState& state)
{
    SceneDrawPlan plan;
    plan.triangleEdgeXray = state.triangleEdgeXray;
    const auto parts = scenePickParts(state);
    plan.items.reserve(parts.size());
    for (const auto& part : parts) {
        const auto& file = state.files[part.fileIndex];
        const auto& group = file.groupSettings[part.groupIndex];
        const auto& node = file.mesh.nodes[part.groupIndex];
        SceneDrawItem item;
        item.fileIndex = part.fileIndex; item.groupIndex = part.groupIndex;
        item.fileId = file.objectId; item.model = part.model;
        item.color = group.color; item.color[3] = part.opacity;
        item.uvGrid = uvColorParameters(group.uvGrid, node.hasTexcoords, false);
        item.pointSize = part.pointSize; item.lineWidth = group.lines.width;
        item.lineIndexOffset = node.lineIndexOffset; item.lineIndexCount = node.lineIndexCount;
        item.solid = part.solid; item.edges = part.edges; item.points = part.vertices;
        item.importedLines = node.lineIndexCount != 0;
        item.lineDepthTest = group.lines.depthTest;
        plan.items.push_back(item);
    }
    return plan;
}

bool updateSceneDrawPlan(SceneDrawCache& cache, const UiState& state)
{
    const auto& revision = state.revisions;
    if (cache.valid && cache.generation == state.sceneGeneration
        && cache.geometry == revision.geometry && cache.appearance == revision.appearance
        && cache.visibility == revision.visibility) { return false; }
    cache.plan = buildSceneDrawPlan(state);
    cache.generation = state.sceneGeneration; cache.geometry = revision.geometry;
    cache.appearance = revision.appearance; cache.visibility = revision.visibility;
    cache.valid = true;
    return true;
}

} // namespace woby
