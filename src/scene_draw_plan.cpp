#include "scene_draw_plan.h"
#include "scene_pick.h"
#include "scene_queries.h"

namespace woby {

static SceneDrawPlan buildSceneDrawPlan(const UiState& state, SceneQueryRuntime* queries)
{
    SceneDrawPlan plan;
    plan.triangleEdgeXray = state.triangleEdgeXray;
    std::vector<ScenePickPart> parts;
    if (queries) { resolveSceneParts(*queries, state, parts); } else { scenePickParts(state, parts); }
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

SceneDrawPlan buildSceneDrawPlan(const UiState& state) { return buildSceneDrawPlan(state, nullptr); }

bool updateSceneDrawPlan(SceneDrawCache& cache, const UiState& state, SceneQueryRuntime* queries)
{
    const auto& revision = state.revisions;
    if (cache.owner == &state && cache.valid && cache.generation == state.sceneGeneration
        && cache.geometry == revision.geometry && cache.appearance == revision.appearance
        && cache.visibility == revision.visibility) { return false; }
    cache.plan = buildSceneDrawPlan(state, queries);
    cache.owner = &state;
    cache.generation = state.sceneGeneration; cache.geometry = revision.geometry;
    cache.appearance = revision.appearance; cache.visibility = revision.visibility;
    cache.valid = true;
    return true;
}

} // namespace woby
