#include "control_scene.h"
#include "math_utils.h"
#include "annotation_command.h"
#include "annotation_preparation.h"
#include "surface_annotation.h"
#include "cross_section.h"
#include "ui_operations.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <stdexcept>

namespace woby {
using Json = nlohmann::json;
namespace {
bool hasSectionOptions(const ControlOperation& c)
{
    return c.planeStart || c.planeEnd || c.sectionUpAxis || c.lift || c.showPlane || c.showConnections
        || c.showDifferences || c.faceOn || c.sourceIndex || c.sourceVisible || c.referenceIndex || c.comparedIndex;
}
CrossSectionSettings sectionOptions(CrossSectionSettings s, const ControlOperation& c, size_t sourceCount)
{
    if (c.planeStart) { s.a = *c.planeStart; }
    if (c.planeEnd) { s.b = *c.planeEnd; }
    if (c.sectionUpAxis) { s.upAxis = *c.sectionUpAxis == "z" ? SceneUpAxis::z : SceneUpAxis::y; }
    if (c.lift) { s.lift = *c.lift; }
    if (c.showPlane) { s.showPlane = *c.showPlane; }
    if (c.showConnections) { s.showConnections = *c.showConnections; }
    if (c.showDifferences) { s.showDifferences = *c.showDifferences; }
    if (c.faceOn) { s.faceOn = *c.faceOn; }
    for (const auto index : {c.sourceIndex, c.referenceIndex, c.comparedIndex}) {
        if (index && *index >= sourceCount) { throw std::invalid_argument("Section index is outside sourceIds. Indices start at zero."); }
    }
    if (c.referenceIndex) { s.reference = static_cast<int>(*c.referenceIndex); }
    if (c.comparedIndex) { s.compared = static_cast<int>(*c.comparedIndex); }
    s.sourceVisible.resize(sourceCount, true);
    if (c.sourceIndex) { s.sourceVisible[static_cast<size_t>(*c.sourceIndex)] = *c.sourceVisible; }
    return normalizedCrossSectionSettings(std::move(s), sourceCount);
}
}

bool annotationControlReady(const UiState& state, ControlAction action)
{
    using A = ControlAction;
    const bool needsPreparation = action == A::annotationCreate || action == A::annotationMove
        || action == A::annotationReshape || action == A::annotationGet
        || (action == A::annotationList && !state.annotations.empty());
    return !needsPreparation || annotationPreparationReady(state);
}

Json controlAnnotationDetails(const UiState& state, const UiAnnotation& item)
{
    const auto& settings = item.settings;
    if (item.text) {
        const auto& t = *item.text;
        return {{"shape", "text"}, {"settings", {{"name", settings.name}, {"comments", settings.note}, {"visible", settings.visible},
            {"locked", settings.locked}, {"color", settings.color}}}, {"text", t.text}, {"position", t.position},
            {"size", t.size}, {"faceCamera", t.faceCamera}, {"rotationDegrees", t.rotationDegrees}, {"controlSpace", "world"},
            {"targetValid", true}, {"targetPending", false}, {"effectiveVisible", settings.visible && settings.color[3] > 0 && !t.text.empty()}};
    }
    const auto parts = scenePickParts(state);
    if (item.section) {
        const auto& s = *item.section;
        return {{"shape", "section"}, {"settings", {{"name", settings.name}, {"comments", settings.note}, {"visible", settings.visible},
            {"locked", settings.locked}, {"width", settings.width}, {"color", settings.color}}}, {"start", s.a}, {"end", s.b}, {"controlSpace", "world"},
            {"lift", s.lift}, {"upAxis", s.upAxis == SceneUpAxis::z ? "z" : "y"}, {"showPlane", s.showPlane},
            {"showConnections", s.showConnections}, {"showDifferences", s.showDifferences}, {"faceOn", s.faceOn},
            {"sourceVisible", s.sourceVisible}, {"referenceIndex", s.reference}, {"comparedIndex", s.compared},
            {"targetValid", item.targetValid}, {"targetPending", false}, {"effectiveVisible", settings.visible},
            {"vertices", annotationOriginalVertices(state, item)}, {"vertexSpace", "world"}};
    }
    return {{"shape", item.geometry->shape == AnnotationShape::line ? "line" : "rectangle"},
        {"settings", {{"name", settings.name}, {"comments", settings.note}, {"visible", settings.visible},
            {"locked", settings.locked}, {"width", settings.width}, {"color", settings.color}}},
        {"sourceName", item.targetName}, {"targetValid", item.targetValid && findSceneObject(state, item.targetId).has_value()},
        {"targetPending", item.targetPending},
        {"effectiveVisible", !annotationWorldLines(item, parts).empty()},
        {"vertices", annotationOriginalVertices(state, item)}, {"vertexSpace", item.targetIds.empty() ? "model" : "world"},
        {"start", item.geometry->start}, {"end", item.geometry->end}, {"controlSpace", "original-projector-ndc"},
        {"projector", item.geometry->projector}, {"homogeneousDepth", item.geometry->homogeneousDepth},
        {"segmentCount", item.geometry->segments.size()}};
}

Json applyControlAnnotationOperation(UiState& state, const SceneDocument& cleanDocument,
    const ControlOperation& command, const ObjectIdFormatter& formatId, const AnnotationCommandResult* computed)
{
    using A = ControlAction;
    const auto* existing = findAnnotation(state, command.objectId);
    const bool textCommand = (command.action == A::annotationCreate && command.shape == "text") || (existing && existing->text);
    if (!textCommand && !annotationControlReady(state, command.action)) {
        throw std::runtime_error("Annotation actions are disabled while annotation data is being prepared. Retry when annotationReady is true.");
    }
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
    if (command.action == A::annotationCreate && command.shape == "text") {
        TextAnnotationSettings t;
        t.text = *command.text; t.position = *command.position;
        if (command.size) { t.size = *command.size; }
        if (command.faceCamera) { t.faceCamera = *command.faceCamera; }
        if (command.rotationDegrees) { t.rotationDegrees = *command.rotationDegrees; }
        const auto selection = state.selectedSceneObjects;
        id = createTextAnnotation(state, std::move(t));
        clearSceneSelection(state);
        for (const auto selected : selection) { selectSceneObject(state, selected, true); }
    } else if (command.action == A::annotationCreate && command.shape == "section") {
        std::vector<SceneObjectId> sources;
        if (command.target == "scene") {
            for (const auto& part : scenePickParts(state, true)) {
                if (part.mesh && part.indexCount) { sources.push_back(part.objectId); }
            }
        } else {
            const auto target = findSceneObject(state, id);
            if (!target || (target->kind != SceneObjectKind::group && target->kind != SceneObjectKind::file && target->kind != SceneObjectKind::folder)) {
                throw std::invalid_argument("Section creation requires scene, a folder, file, or model group ID.");
            }
            sources = comparisonObjectParts(state, {id});
        }
        CrossSectionSettings s;
        s.upAxis = state.upAxis;
        s.sourceVisible = crossSectionSourceVisibility(state, sources);
        s = sectionOptions(std::move(s), command, sources.size());
        const auto selection = state.selectedSceneObjects;
        id = createCrossSection(state, std::move(s), std::move(sources));
        clearSceneSelection(state);
        for (const auto selected : selection) { selectSceneObject(state, selected, true); }
    } else if (command.action == A::annotationCreate) {
        const auto target = findSceneObject(state, id);
        if (!target || target->kind != SceneObjectKind::group) { throw std::invalid_argument("Annotation creation requires a model group ID."); }
        // A fixed authoring aspect makes scripts independent of panel/window sizes.
        auto view = scenePickView(state.camera, state.upAxis, state.sceneBounds, 1000, 1000, true, 1);
        const float aspect = command.aspect.value_or(1);
        const auto depth = cameraDepthRange(state.camera, state.sceneBounds, state.upAxis);
        woby::perspectiveMatrix(view.projection.data(), cameraViewportFov(state.camera, aspect), aspect,
            depth.nearPlane, depth.farPlane, true);
        AnnotationProjection projection;
        if (!computed) { projection = annotationProjection(scenePickParts(state), view, id, annotationGroupTargets(state, id), annotationRegion(*command.start, *command.end)); }
        auto geometry = computed ? computed->geometry : shareAnnotationGeometry(projectAnnotation(projection,
            *command.shape == "line" ? AnnotationShape::line : AnnotationShape::rectangle, *command.start, *command.end));
        // CLI commands name their target explicitly; preserve the user's selection.
        const auto selection = state.selectedSceneObjects;
        id = createAnnotation(state, id, std::move(geometry), computed ? computed->targets : projection.targetIds);
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
            if (item->text) { throw std::invalid_argument("Use annotation set --position X Y Z to move text."); }
            if (item->section) { throw std::invalid_argument("Use annotation set --plane-start/--plane-end to change a cross section."); }
            if (item->settings.locked || !item->targetValid) { throw std::invalid_argument("The annotation is locked or its source is unavailable."); }
            const auto parts = scenePickParts(state);
            const auto target = std::find_if(parts.begin(), parts.end(), [&](const auto& part) { return part.objectId == item->targetId; });
            if (target == parts.end()) { throw std::invalid_argument("Make the annotation source model visible before editing its geometry."); }
            auto start = command.start.value_or(item->geometry->start), end = command.end.value_or(item->geometry->end);
            if (command.delta) {
                for (size_t axis = 0; axis < 2; ++axis) { start[axis] += (*command.delta)[axis]; end[axis] += (*command.delta)[axis]; }
            }
            if (computed) { reshapeAnnotation(state, id, computed->geometry); }
            else {
                const auto projection = annotationEditProjection(parts, *item, annotationRegion(start, end));
                reshapeAnnotation(state, id, projectAnnotation(projection, item->geometry->shape, start, end));
            }
        } else if (command.action != A::annotationSet && command.action != A::visibility
            && command.action != A::colorSet && command.action != A::colorReset && command.action != A::opacity) {
            throw std::invalid_argument("Use annotation get, set, reshape, move, or delete for annotation objects.");
        }
    }
    if (command.action == A::annotationSet && (command.text || command.position || command.size || command.faceCamera || command.rotationDegrees)) {
        const auto* item = findAnnotation(state, id);
        if (!item->text) { throw std::invalid_argument("Text options require a text annotation."); }
        if (hasSectionOptions(command) || command.width) { throw std::invalid_argument("Section and line width options do not apply to text."); }
        auto t = *item->text;
        if (command.text) { t.text = *command.text; }
        if (command.position) { t.position = *command.position; }
        if (command.size) { t.size = *command.size; }
        if (command.faceCamera) { t.faceCamera = *command.faceCamera; }
        if (command.rotationDegrees) { t.rotationDegrees = *command.rotationDegrees; }
        if (item->settings.locked && (t.position != item->text->position || t.rotationDegrees != item->text->rotationDegrees)) {
            throw std::invalid_argument("Unlock the text before changing its position or rotation.");
        }
        setTextAnnotationSettings(state, id, std::move(t));
    }
    if (findAnnotation(state, id)->text && command.width) { throw std::invalid_argument("Use --size to change text size."); }
    if (command.action == A::annotationSet && hasSectionOptions(command)) {
        const auto* item = findAnnotation(state, id);
        if (!item->section) { throw std::invalid_argument("Section options require a cross-section annotation."); }
        auto section = sectionOptions(*item->section, command, item->targetIds.size());
        if (item->settings.locked && (section.a != item->section->a || section.b != item->section->b || section.upAxis != item->section->upAxis)) {
            throw std::invalid_argument("Unlock the section before changing its plane.");
        }
        setCrossSectionSettings(state, id, std::move(section));
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
