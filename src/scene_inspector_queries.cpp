#include "scene_inspector_queries.h"
#include "utf8_path.h"
#include <boost/unordered/unordered_flat_map.hpp>

namespace woby {

UiPropertyValue inspectorProperty(const SceneInspectorSnapshot& snapshot, UiObjectProperty property)
{
    return snapshot.properties.at(static_cast<size_t>(property));
}

void updateSceneInspector(SceneInspectorRuntime& runtime, const UiState& state)
{
    auto& queries = runtime.sharedQueries ? *runtime.sharedQueries : runtime.localQueries;
    updateSceneSelectionQueries(queries, state, runtime.comparisons);
    const bool dimensionsChanged = runtime.queryOwner != &queries || runtime.dimensionBuilds != queries.selection.dimensionBuilds;
    // Compare before copying selection: unchanged frames allocate nothing.
    if (!dimensionsChanged && runtime.owner == &state && runtime.key && runtime.key->generation == state.sceneGeneration
        && runtime.key->revision == state.sceneEditRevision && runtime.key->selection == state.selectedSceneObjects) { return; }
    SceneInspectorSnapshot next;
    next.properties = selectedObjectProperties(state);
    next.visibility = selectedObjectVisibility(state);
    const auto objects = sceneObjects(state);
    boost::unordered_flat_map<SceneObjectId, const SceneObjectInfo*> byId;
    byId.reserve(objects.size());
    for (const auto& object : objects) { byId.emplace(object.id, &object); }
    for (const auto id : state.selectedSceneObjects) {
        next.selectionId += std::to_string(id) + ":";
        if (const auto found = byId.find(id); found != byId.end()) {
            const auto* object = found->second;
            InspectorTargetLabel label{object->kind, object->name, {}, pathToUtf8(object->path)};
            if (object->kind == SceneObjectKind::group) {
                if (const auto parent = byId.find(object->fileId); parent != byId.end()) {
                    label.fileName = parent->second->name;
                    label.path = pathToUtf8(parent->second->path);
                }
            }
            next.targets.push_back(std::move(label));
        }
    }
    if (state.selectedSceneObjects.size() == 1) {
        const auto id = state.selectedSceneObjects.front();
        for (const auto& file : state.files) {
            if (file.objectId == id) {
                next.statistics.push_back(std::to_string(file.groupSettings.size()) + " parts | "
                    + std::to_string(file.mesh.vertices.size()) + " vertices | " + std::to_string(file.mesh.indices.size() / 3) + " triangles");
                if (!file.mesh.lineIndices.empty()) { next.statistics.push_back(std::to_string(file.mesh.lineIndices.size() / 2) + " line segments"); }
                if (!file.mesh.pointIndices.empty()) { next.statistics.push_back(std::to_string(file.mesh.pointIndices.size()) + " points"); }
                next.bounds = originalMeshBounds(file.mesh);
                break;
            }
            for (size_t i = 0; i < file.groupSettings.size() && i < file.mesh.nodes.size(); ++i) {
                if (file.groupSettings[i].objectId != id) { continue; }
                const auto& node = file.mesh.nodes[i];
                next.statistics.push_back(node.pointIndexCount ? std::to_string(node.pointIndexCount) + " points"
                    : node.lineIndexCount ? std::to_string(node.lineIndexCount / 2) + " line segments"
                    : std::to_string(node.indexCount / 3) + " triangles");
                if (file.groupSettings[i].localBoundsValid) {
                    next.bounds = file.groupSettings[i].originalBounds;
                    if (!next.bounds) { next.bounds = originalMeshBounds(file.mesh, &node); }
                }
                break;
            }
        }
    }
    next.dimensions = queries.selection.dimensions;
    runtime.dimensionBuilds = queries.selection.dimensionBuilds;
    runtime.queryOwner = &queries;
    runtime.snapshot = std::move(next);
    runtime.owner = &state;
    runtime.key = PropertiesKey{state.sceneGeneration, state.sceneEditRevision, state.selectedSceneObjects};
    ++runtime.builds;
}

void applyInspectorEdits(UiState& state, std::span<const InspectorEdit> edits)
{
    for (const auto& edit : edits) {
        switch (edit.kind) {
        case InspectorEditKind::property: setSelectedObjectProperty(state, edit.property, edit.value); break;
        case InspectorEditKind::reset: resetSelectedObjectProperties(state, edit.group); break;
        case InspectorEditKind::visibility: setSelectedObjectsVisible(state, edit.value != 0); break;
        }
    }
}

} // namespace woby
