#include "control_scene.h"
#include "surface_annotation.h"
#include "ui_operations.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <stdexcept>

namespace woby {
using Json = nlohmann::json;

Json controlAnnotationDetails(const UiState& state, const UiAnnotation& item)
{
    const auto& settings = item.settings;
    const auto parts = scenePickParts(state);
    return {{"shape", item.geometry.shape == AnnotationShape::line ? "line" : "rectangle"},
        {"settings", {{"name", settings.name}, {"comments", settings.note}, {"visible", settings.visible},
            {"locked", settings.locked}, {"width", settings.width}, {"color", settings.color}}},
        {"sourceName", item.targetName}, {"targetValid", item.targetValid && findSceneObject(state, item.targetId).has_value()},
        {"effectiveVisible", !annotationWorldLines(item, parts).empty()},
        {"vertices", annotationVertices(state, item)}, {"vertexSpace", "model"},
        {"start", item.geometry.start}, {"end", item.geometry.end}, {"controlSpace", "original-projector-ndc"},
        {"projector", item.geometry.projector}, {"homogeneousDepth", item.geometry.homogeneousDepth},
        {"segmentCount", item.geometry.segments.size()}};
}

Json applyControlAnnotationOperation(UiState& state, const SceneDocument& cleanDocument,
    const ControlOperation& command, const ObjectIdFormatter& formatId)
{
    using A = ControlAction;
    const auto objectInfo = [&](SceneObjectId id) {
        auto result = controlObjectDetails(state, id, formatId);
        result.update({{"id", formatId(id)}, {"name", findAnnotation(state, id)->settings.name}, {"kind", "annotation"}});
        return result;
    };
    if (command.action == A::annotationList) {
        auto items = Json::array();
        for (const auto& item : state.annotations) { items.push_back(objectInfo(item.objectId)); }
        return {{"annotations", std::move(items)}};
    }
    auto id = command.objectId;
    if (command.action == A::annotationCreate) {
        const auto target = findSceneObject(state, id);
        if (!target || target->kind != SceneObjectKind::group) { throw std::invalid_argument("Annotation creation requires a model group ID."); }
        // A fixed authoring aspect makes scripts independent of panel/window sizes.
        auto view = scenePickView(state.camera, state.upAxis, state.sceneBounds, 1000, 1000, true, 1);
        const float aspect = command.aspect.value_or(1);
        bx::mtxProj(view.projection.data(), cameraViewportFov(state.camera, aspect), aspect,
            state.camera.nearPlane, cameraFarPlane(state.camera, state.sceneBounds), true);
        const auto projection = annotationProjection(scenePickParts(state), view, id);
        auto geometry = projectAnnotation(projection, *command.shape == "line" ? AnnotationShape::line : AnnotationShape::rectangle,
            *command.start, *command.end);
        // CLI commands name their target explicitly; preserve the user's selection.
        const auto selection = state.selectedSceneObjects;
        id = createAnnotation(state, id, std::move(geometry));
        clearSceneSelection(state);
        for (const auto selected : selection) { selectSceneObject(state, selected, true); }
    } else {
        const auto* item = findAnnotation(state, id);
        if (!item) { throw std::invalid_argument("This command requires an annotation ID."); }
        if (command.action == A::annotationGet) { return {{"object", objectInfo(id)}}; }
        if (command.action == A::annotationDelete) {
            deleteAnnotation(state, id); updateSceneDirty(state, cleanDocument);
            return {{"removed", formatId(id)}, {"dirty", state.isDirty}};
        }
        if (command.action == A::annotationMove || command.action == A::annotationReshape) {
            if (item->settings.locked || !item->targetValid) { throw std::invalid_argument("The annotation is locked or its source is unavailable."); }
            const auto parts = scenePickParts(state);
            const auto target = std::find_if(parts.begin(), parts.end(), [&](const auto& part) { return part.objectId == item->targetId; });
            if (target == parts.end()) { throw std::invalid_argument("Make the annotation source model visible before editing its geometry."); }
            auto start = command.start.value_or(item->geometry.start), end = command.end.value_or(item->geometry.end);
            if (command.delta) {
                for (size_t axis = 0; axis < 2; ++axis) { start[axis] += (*command.delta)[axis]; end[axis] += (*command.delta)[axis]; }
            }
            const auto projection = annotationEditProjection(*target, item->geometry);
            reshapeAnnotation(state, id, projectAnnotation(projection, item->geometry.shape, start, end));
        } else if (command.action != A::annotationSet && command.action != A::visibility
            && command.action != A::colorSet && command.action != A::colorReset && command.action != A::opacity) {
            throw std::invalid_argument("Use annotation get, set, reshape, move, or delete for annotation objects.");
        }
    }
    auto settings = findAnnotation(state, id)->settings;
    if (command.name) { settings.name = *command.name; }
    if (command.comments) { settings.note = *command.comments; }
    if (command.visible) { settings.visible = *command.visible; }
    if (command.locked) { settings.locked = *command.locked; }
    if (command.width) { settings.width = *command.width; }
    if (command.rgb) { std::copy(command.rgb->begin(), command.rgb->end(), settings.color.begin()); }
    if (command.action == A::colorReset) {
        const AnnotationSettings defaults;
        std::copy_n(defaults.color.begin(), 3, settings.color.begin());
    }
    if (command.opacity) { settings.color[3] = *command.opacity; }
    if (command.action == A::opacity) { settings.color[3] = *command.value; }
    setAnnotationSettings(state, id, std::move(settings));
    updateSceneDirty(state, cleanDocument);
    auto result = Json{{"target", formatId(id)}, {"object", objectInfo(id)}, {"dirty", state.isDirty}};
    if (command.action == A::visibility || command.action == A::colorSet
        || command.action == A::colorReset || command.action == A::opacity) {
        result["applied"] = result["object"]["settings"];
        result["bounds"] = controlSceneInfo(state)["bounds"];
    }
    return result;
}
} // namespace woby
