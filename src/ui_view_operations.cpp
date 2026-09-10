#include "ui_operations.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace woby {
namespace {

template <typename Settings>
SceneGroupSettings appearance(const Settings& value)
{
    SceneGroupSettings result;
    result.visible = value.visible;
    result.scale = value.scale;
    result.opacity = value.opacity;
    result.translation = value.translation;
    result.rotationDegrees = value.rotationDegrees;
    return result;
}

SceneGroupSettings normalizedAppearance(const SceneGroupSettings& value)
{
    UiGroupState group;
    group.visible = value.visible;
    group.showSolidMesh = value.showSolidMesh;
    group.showTriangles = value.showTriangles;
    group.showVertices = value.showVertices;
    setGroupScale(group, value.scale);
    setGroupOpacity(group, value.opacity);
    setGroupTranslation(group, value.translation);
    setGroupRotationDegrees(group, value.rotationDegrees);
    setGroupVertexSizeScale(group, value.vertexSizeScale);
    setGroupColor(group, value.color);
    return sceneGroupSettings(group);
}

template <typename Settings>
void applyAppearance(Settings& target, const SceneGroupSettings& value)
{
    target.visible = value.visible;
    target.scale = value.scale;
    target.opacity = value.opacity;
    target.translation = value.translation;
    target.rotationDegrees = value.rotationDegrees;
}

template <typename Nodes, typename Visitor>
void visitNodes(Nodes& nodes, const Visitor& visitor)
{
    for (auto& node : nodes) {
        visitor(node);
        visitNodes(node.children, visitor);
    }
}

ViewSceneSettings sceneSettings(const UiState& state)
{
    return {state.camera, state.showOrigin, state.showGrid, state.showDimensions,
        state.upAxis, state.masterVertexPointSize};
}

UiView captureView(const UiState& state)
{
    UiView view;
    view.scene = sceneSettings(state);
    const auto add = [&](SceneObjectId id, SceneGroupSettings settings, ComparisonSettings comparison = {}) {
        const auto selected = std::find(state.selectedSceneObjects.begin(), state.selectedSceneObjects.end(), id);
        const int order = selected == state.selectedSceneObjects.end() ? -1
            : static_cast<int>(selected - state.selectedSceneObjects.begin());
        view.objects.push_back({id, {settings, comparison, order}});
    };
    for (const auto& file : state.files) {
        auto settings = appearance(file.fileSettings);
        settings.vertexSizeScale = file.vertexSizeScale;
        add(file.objectId, settings);
        for (const auto& group : file.groupSettings) { add(group.objectId, sceneGroupSettings(group)); }
    }
    visitNodes(state.sceneNodes, [&](const auto& node) {
        if (node.kind == UiSceneNodeKind::folder) { add(node.objectId, appearance(node.settings)); }
    });
    for (const auto& comparison : state.comparisons) {
        SceneGroupSettings settings;
        settings.translation = comparison.translation;
        add(comparison.objectId, settings, comparison.settings);
        for (const auto side : {ComparisonSide::a, ComparisonSide::b}) {
            for (const auto& part : side == ComparisonSide::a ? comparison.a : comparison.b) {
                if (findSceneObject(state, part.objectId)) {
                    view.parts.push_back({comparison.objectId, part.objectId, side, part.enabled});
                }
            }
        }
    }
    return view;
}

UiView* mutableView(UiState& state, ViewId id)
{
    const auto found = std::find_if(state.views.begin(), state.views.end(),
        [id](const auto& view) { return view.id == id; });
    return found == state.views.end() ? nullptr : &*found;
}

ViewId allocateViewId(UiState& state)
{
    if (state.nextViewId == 0) { throw std::overflow_error("View IDs exhausted."); }
    return state.nextViewId++;
}

// Document indices are generated from the current object order, never retained
// across edits. Folder indices include every node in the persisted preorder.
struct ObjectReference {
    SceneObjectId id;
    SceneViewObjectRecord record;
};
std::vector<ObjectReference> objectReferences(const UiState& state)
{
    std::vector<ObjectReference> result;
    for (size_t f = 0; f < state.files.size(); ++f) {
        const auto& file = state.files[f];
        result.push_back({file.objectId, {ViewObjectKind::file, static_cast<int>(f), -1, {}}});
        for (size_t g = 0; g < file.groupSettings.size() && g < file.mesh.nodes.size(); ++g) {
            result.push_back({file.groupSettings[g].objectId,
                {ViewObjectKind::group, static_cast<int>(f), static_cast<int>(g), {}}});
        }
    }
    int index = 0;
    visitNodes(state.sceneNodes, [&](const auto& node) {
        if (node.kind == UiSceneNodeKind::folder) {
            result.push_back({node.objectId, {ViewObjectKind::folder, index, -1, {}}});
        }
        ++index;
    });
    for (size_t c = 0; c < state.comparisons.size(); ++c) {
        result.push_back({state.comparisons[c].objectId,
            {ViewObjectKind::comparison, static_cast<int>(c), -1, {}}});
    }
    return result;
}

} // namespace

