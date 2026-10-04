#pragma once
#include "scene_inspector_queries.h"

namespace woby {
void drawSceneInspectorSnapshot(const SceneInspectorSnapshot& snapshot, std::vector<InspectorEdit>& edits);
// Preparation / drawing / operation boundary. Drawing itself only reads the snapshot.
void drawSceneInspector(UiState& state, SceneInspectorRuntime& runtime);
} // namespace woby
