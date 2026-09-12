#pragma once

#include "control_protocol.h"
#include "ui_state.h"
#include "mesh_comparison.h"
#include <functional>

namespace woby {
using ObjectIdFormatter = std::function<std::string(SceneObjectId)>;
nlohmann::json controlSceneInfo(const UiState& state);
nlohmann::json controlCameraInfo(const UiState& state);
nlohmann::json controlSceneTree(const UiState& state, const ObjectIdFormatter& formatId);
nlohmann::json controlObjectDetails(const UiState& state, SceneObjectId id, const ObjectIdFormatter& formatId);
nlohmann::json controlAnnotationDetails(const UiState& state, const UiAnnotation& item);
nlohmann::json applyControlAnnotationOperation(UiState& state, const SceneDocument& cleanDocument,
    const ControlOperation& command, const ObjectIdFormatter& formatId);
// Summarizes an immutable measurement snapshot; no renderer or timing dependencies.
nlohmann::json controlComparisonResults(const MeshComparison& result, double tolerance);
// Main-thread adapter for logical operations; runtime/file operations are dispatched elsewhere.
nlohmann::json applyControlSceneOperation(UiState& state, const SceneDocument& cleanDocument,
    const ControlOperation& command, const ObjectIdFormatter& formatId, float minPaneWidth, float maxPaneWidth);
} // namespace woby
