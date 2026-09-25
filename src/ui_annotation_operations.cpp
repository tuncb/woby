#include "ui_operations.h"
#include "surface_annotation.h"
#include "utf8_path.h"

#include <algorithm>
#include <stdexcept>

namespace woby {
namespace {
bool validAttachments(const UiState& state, const UiAnnotation& item)
{
    const auto count = std::max(size_t{1}, item.targetIds.size());
    if ((!item.geometry.sources.empty() && item.geometry.sources.size() != count)
        || (item.geometry.sources.empty() != item.targetIds.empty())
        || (!item.targetIds.empty() && item.targetIds.front() != item.targetId)) { return false; }
    for (size_t i = 0; i < count; ++i) {
        const auto id = item.targetIds.empty() ? item.targetId : item.targetIds[i];
        bool found = false;
        for (const auto& file : state.files) {
            for (size_t g = 0; g < file.groupSettings.size() && g < file.mesh.nodes.size(); ++g) {
                if (!id || file.groupSettings[g].objectId != id) { continue; }
                const auto& node = file.mesh.nodes[g];
                const auto& fingerprint = item.geometry.sources.empty() ? item.geometry.fingerprint : item.geometry.sources[i].fingerprint;
                if (fingerprint != annotationFingerprint(file.mesh, node.indexOffset, node.indexCount)) { return false; }
                for (const auto& segment : item.geometry.segments) {
                    if ((segment.source == i && segment.triangle >= node.indexCount / 3)
                        || (segment.endSource.value_or(segment.source) == i
                            && segment.endTriangle.value_or(segment.triangle) >= node.indexCount / 3)) { return false; }
                }
                found = true;
            }
        }
        if (!found) { return false; }
    }
    return true;
}
SceneAnnotationTarget savedTarget(const UiState& state, SceneObjectId id)
{
    for (size_t f = 0; f < state.files.size(); ++f) {
        for (size_t g = 0; g < state.files[f].groupSettings.size(); ++g) {
            if (state.files[f].groupSettings[g].objectId == id) { return {static_cast<int>(f), static_cast<int>(g)}; }
        }
    }
    return {};
}
SceneObjectId loadedTarget(const UiState& state, const SceneDocument& document, SceneAnnotationTarget target)
{
    if (target.fileIndex < 0 || target.groupIndex < 0) { return 0; }
    const auto f = static_cast<size_t>(target.fileIndex), g = static_cast<size_t>(target.groupIndex);
    if (f >= state.files.size() || f >= document.files.size()) { return 0; }
    const auto& file = state.files[f];
    if (g >= file.groupSettings.size() || g >= file.mesh.nodes.size() || g >= document.files[f].groups.size()
        || file.mesh.nodes[g].name != document.files[f].groups[g].name) { return 0; }
    return file.groupSettings[g].objectId;
}
} // namespace
std::vector<SceneObjectId> annotationGroupTargets(const UiState& state, SceneObjectId target)
{
    const auto object = findSceneObject(state, target);
    if (!object || object->kind != SceneObjectKind::group) { return {}; }
    // A selected parent explicitly scopes the gesture. Otherwise use the source
    // file's parts; unrelated selections never combine independent models.
    std::vector<SceneObjectId> result;
    for (const auto id : state.selectedSceneObjects) {
        const auto parent = findSceneObject(state, id);
        if (!parent || (parent->kind != SceneObjectKind::folder && parent->kind != SceneObjectKind::file)) { continue; }
        auto members = comparisonObjectParts(state, {id});
        if (std::find(members.begin(), members.end(), target) != members.end()
            && (result.empty() || members.size() < result.size())) { result = std::move(members); }
    }
    if (result.empty()) { result = comparisonObjectParts(state, {object->fileId}); }
    std::erase(result, target);
    result.insert(result.begin(), target);
    return result;
}
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
    if (!item.targetIds.empty()) { return annotationControlWorldPositions(item, scenePickParts(state, true)); }
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
    for (auto& item : state.annotations) { item.targetValid = validAttachments(state, item); }
}
SceneObjectId createAnnotation(UiState& state, SceneObjectId target, AnnotationGeometry geometry, std::vector<SceneObjectId> targets)
{
    validateAnnotationGeometry(geometry);
    const auto object = findSceneObject(state, target);
    if (!object || object->kind != SceneObjectKind::group) { throw std::runtime_error("Choose a model part for the annotation."); }
    UiAnnotation item;
    item.targetId = target;
    item.targetIds = std::move(targets);
    item.targetName = object->name;
    if (const auto file = findSceneObject(state, object->fileId)) { item.targetName = file->name + " / " + item.targetName; }
    item.settings.name = (geometry.shape == AnnotationShape::line ? "Surface line " : "Surface rectangle ") + std::to_string(state.annotations.size() + 1);
    item.geometry = std::move(geometry);
    // Validate before mutating the scene, including untrusted operation callers.
    if (!item.targetIds.empty()) {
        const auto allowed = annotationGroupTargets(state, target);
        auto unique = item.targetIds;
        std::sort(unique.begin(), unique.end());
        if (std::adjacent_find(unique.begin(), unique.end()) != unique.end()
            || std::any_of(unique.begin(), unique.end(), [&](auto id) { return std::find(allowed.begin(), allowed.end(), id) == allowed.end(); })) {
            throw std::runtime_error("Annotation sources must belong to the same group.");
        }
    }
    if (!validAttachments(state, item)) { throw std::runtime_error("The annotation target has changed. Draw it again."); }
    item.targetValid = true;
    state.annotations.push_back(std::move(item));
    assignSceneObjectIds(state);
    const auto id = state.annotations.back().objectId;
    selectSceneObject(state, id);
    markSceneDirty(state);
    return id;
}
SceneObjectId duplicateAnnotation(UiState& state, SceneObjectId id)
{
    const auto* source = findAnnotation(state, id);
    if (!source) { return 0; }
    auto copy = *source;
    copy.objectId = 0;
    copy.settings.name += " copy";
    state.annotations.push_back(std::move(copy));
    assignSceneObjectIds(state);
    const auto created = state.annotations.back().objectId;
    selectSceneObject(state, created);
    markSceneDirty(state);
    return created;
}
void renameAnnotation(UiState& state, SceneObjectId id, const std::string& name)
{
    const auto* item = findAnnotation(state, id);
    if (!item) { return; }
    auto settings = item->settings;
    settings.name = name;
    setAnnotationSettings(state, id, std::move(settings));
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
        if (geometry.fingerprint != item.geometry.fingerprint || geometry.projector != item.geometry.projector
            || geometry.sources != item.geometry.sources) { throw std::runtime_error("Annotation attachment cannot change while reshaping."); }
        auto candidate = item;
        candidate.geometry = geometry;
        if (!validAttachments(state, candidate)) { throw std::runtime_error("Invalid annotation triangle."); }
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
        const auto primary = savedTarget(state, item.targetId);
        record.fileIndex = primary.fileIndex; record.groupIndex = primary.groupIndex;
        for (const auto id : item.targetIds) { record.targets.push_back(savedTarget(state, id)); }
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
        if (record.targets.size() != record.geometry.sources.size()) { throw std::runtime_error("Invalid annotation source count."); }
        item.targetId = loadedTarget(state, document, {record.fileIndex, record.groupIndex});
        for (const auto& target : record.targets) { item.targetIds.push_back(loadedTarget(state, document, target)); }
        state.annotations.push_back(std::move(item));
    }
    assignSceneObjectIds(state);
    validateAnnotationTargets(state);
}
} // namespace woby
