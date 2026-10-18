#include "ui_operations.h"
#include "surface_annotation.h"
#include "cross_section.h"
#include "utf8_path.h"

#include <algorithm>
#include <map>
#include <stdexcept>

namespace woby {
namespace {
bool validAttachments(const UiState& state, const UiAnnotation& item)
{
    const auto count = std::max(size_t{1}, item.targetIds.size());
    if ((!item.geometry->sources.empty() && item.geometry->sources.size() != count)
        || (item.geometry->sources.empty() != item.targetIds.empty())
        || (!item.targetIds.empty() && item.targetIds.front() != item.targetId)) { return false; }
    for (size_t i = 0; i < count; ++i) {
        const auto id = item.targetIds.empty() ? item.targetId : item.targetIds[i];
        bool found = false;
        for (const auto& file : state.files) {
            for (size_t g = 0; g < file.groupSettings.size() && g < meshGeometry(file.mesh).nodes.size(); ++g) {
                if (!id || file.groupSettings[g].objectId != id) { continue; }
                const auto& node = meshGeometry(file.mesh).nodes[g];
                const auto& fingerprint = item.geometry->sources.empty() ? item.geometry->fingerprint : item.geometry->sources[i].fingerprint;
                if (fingerprint != gestureFingerprint(file.mesh, node.indexOffset, node.indexCount)) { return false; }
                for (const auto& segment : item.geometry->segments) {
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
    if (g >= file.groupSettings.size() || g >= meshGeometry(file.mesh).nodes.size() || g >= document.files[f].groups.size()
        || meshGeometry(file.mesh).nodes[g].name != document.files[f].groups[g].name) { return 0; }
    return file.groupSettings[g].objectId;
}
// Legacy annotation projectors address unrecentered model coordinates. Migrate
// only after verifying the old float geometry fingerprint; changed files remain
// unresolved instead of silently attaching a saved annotation to new geometry.
void migrateAnnotationFrame(UiAnnotation& item, const SceneAnnotationRecord& record,
    const UiState& state, const SceneDocument& document, std::map<size_t, Mesh>& legacyMeshes)
{
    const auto targets = record.targets.empty()
        ? std::vector<SceneAnnotationTarget>{{record.fileIndex, record.groupIndex}} : record.targets;
    // Current scenes keep their owner. Only legacy coordinate migration needs
    // a new geometry value, and it must not alter clean or historical owners.
    if (std::none_of(targets.begin(), targets.end(), [&](const auto& target) {
        if (target.fileIndex < 0) { return false; }
        const auto index = static_cast<size_t>(target.fileIndex);
        return index < document.files.size() && index < state.files.size()
            && !document.files[index].coordinateOrigin && state.files[index].mesh.origin != Coordinate{};
    })) { return; }
    auto geometry = *item.geometry;
    std::vector<Coordinate> offsets(targets.size());
    bool changed = false;
    for (size_t i = 0; i < targets.size(); ++i) {
        if (targets[i].fileIndex < 0 || targets[i].groupIndex < 0) { return; }
        const auto f = static_cast<size_t>(targets[i].fileIndex), g = static_cast<size_t>(targets[i].groupIndex);
        if (f >= state.files.size() || f >= document.files.size() || g >= meshGeometry(state.files[f].mesh).nodes.size()) { return; }
        const auto& mesh = state.files[f].mesh;
        if (document.files[f].coordinateOrigin || mesh.origin == Coordinate{}) { continue; }
        const auto& node = meshGeometry(mesh).nodes[g];
        auto [it, inserted] = legacyMeshes.try_emplace(f);
        if (inserted) {
            auto& legacy = it->second;
            editMeshGeometry(legacy).vertices = meshGeometry(mesh).vertices;
            editMeshGeometry(legacy).indices = meshGeometry(mesh).indices;
            for (size_t v = 0; v < editMeshGeometry(legacy).vertices.size(); ++v) {
                editMeshGeometry(legacy).vertices[v].position = renderPosition(originalPosition(meshPosition(mesh,v),mesh.origin));
            }
        }
        auto& fingerprint = geometry.sources.empty() ? geometry.fingerprint : geometry.sources[i].fingerprint;
        if (fingerprint != annotationFingerprint(it->second,node.indexOffset,node.indexCount)) { return; }
        fingerprint = annotationFingerprint(mesh,node.indexOffset,node.indexCount);
        offsets[i] = mesh.origin;
        auto& projector = geometry.sources.empty() ? geometry.projector : geometry.sources[i].projector;
        for (size_t row = 0; row < 4; ++row) {
            for (size_t k = 0; k < 3; ++k) { projector[12+row] += projector[k*4+row]*offsets[i][k]; }
        }
        changed = true;
    }
    if (!changed) { return; }
    for (size_t i = 0; i < geometry.sources.size(); ++i) {
        auto& transform = geometry.sources[i].toPrimary;
        for (size_t row = 0; row < 3; ++row) {
            double translation = double(transform[12+row]) - offsets[0][row];
            for (size_t k = 0; k < 3; ++k) { translation += double(transform[k*4+row])*offsets[i][k]; }
            transform[12+row] = static_cast<float>(translation);
        }
    }
    if (!geometry.sources.empty()) {
        geometry.projector = geometry.sources[0].projector;
        geometry.fingerprint = geometry.sources[0].fingerprint;
    }
    validateAnnotationGeometry(geometry);
    item.geometry = shareAnnotationGeometry(std::move(geometry));
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
    if (item.text) { return {item.text->position}; }
    if (!item.targetValid) { return {}; }
    if (!item.targetIds.empty()) { return annotationControlWorldPositions(item, scenePickParts(state, true)); }
    // Resolve the source independently of visibility, opacity, and selection.
    for (const auto& file : state.files) {
        for (size_t g = 0; g < file.groupSettings.size() && g < meshGeometry(file.mesh).nodes.size(); ++g) {
            if (file.groupSettings[g].objectId == item.targetId) {
                return annotationControlPositions(file.mesh, meshGeometry(file.mesh).nodes[g].indexOffset, *item.geometry);
            }
        }
    }
    return {};
}
std::vector<Coordinate> annotationOriginalVertices(const UiState& state, const UiAnnotation& item)
{
    return annotationOriginalVertices(state, item, annotationVertices(state, item));
}
std::vector<Coordinate> annotationOriginalVertices(const UiState& state, const UiAnnotation& item,
    std::span<const std::array<float, 3>> vertices)
{
    Coordinate origin = state.coordinateOrigin.value_or(Coordinate{});
    if (item.targetIds.empty()) {
        for (const auto& file : state.files) {
            if (std::any_of(file.groupSettings.begin(), file.groupSettings.end(),
                [&](const auto& group) { return group.objectId == item.targetId; })) { origin = file.mesh.origin; break; }
        }
    }
    std::vector<Coordinate> result;
    for (const auto& p : vertices) { result.push_back(originalPosition({p[0],p[1],p[2]}, origin)); }
    return result;
}
void validateAnnotationTargets(UiState& state, bool notifyEdit)
{
    bool changed = false;
    for (auto& item : state.annotations) {
        if (item.section || item.text) { item.targetValid = true; item.targetPending = false; continue; }
        const bool validBefore = item.targetValid, pendingBefore = item.targetPending;
        item.targetPending = false;
        for (const auto& file : state.files) {
            if (annotationMeshCacheReady(file.mesh)) { continue; }
            for (const auto& group : file.groupSettings) {
                if (group.objectId == item.targetId
                    || std::find(item.targetIds.begin(), item.targetIds.end(), group.objectId) != item.targetIds.end()) {
                    item.targetPending = true;
                }
            }
        }
        item.targetValid = !item.targetPending && validAttachments(state, item);
        changed = changed || validBefore != item.targetValid || pendingBefore != item.targetPending;
    }
    if (changed && notifyEdit) { notifySceneEdit(state, SceneChange::annotations); }
}
SceneObjectId createTextAnnotation(UiState& state, TextAnnotationSettings settings)
{
    UiAnnotation item;
    item.text = normalizedTextAnnotationSettings(std::move(settings));
    item.targetValid = true;
    item.settings.name = "Text " + std::to_string(state.annotations.size() + 1);
    state.annotations.push_back(std::move(item));
    assignSceneObjectIds(state);
    const auto id = state.annotations.back().objectId;
    selectSceneObject(state, id);
    markSceneDirty(state, SceneChange::annotations);
    return id;
}
void setTextAnnotationSettings(UiState& state, SceneObjectId id, TextAnnotationSettings settings)
{
    settings = normalizedTextAnnotationSettings(std::move(settings));
    for (auto& item : state.annotations) {
        if (item.objectId != id || !item.text) { continue; }
        if (item.settings.locked && (settings.position != item.text->position || settings.rotationDegrees != item.text->rotationDegrees)) { return; }
        if (settings != *item.text) {
            item.text = std::move(settings);
            ++item.geometryRevision;
            markSceneDirty(state, SceneChange::annotations);
        }
        return;
    }
}
SceneObjectId createCrossSection(UiState& state, CrossSectionSettings settings, std::vector<SceneObjectId> sources)
{
    if (sources.empty()) { throw std::runtime_error("Choose triangle meshes for the section."); }
    std::vector<SceneObjectId> unique;
    for (auto id : sources) {
        const auto object = findSceneObject(state, id);
        if (!object || object->kind != SceneObjectKind::group || std::find(unique.begin(), unique.end(), id) != unique.end()) {
            throw std::runtime_error("Section sources must be distinct model parts.");
        }
        unique.push_back(id);
    }
    if (settings.sourceVisible.size() < sources.size()) {
        auto visible = crossSectionSourceVisibility(state, sources);
        std::copy(settings.sourceVisible.begin(), settings.sourceVisible.end(), visible.begin());
        settings.sourceVisible = std::move(visible);
    }
    UiAnnotation item;
    item.section = normalizedCrossSectionSettings(std::move(settings), sources.size());
    item.targetId = sources.front(); item.targetIds = std::move(sources); item.targetValid = true;
    item.settings.name = "Cross section " + std::to_string(state.annotations.size() + 1);
    state.annotations.push_back(std::move(item)); assignSceneObjectIds(state);
    const auto id = state.annotations.back().objectId; selectSceneObject(state, id);
    markSceneDirty(state, SceneChange::annotations); return id;
}
void setCrossSectionSettings(UiState& state, SceneObjectId id, CrossSectionSettings settings)
{
    for (auto& item : state.annotations) {
        if (item.objectId != id || !item.section) { continue; }
        settings = normalizedCrossSectionSettings(std::move(settings), item.targetIds.size());
        if (item.settings.locked && (settings.a != item.section->a || settings.b != item.section->b || settings.upAxis != item.section->upAxis)) { return; }
        if (settings != *item.section) { item.section = std::move(settings); ++item.geometryRevision; markSceneDirty(state, SceneChange::annotations); }
        return;
    }
}
SceneObjectId createAnnotation(UiState& state, SceneObjectId target, AnnotationGeometry geometry, std::vector<SceneObjectId> targets)
{
    return createAnnotation(state, target, shareAnnotationGeometry(std::move(geometry)), std::move(targets));
}
SceneObjectId createAnnotation(UiState& state, SceneObjectId target, AnnotationGeometryOwner geometry, std::vector<SceneObjectId> targets)
{
    validateAnnotationGeometry(*geometry);
    const auto object = findSceneObject(state, target);
    if (!object || object->kind != SceneObjectKind::group) { throw std::runtime_error("Choose a model part for the annotation."); }
    UiAnnotation item;
    item.targetId = target;
    item.targetIds = std::move(targets);
    item.targetName = object->name;
    if (const auto file = findSceneObject(state, object->fileId)) { item.targetName = file->name + " / " + item.targetName; }
    item.settings.name = (geometry->shape == AnnotationShape::line ? "Surface line " : "Surface rectangle ") + std::to_string(state.annotations.size() + 1);
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
    markSceneDirty(state, SceneChange::annotations);
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
    markSceneDirty(state, SceneChange::annotations);
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
        if (item.objectId == id && item.settings != settings) {
            const bool geometry = item.section.has_value() || item.settings.visible != settings.visible || item.settings.color[3] != settings.color[3]
                || item.settings.locked != settings.locked;
            const bool label = item.settings.name != settings.name;
            item.settings = std::move(settings);
            markSceneDirty(state, (geometry ? SceneChange::annotations : SceneChange::appearance)
                | (label ? SceneChange::labels : SceneChange::none));
            return;
        }
    }
}
void setAllAnnotationsVisible(UiState& state, bool visible)
{
    bool changed = false;
    for (auto& item : state.annotations) {
        changed = changed || item.settings.visible != visible;
        item.settings.visible = visible;
    }
    if (changed) { markSceneDirty(state, SceneChange::annotations); }
}

void reshapeAnnotation(UiState& state, SceneObjectId id, AnnotationGeometry geometry)
{
    reshapeAnnotation(state, id, shareAnnotationGeometry(std::move(geometry)));
}
void reshapeAnnotation(UiState& state, SceneObjectId id, AnnotationGeometryOwner geometry)
{
    validateAnnotationGeometry(*geometry);
    for (auto& item : state.annotations) {
        if (item.objectId != id) { continue; }
        if (item.text) { throw std::runtime_error("Use the text position controls to move text."); }
        if (item.section) { throw std::runtime_error("Use the section plane controls to reshape a cross section."); }
        if (item.settings.locked || !item.targetValid || !findSceneObject(state, item.targetId)) { throw std::runtime_error("This annotation is locked or its target is unavailable."); }
        if (geometry->fingerprint != item.geometry->fingerprint || geometry->projector != item.geometry->projector
            || geometry->sources != item.geometry->sources) { throw std::runtime_error("Annotation attachment cannot change while reshaping."); }
        auto candidate = item;
        candidate.geometry = geometry;
        if (!validAttachments(state, candidate)) { throw std::runtime_error("Invalid annotation triangle."); }
        if (item.geometry != geometry) { item.geometry = std::move(geometry); ++item.geometryRevision; markSceneDirty(state, SceneChange::annotations); }
        return;
    }
}
void deleteAnnotation(UiState& state, SceneObjectId id)
{
    if (!findAnnotation(state, id)) { return; }
    std::erase_if(state.annotations, [id](const auto& item) { return item.objectId == id; });
    std::erase(state.selectedSceneObjects, id);
    markSceneDirty(state, SceneChange::annotations);
}
std::vector<SceneAnnotationRecord> sceneAnnotationRecords(const UiState& state)
{
    std::vector<SceneAnnotationRecord> records;
    for (const auto& item : state.annotations) {
        SceneAnnotationRecord record;
        record.targetName = item.targetName; record.settings = item.settings; record.geometry = item.geometry;
        record.section = item.section; record.text = item.text;
        const auto primary = savedTarget(state, item.targetId);
        record.fileIndex = primary.fileIndex; record.groupIndex = primary.groupIndex;
        for (const auto id : item.targetIds) { record.targets.push_back(savedTarget(state, id)); }
        records.push_back(std::move(record));
    }
    return records;
}
void loadSceneAnnotations(UiState& state, const SceneDocument& document)
{
    std::vector<UiAnnotation> annotations;
    annotations.reserve(document.annotations.size());
    std::map<size_t, Mesh> legacyMeshes;
    for (const auto& record : document.annotations) {
        if (!record.section && !record.text) { validateAnnotationGeometry(*record.geometry); }
        UiAnnotation item;
        item.targetName = record.targetName;
        item.settings = normalizedAnnotationSettings(record.settings);
        item.geometry = record.geometry;
        if (record.text && record.section) { throw std::runtime_error("An annotation cannot be both text and a section."); }
        if (record.text) { item.text = normalizedTextAnnotationSettings(*record.text); }
        if (record.section) { item.section = normalizedCrossSectionSettings(*record.section, record.targets.size()); }
        if (!record.section && !record.text && record.targets.size() != record.geometry->sources.size()) { throw std::runtime_error("Invalid annotation source count."); }
        item.targetId = loadedTarget(state, document, {record.fileIndex, record.groupIndex});
        for (const auto& target : record.targets) { item.targetIds.push_back(loadedTarget(state, document, target)); }
        if (!record.section && !record.text) { migrateAnnotationFrame(item, record, state, document, legacyMeshes); }
        annotations.push_back(std::move(item));
    }
    state.annotations = std::move(annotations);
    assignSceneObjectIds(state);
    validateAnnotationTargets(state);
}
} // namespace woby
