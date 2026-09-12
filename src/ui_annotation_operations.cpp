#include "ui_operations.h"
#include "surface_annotation.h"
#include "utf8_path.h"

#include <algorithm>
#include <stdexcept>

namespace woby {
const UiAnnotation* findAnnotation(const UiState& state, SceneObjectId id)
{
    const auto it = std::find_if(state.annotations.begin(), state.annotations.end(),
        [id](const auto& item) { return item.objectId == id; });
    return it == state.annotations.end() ? nullptr : &*it;
}
const UiAnnotation* selectedAnnotation(const UiState& state)
{
    return state.selectedSceneObjects.size() == 1 ? findAnnotation(state, state.selectedSceneObjects.front()) : nullptr;
}
std::vector<std::array<float, 3>> annotationVertices(const UiState& state, const UiAnnotation& item)
{
    if (!item.targetValid) { return {}; }
    // Resolve the source independently of visibility, opacity, and selection.
    for (const auto& file : state.files) {
        for (size_t g = 0; g < file.groupSettings.size() && g < file.mesh.nodes.size(); ++g) {
            if (file.groupSettings[g].objectId == item.targetId) {
                return annotationControlPositions(file.mesh, file.mesh.nodes[g].indexOffset, item.geometry);
            }
        }
    }
    return {};
}
void validateAnnotationTargets(UiState& state)
{
    for (auto& item : state.annotations) {
        item.targetValid = false;
        for (const auto& file : state.files) {
            for (size_t g = 0; g < file.groupSettings.size() && g < file.mesh.nodes.size(); ++g) {
                if (file.groupSettings[g].objectId != item.targetId) { continue; }
                const auto& node = file.mesh.nodes[g];
                item.targetValid = item.geometry.fingerprint == annotationFingerprint(file.mesh, node.indexOffset, node.indexCount)
                    && std::all_of(item.geometry.segments.begin(), item.geometry.segments.end(),
                        [&](const auto& segment) { return segment.triangle < node.indexCount / 3; });
            }
        }
    }
}
SceneObjectId createAnnotation(UiState& state, SceneObjectId target, AnnotationGeometry geometry)
{
    validateAnnotationGeometry(geometry);
    const auto object = findSceneObject(state, target);
    if (!object || object->kind != SceneObjectKind::group) { throw std::runtime_error("Choose a model part for the annotation."); }
    UiAnnotation item;
    item.targetId = target;
    item.targetName = object->name;
    if (const auto file = findSceneObject(state, object->fileId)) { item.targetName = file->name + " / " + item.targetName; }
    item.settings.name = (geometry.shape == AnnotationShape::line ? "Surface line " : "Surface rectangle ") + std::to_string(state.annotations.size() + 1);
    item.geometry = std::move(geometry);
    // Validate before mutating the scene, including untrusted operation callers.
    bool valid = false;
    for (const auto& file : state.files) {
        for (size_t g = 0; g < file.groupSettings.size() && g < file.mesh.nodes.size(); ++g) {
            if (file.groupSettings[g].objectId != target) { continue; }
            const auto& node = file.mesh.nodes[g];
            valid = item.geometry.fingerprint == annotationFingerprint(file.mesh, node.indexOffset, node.indexCount)
                && std::all_of(item.geometry.segments.begin(), item.geometry.segments.end(),
                    [&](const auto& segment) { return segment.triangle < node.indexCount / 3; });
        }
    }
    if (!valid) { throw std::runtime_error("The annotation target has changed. Draw it again."); }
    item.targetValid = true;
    state.annotations.push_back(std::move(item));
    assignSceneObjectIds(state);
    const auto id = state.annotations.back().objectId;
    selectSceneObject(state, id);
    markSceneDirty(state);
    return id;
}
void setAnnotationSettings(UiState& state, SceneObjectId id, AnnotationSettings settings)
{
    settings = normalizedAnnotationSettings(std::move(settings));
    for (auto& item : state.annotations) {
        if (item.objectId == id && item.settings != settings) { item.settings = std::move(settings); markSceneDirty(state); return; }
    }
}
void reshapeAnnotation(UiState& state, SceneObjectId id, AnnotationGeometry geometry)
{
    validateAnnotationGeometry(geometry);
    for (auto& item : state.annotations) {
        if (item.objectId != id) { continue; }
        if (item.settings.locked || !item.targetValid || !findSceneObject(state, item.targetId)) { throw std::runtime_error("This annotation is locked or its target is unavailable."); }
        if (geometry.fingerprint != item.geometry.fingerprint || geometry.projector != item.geometry.projector) { throw std::runtime_error("Annotation attachment cannot change while reshaping."); }
        for (const auto& file : state.files) {
            for (size_t g = 0; g < file.groupSettings.size() && g < file.mesh.nodes.size(); ++g) {
                if (file.groupSettings[g].objectId == item.targetId
                    && std::any_of(geometry.segments.begin(), geometry.segments.end(), [&](const auto& s) { return s.triangle >= file.mesh.nodes[g].indexCount / 3; })) {
                    throw std::runtime_error("Invalid annotation triangle.");
                }
            }
        }
        if (item.geometry != geometry) { item.geometry = std::move(geometry); markSceneDirty(state); }
        return;
    }
}
void deleteAnnotation(UiState& state, SceneObjectId id)
{
    if (!findAnnotation(state, id)) { return; }
    std::erase_if(state.annotations, [id](const auto& item) { return item.objectId == id; });
    std::erase(state.selectedSceneObjects, id);
    markSceneDirty(state);
}
std::vector<SceneAnnotationRecord> sceneAnnotationRecords(const UiState& state)
{
    std::vector<SceneAnnotationRecord> records;
    for (const auto& item : state.annotations) {
        SceneAnnotationRecord record;
        record.targetName = item.targetName; record.settings = item.settings; record.geometry = item.geometry;
        for (size_t f = 0; f < state.files.size(); ++f) {
            for (size_t g = 0; g < state.files[f].groupSettings.size(); ++g) {
                if (state.files[f].groupSettings[g].objectId == item.targetId) {
                    record.fileIndex = static_cast<int>(f); record.groupIndex = static_cast<int>(g);
                }
            }
        }
        records.push_back(std::move(record));
    }
    return records;
}
void loadSceneAnnotations(UiState& state, const SceneDocument& document)
{
    state.annotations.clear();
    for (const auto& record : document.annotations) {
        validateAnnotationGeometry(record.geometry);
        UiAnnotation item;
        item.targetName = record.targetName;
        item.settings = normalizedAnnotationSettings(record.settings);
        item.geometry = record.geometry;
        if (record.fileIndex >= 0 && static_cast<size_t>(record.fileIndex) < state.files.size() && record.groupIndex >= 0) {
            const auto f = static_cast<size_t>(record.fileIndex), g = static_cast<size_t>(record.groupIndex);
            const auto& file = state.files[f];
            if (g < file.groupSettings.size() && g < file.mesh.nodes.size() && f < document.files.size()
                && g < document.files[f].groups.size() && file.mesh.nodes[g].name == document.files[f].groups[g].name) {
                item.targetId = file.groupSettings[g].objectId;
            }
        }
        state.annotations.push_back(std::move(item));
    }
    assignSceneObjectIds(state);
    validateAnnotationTargets(state);
}
} // namespace woby