const UiView* findView(const UiState& state, ViewId id)
{
    const auto found = std::find_if(state.views.begin(), state.views.end(),
        [id](const auto& view) { return view.id == id; });
    return found == state.views.end() ? nullptr : &*found;
}

ViewId createView(UiState& state)
{
    auto view = captureView(state);
    size_t number = 1;
    do { view.name = "View " + std::to_string(number++); }
    while (std::any_of(state.views.begin(), state.views.end(),
        [&](const auto& other) { return other.name == view.name; }));
    view.id = allocateViewId(state);
    state.activeViewId = view.id;
    state.views.push_back(std::move(view));
    markSceneDirty(state);
    return state.activeViewId;
}

void updateView(UiState& state, ViewId id)
{
    auto* view = mutableView(state, id);
    if (!view) { return; }
    auto captured = captureView(state);
    if (view->scene == captured.scene && view->objects == captured.objects && view->parts == captured.parts) { return; }
    view->scene = captured.scene;
    view->objects = std::move(captured.objects);
    view->parts = std::move(captured.parts);
    markSceneDirty(state);
}

void renameView(UiState& state, ViewId id, const std::string& name)
{
    auto* view = mutableView(state, id);
    if (!view) { return; }
    const auto first = name.find_first_not_of(" \t\r\n");
    const auto normalized = first == std::string::npos ? "View"
        : name.substr(first, name.find_last_not_of(" \t\r\n") - first + 1);
    if (view->name == normalized) { return; }
    view->name = normalized;
    markSceneDirty(state);
}

void removeView(UiState& state, ViewId id)
{
    if (!findView(state, id)) { return; }
    std::erase_if(state.views, [id](const auto& view) { return view.id == id; });
    if (state.activeViewId == id) { state.activeViewId = 0; }
    markSceneDirty(state);
}

void pruneMissingViewReferences(UiState& state)
{
    if (state.views.empty()) { return; }
    std::unordered_set<SceneObjectId> live;
    for (const auto& object : sceneObjects(state)) { live.insert(object.id); }
    for (auto& view : state.views) {
        std::erase_if(view.objects, [&](const auto& object) { return !live.contains(object.objectId); });
        std::erase_if(view.parts, [&](const auto& part) {
            return !live.contains(part.partId)
                || !comparisonContains(state, part.partId, part.side, part.comparisonId);
        });
    }
}

void applyView(UiState& state, ViewId id)
{
    if (!findView(state, id)) { return; }
    const auto beforeDocument = createSceneDocument(state);
    const ViewNavigation before{state.camera, state.selectedSceneObjects};
    pruneMissingViewReferences(state);
    const auto& view = *findView(state, id);
    // Validate the camera before mutating any display properties.
    const auto camera = normalizedSceneCamera(view.scene.camera);
    for (const auto& saved : view.parts) {
        if (auto* comparison = findComparison(state, saved.comparisonId)) {
            for (auto& part : saved.side == ComparisonSide::a ? comparison->a : comparison->b) {
                if (part.objectId == saved.partId) { part.enabled = saved.enabled; }
            }
        }
    }
    for (const auto& object : view.objects) {
        const auto settings = normalizedAppearance(object.settings.appearance);
        for (auto& file : state.files) {
            if (file.objectId == object.objectId) {
                applyAppearance(file.fileSettings, settings);
                file.vertexSizeScale = settings.vertexSizeScale;
            }
            for (auto& group : file.groupSettings) {
                if (group.objectId != object.objectId) { continue; }
                applyAppearance(group, settings);
                group.showSolidMesh = settings.showSolidMesh;
                group.showTriangles = settings.showTriangles;
                group.showVertices = settings.showVertices;
                group.vertexSizeScale = settings.vertexSizeScale;
                group.color = settings.color;
            }
        }
        visitNodes(state.sceneNodes, [&](auto& node) {
            if (node.kind == UiSceneNodeKind::folder && node.objectId == object.objectId) {
                applyAppearance(node.settings, settings);
            }
        });
        if (auto* comparison = findComparison(state, object.objectId)) {
            comparison->settings = normalizedComparisonSettings(object.settings.comparison);
            comparison->translation = settings.translation;
        }
    }
    state.showOrigin = view.scene.showOrigin;
    state.showGrid = view.scene.showGrid;
    state.showDimensions = view.scene.showDimensions;
    state.upAxis = view.scene.upAxis == SceneUpAxis::y ? SceneUpAxis::y : SceneUpAxis::z;
    state.masterVertexPointSize = std::isfinite(view.scene.masterVertexPointSize)
        ? std::clamp(view.scene.masterVertexPointSize, minVertexPointSize, maxVertexPointSize)
        : defaultMasterVertexPointSize;
    state.selectedSceneObjects.clear();
    auto ordered = view.objects;
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        return a.settings.selectionOrder < b.settings.selectionOrder;
    });
    for (const auto& object : ordered) {
        if (object.settings.selectionOrder >= 0) { state.selectedSceneObjects.push_back(object.objectId); }
    }
    refreshSceneTreeFolderVisibility(state);
    refreshSceneTreeFolderCenters(state);
    recalculateSceneBounds(state);
    state.camera = camera;
    state.cameraInput = {};
    state.activeViewId = id;
    const ViewNavigation after{state.camera, state.selectedSceneObjects};
    const bool contentChanged = !sceneContentEqual(beforeDocument, createSceneDocument(state));
    if (contentChanged || before != after) {
        if (contentChanged) { markSceneDirty(state); }
        else { notifySceneEdit(state); }
        state.viewApplication = ViewApplication{before, after, state.sceneEditRevision};
    }
}

