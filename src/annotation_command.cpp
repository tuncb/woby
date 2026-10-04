#include "annotation_command.h"
#include "ui_operations.h"
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <chrono>

namespace woby {
AnnotationCommand prepareAnnotationCommand(const UiState& state, const ControlOperation& operation)
{
    const auto started = std::chrono::steady_clock::now();
    if (!annotationControlReady(state, operation.action)) { throw std::runtime_error("Annotation geometry is still being prepared."); }
    AnnotationCommand command;
    command.operation = operation; command.identity = annotationWorkIdentity(state);
    command.work = std::make_shared<AnnotationWork>(); command.result = std::make_shared<AnnotationCommandResult>();
    const bool creating = operation.action == ControlAction::annotationCreate;
    ScenePickView view;
    UiAnnotation item;
    std::vector<SceneObjectId> targets;
    AnnotationShape shape;
    std::array<float, 2> start{}, end{};
    if (creating) {
        const auto target = findSceneObject(state, operation.objectId);
        if (!target || target->kind != SceneObjectKind::group) { throw std::invalid_argument("Annotation creation requires a model group ID."); }
        view = scenePickView(state.camera, state.upAxis, state.sceneBounds, 1000, 1000, true, 1);
        const float aspect = operation.aspect.value_or(1);
        const auto depth = cameraDepthRange(state.camera, state.sceneBounds, state.upAxis);
        bx::mtxProj(view.projection.data(), cameraViewportFov(state.camera, aspect), aspect, depth.nearPlane, depth.farPlane, true);
        targets = annotationGroupTargets(state, operation.objectId);
        shape = *operation.shape == "line" ? AnnotationShape::line : AnnotationShape::rectangle;
        start = *operation.start; end = *operation.end;
    } else {
        if (operation.action != ControlAction::annotationMove && operation.action != ControlAction::annotationReshape) {
            throw std::invalid_argument("This command does not project annotation geometry.");
        }
        const auto* selected = findAnnotation(state, operation.objectId);
        if (!selected || selected->settings.locked || !selected->targetValid) {
            throw std::invalid_argument("The annotation is locked or its source is unavailable.");
        }
        item = *selected; shape = item.geometry.shape;
        start = operation.start.value_or(item.geometry.start); end = operation.end.value_or(item.geometry.end);
        if (operation.delta) {
            for (size_t axis = 0; axis < 2; ++axis) { start[axis] += (*operation.delta)[axis]; end[axis] += (*operation.delta)[axis]; }
        }
    }
    auto sources = snapshotAnnotationParts(scenePickParts(state));
    command.result->snapshotMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    command.work->compute = [sources = std::move(sources), view, item = std::move(item), targets = std::move(targets),
        creating, id = operation.objectId, shape, start, end, result = command.result](std::stop_token stop) {
        const auto begin = std::chrono::steady_clock::now();
        auto projection = creating
            ? annotationProjection(sources.parts, view, id, targets, annotationRegion(start, end), stop)
            : annotationEditProjection(sources.parts, item, annotationRegion(start, end), stop);
        const auto projected = std::chrono::steady_clock::now();
        result->projectedTriangles = projection.triangles.size(); result->projectedVertices = projection.vertices.size();
        result->geometry = projectAnnotation(projection, shape, start, end);
        result->projectionMilliseconds = std::chrono::duration<double, std::milli>(projected - begin).count();
        result->exactMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - projected).count();
        result->targets = std::move(projection.targetIds);
    };
    return command;
}
nlohmann::json publishAnnotationCommand(UiState& state, const SceneDocument& cleanDocument,
    const AnnotationCommand& command, const ObjectIdFormatter& formatId)
{
    if (command.work->cancel.stop_requested() || !annotationWorkCurrent(command.identity, state, true)) {
        throw std::runtime_error("Annotation canceled because the scene, view, or selection changed.");
    }
    if (!command.work->done.load(std::memory_order_acquire)) { throw std::runtime_error("Annotation computation is pending."); }
    if (!command.work->error.empty()) { throw std::runtime_error(command.work->error); }
    return applyControlAnnotationOperation(state, cleanDocument, command.operation, formatId, command.result.get());
}
} // namespace woby
