#pragma once
#include "annotation_work.h"
#include "control_scene.h"

namespace woby {
struct AnnotationCommandResult {
    AnnotationGeometry geometry;
    std::vector<SceneObjectId> targets;
    size_t projectedTriangles = 0, projectedVertices = 0;
    double snapshotMilliseconds = 0, projectionMilliseconds = 0, exactMilliseconds = 0;
};
struct AnnotationCommand {
    ControlOperation operation;
    AnnotationWorkIdentity identity;
    std::shared_ptr<AnnotationWork> work;
    std::shared_ptr<AnnotationCommandResult> result;
};
[[nodiscard]] AnnotationCommand prepareAnnotationCommand(const UiState& state, const ControlOperation& operation);
[[nodiscard]] nlohmann::json publishAnnotationCommand(UiState& state, const SceneDocument& cleanDocument,
    const AnnotationCommand& command, const ObjectIdFormatter& formatId);
} // namespace woby