std::vector<SceneViewRecord> sceneViewRecords(const UiState& state)
{
    if (state.views.empty()) { return {}; }
    const auto references = objectReferences(state);
    std::unordered_map<SceneObjectId, SceneViewObjectRecord> byId;
    for (const auto& reference : references) { byId.emplace(reference.id, reference.record); }
    std::vector<SceneViewRecord> records;
    for (const auto& view : state.views) {
        SceneViewRecord record{view.name, view.scene, {}, {}};
        for (const auto& object : view.objects) {
            const auto found = byId.find(object.objectId);
            if (found == byId.end()) { continue; }
            auto item = found->second;
            item.settings = object.settings;
            record.objects.push_back(std::move(item));
        }
        for (const auto& part : view.parts) {
            const auto comparison = byId.find(part.comparisonId);
            const auto group = byId.find(part.partId);
            if (comparison == byId.end() || group == byId.end()) { continue; }
            record.parts.push_back({comparison->second.index, group->second.index,
                group->second.groupIndex, part.side, part.enabled});
        }
        records.push_back(std::move(record));
    }
    return records;
}

void loadSceneViews(UiState& state, const SceneDocument& document)
{
    const auto references = objectReferences(state);
    state.views.clear();
    state.activeViewId = 0;
    for (const auto& record : document.views) {
        UiView view;
        view.id = allocateViewId(state);
        view.name = record.name.empty() ? "View" : record.name;
        view.scene = record.scene;
        view.scene.camera = normalizedSceneCamera(view.scene.camera);
        view.scene.upAxis = view.scene.upAxis == SceneUpAxis::y ? SceneUpAxis::y : SceneUpAxis::z;
        view.scene.masterVertexPointSize = std::isfinite(view.scene.masterVertexPointSize)
            ? std::clamp(view.scene.masterVertexPointSize, minVertexPointSize, maxVertexPointSize)
            : defaultMasterVertexPointSize;
        for (const auto& item : record.objects) {
            const auto found = std::find_if(references.begin(), references.end(), [&](const auto& reference) {
                return reference.record.kind == item.kind && reference.record.index == item.index
                    && reference.record.groupIndex == item.groupIndex;
            });
            if (found == references.end()) { continue; }
            if (item.kind == ViewObjectKind::group) {
                const auto f = static_cast<size_t>(item.index), g = static_cast<size_t>(item.groupIndex);
                if (f >= document.files.size() || g >= document.files[f].groups.size()
                    || state.files[f].mesh.nodes[g].name != document.files[f].groups[g].name) { continue; }
            }
            if (std::any_of(view.objects.begin(), view.objects.end(),
                [&](const auto& object) { return object.objectId == found->id; })) { continue; }
            auto settings = item.settings;
            settings.appearance = normalizedAppearance(settings.appearance);
            settings.comparison = normalizedComparisonSettings(settings.comparison);
            view.objects.push_back({found->id, settings});
        }
        for (const auto& part : record.parts) {
            if (part.comparisonIndex < 0 || static_cast<size_t>(part.comparisonIndex) >= state.comparisons.size()) { continue; }
            const auto group = std::find_if(references.begin(), references.end(), [&](const auto& ref) {
                return ref.record.kind == ViewObjectKind::group && ref.record.index == part.fileIndex
                    && ref.record.groupIndex == part.groupIndex;
            });
            if (group == references.end()) { continue; }
            const auto comparisonId = state.comparisons[static_cast<size_t>(part.comparisonIndex)].objectId;
            if (!comparisonContains(state, group->id, part.side, comparisonId)) { continue; }
            if (std::any_of(view.parts.begin(), view.parts.end(), [&](const auto& other) {
                return other.comparisonId == comparisonId && other.partId == group->id && other.side == part.side;
            })) { continue; }
            view.parts.push_back({comparisonId, group->id, part.side, part.enabled});
        }
        state.views.push_back(std::move(view));
    }
}

} // namespace woby
