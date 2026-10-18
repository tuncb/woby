#include "ui_operations.h"

#include <algorithm>
#include <cmath>
#include <boost/unordered/unordered_flat_set.hpp>

namespace woby {
namespace {
std::vector<uint64_t> uniqueIds(std::span<const uint64_t> ids)
{
    std::vector<uint64_t> result;
    boost::unordered_flat_set<uint64_t> seen;
    for (const auto id : ids) { if (id && seen.insert(id).second) { result.push_back(id); } }
    return result;
}

bool validSceneSelection(const UiState& state)
{
    if (state.selectedSceneObjects.empty()) { return false; }
    const auto objects = sceneObjects(state);
    boost::unordered_flat_set<SceneObjectId> live;
    for (const auto& object : objects) { live.insert(object.id); }
    return std::all_of(state.selectedSceneObjects.begin(), state.selectedSceneObjects.end(),
        [&](auto id) { return live.contains(id); });
}

bool validViews(const UiState& state)
{
    return !state.viewSelection.ids.empty() && std::all_of(state.viewSelection.ids.begin(), state.viewSelection.ids.end(),
        [&](auto id) { return findView(state, id) != nullptr; });
}

boost::unordered_flat_set<SceneObjectId> inputSelectableIds(const UiState& state, SceneObjectId analysis, ComparisonSide side)
{
    const auto members = comparisonMemberIds(state, side, analysis, false);
    boost::unordered_flat_set<SceneObjectId> result;
    for (auto part : comparableScenePartIds(state)) {
        if (std::binary_search(members.begin(), members.end(), part)) { result.insert(part); }
    }
    for (const auto& file : state.files) {
        if (std::any_of(file.groupSettings.begin(), file.groupSettings.end(),
                [&](const auto& part) { return result.contains(part.objectId); })) { result.insert(file.objectId); }
    }
    const auto visit = [&](auto&& self, const UiSceneNode& node) -> bool {
        bool included = result.contains(node.objectId);
        for (const auto& child : node.children) { included = self(self, child) || included; }
        if (included && node.kind == UiSceneNodeKind::folder) { result.insert(node.objectId); }
        return included;
    };
    for (const auto& node : state.sceneNodes) { visit(visit, node); }
    return result;
}

void setDetectorVisible(ComparisonSettings& settings, DiagnosticCategory category, bool visible)
{
    switch (category) {
    case DiagnosticCategory::boundary: settings.showBoundaries = visible; break;
    case DiagnosticCategory::nonManifold: settings.showNonManifold = visible; break;
    case DiagnosticCategory::winding: settings.showWinding = visible; break;
    case DiagnosticCategory::duplicatePoints: settings.duplicates.showPoints = visible; break;
    case DiagnosticCategory::duplicateTriangles: settings.duplicates.showTriangles = visible; break;
    case DiagnosticCategory::degenerateTriangles: settings.degenerates.show = visible; break;
    case DiagnosticCategory::nonManifoldVertices: settings.topologyInspection.showNonManifoldVertices = visible; break;
    case DiagnosticCategory::holes: settings.topologyInspection.showHoles = visible; break;
    case DiagnosticCategory::fins: settings.topologyInspection.showFins = visible; break;
    case DiagnosticCategory::selfIntersections: settings.intersections.show = visible; break;
    }
}
} // namespace

void selectListItem(std::vector<uint64_t>& selection, uint64_t& anchor, uint64_t id,
    std::span<const uint64_t> rows, bool toggle, bool range, bool context)
{
    const auto last = std::find(rows.begin(), rows.end(), id);
    if (!id || last == rows.end()) { return; }
    if (context) {
        if (std::find(selection.begin(), selection.end(), id) == selection.end()) { selection.push_back(id); }
        if (!anchor) { anchor = id; }
        return;
    }
    const auto first = std::find(rows.begin(), rows.end(), anchor);
    if (range && first != rows.end()) {
        if (!toggle) { selection.clear(); }
        const auto low = std::min(first, last), high = std::max(first, last);
        for (auto it = low; it != high + 1; ++it) {
            if (std::find(selection.begin(), selection.end(), *it) == selection.end()) { selection.push_back(*it); }
        }
    } else {
        if (!toggle) { selection = {id}; }
        else if (std::find(selection.begin(), selection.end(), id) != selection.end()) { std::erase(selection, id); }
        else { selection.push_back(id); }
        anchor = id;
    }
}

void selectSceneRow(UiState& state, SceneObjectId id, std::span<const SceneObjectId> rows,
    bool toggle, bool range, bool context)
{
    if (!findSceneObject(state, id)) { return; }
    if (!range) { selectSceneObject(state, id, toggle, context); return; }
    std::vector<SceneObjectId> live;
    for (auto row : rows) { if (findSceneObject(state, row)) { live.push_back(row); } }
    selectListItem(state.selectedSceneObjects, state.sceneSelectionAnchor, id, live, toggle, range, context);
    setPropertiesPaneVisible(state, true);
    if (const auto* selected = selectedComparison(state)) { state.activeComparisonId = selected->objectId; }
}

void selectSceneRows(UiState& state, std::span<const SceneObjectId> rows)
{
    const auto ids = uniqueIds(rows);
    boost::unordered_flat_set<SceneObjectId> live;
    for (const auto& object : sceneObjects(state)) { live.insert(object.id); }
    if (std::any_of(ids.begin(), ids.end(), [&](auto id) { return !live.contains(id); })) { return; }
    state.selectedSceneObjects = ids;
    state.sceneSelectionAnchor = ids.empty() ? 0 : ids.front();
    if (!ids.empty()) { setPropertiesPaneVisible(state, true); }
    if (const auto* selected = selectedComparison(state)) { state.activeComparisonId = selected->objectId; }
}

void selectViewRow(UiState& state, ViewId id, std::span<const ViewId> rows, bool toggle, bool range, bool context)
{
    if (!findView(state, id)) { return; }
    auto& selection = state.viewSelection;
    std::erase_if(selection.ids, [&](auto value) { return !findView(state, value); });
    std::vector<ViewId> live;
    for (auto row : rows) { if (findView(state, row)) { live.push_back(row); } }
    selectListItem(selection.ids, selection.anchor, id, live, toggle, range, context);
}

void selectViewRows(UiState& state, std::span<const ViewId> rows)
{
    const auto ids = uniqueIds(rows);
    if (std::any_of(ids.begin(), ids.end(), [&](auto id) { return !findView(state, id); })) { return; }
    state.viewSelection = {ids, ids.empty() ? 0 : ids.front()};
}

void selectAnalysisInputRow(UiState& state, SceneObjectId analysis, ComparisonSide side, SceneObjectId id,
    std::span<const SceneObjectId> rows, bool toggle, bool range, bool context)
{
    auto* item = findComparison(state, analysis);
    if (!item) { return; }
    const auto eligible = inputSelectableIds(state, analysis, side);
    if (!eligible.contains(id)) { return; }
    auto& selection = item->inputSelection[side == ComparisonSide::a ? 0 : 1];
    std::erase_if(selection.ids, [&](auto value) { return !eligible.contains(value); });
    std::vector<SceneObjectId> live;
    for (auto row : rows) { if (eligible.contains(row)) { live.push_back(row); } }
    selectListItem(selection.ids, selection.anchor, id, live, toggle, range, context);
}

void selectAnalysisInputRows(UiState& state, SceneObjectId analysis, ComparisonSide side,
    std::span<const SceneObjectId> rows)
{
    auto* item = findComparison(state, analysis);
    if (!item) { return; }
    const auto ids = uniqueIds(rows);
    const auto eligible = inputSelectableIds(state, analysis, side);
    if (std::any_of(ids.begin(), ids.end(), [&](auto id) { return !eligible.contains(id); })) { return; }
    item->inputSelection[side == ComparisonSide::a ? 0 : 1] = {ids, ids.empty() ? 0 : ids.front()};
}

void selectDetectorRow(UiState& state, SceneObjectId analysis, DiagnosticCategory category,
    std::span<const uint64_t> rows, bool toggle, bool range, bool context)
{
    auto* item = findComparison(state, analysis);
    if (!item || static_cast<size_t>(category) >= diagnosticCategoryCount) { return; }
    auto& selection = item->detectorSelection;
    selectListItem(selection.ids, selection.anchor, static_cast<uint64_t>(category) + 1, rows, toggle, range, context);
}

void selectDetectorRows(UiState& state, SceneObjectId analysis, std::span<const uint64_t> rows)
{
    auto* item = findComparison(state, analysis);
    if (!item || std::any_of(rows.begin(), rows.end(), [](auto id) { return !id || id > diagnosticCategoryCount; })) { return; }
    const auto ids = uniqueIds(rows);
    item->detectorSelection = {ids, ids.empty() ? 0 : ids.front()};
}

bool sceneSelectionHasKind(const UiState& state, SceneObjectKind kind)
{
    if (state.selectedSceneObjects.empty()) { return false; }
    boost::unordered_flat_set<SceneObjectId> eligible;
    for (const auto& item : sceneObjects(state)) { if (item.kind == kind) { eligible.insert(item.id); } }
    return std::all_of(state.selectedSceneObjects.begin(), state.selectedSceneObjects.end(),
        [&](auto id) { return eligible.contains(id); });
}

bool setSceneSelectionVisible(UiState& state, bool visible)
{
    if (!validSceneSelection(state)) { return false; }
    const boost::unordered_flat_set<SceneObjectId> ids(state.selectedSceneObjects.begin(), state.selectedSceneObjects.end());
    bool changed = false;
    const auto assign = [&](bool& value) { changed = changed || value != visible; value = visible; };
    const auto fileVisibility = [&](UiFileState& file) {
        assign(file.fileSettings.visible);
        for (auto& part : file.groupSettings) { assign(part.visible); }
    };
    const auto visit = [&](auto&& self, UiSceneNode& node, bool inherited) -> void {
        const bool selected = inherited || ids.contains(node.objectId);
        if (node.kind == UiSceneNodeKind::folder && selected) { assign(node.settings.visible); }
        if (node.fileIndex < state.files.size()) {
            auto& file = state.files[node.fileIndex];
            if (selected && node.kind == UiSceneNodeKind::file) { fileVisibility(file); }
            else if (selected && node.kind == UiSceneNodeKind::group && node.groupIndex < file.groupSettings.size()) {
                assign(file.groupSettings[node.groupIndex].visible);
                if (visible) { assign(file.fileSettings.visible); }
            }
        }
        for (auto& child : node.children) { self(self, child, selected); }
    };
    for (auto& node : state.sceneNodes) { visit(visit, node, false); }
    for (auto& file : state.files) {
        if (ids.contains(file.objectId)) { fileVisibility(file); }
        for (auto& part : file.groupSettings) {
            if (ids.contains(part.objectId)) {
                assign(part.visible);
                if (visible) { assign(file.fileSettings.visible); }
            }
        }
    }
    for (auto& item : state.comparisons) {
        if (!ids.contains(item.objectId)) { continue; }
        assign(item.settings.enabled);
        if (!visible) { item.diagnosticFocus.reset(); item.pendingDiagnosticFocus.reset(); item.uvFindingFocus.reset(); }
    }
    for (auto& item : state.annotations) { if (ids.contains(item.objectId)) { assign(item.settings.visible); } }
    if (changed) {
        refreshSceneTreeFolderVisibility(state);
        recalculateSceneBounds(state);
        markSceneDirty(state, SceneChange::visibility | SceneChange::annotations);
    }
    return changed;
}

bool duplicateSceneSelection(UiState& state)
{
    const bool analyses = sceneSelectionHasKind(state, SceneObjectKind::comparison);
    if (!analyses && !sceneSelectionHasKind(state, SceneObjectKind::annotation)) { return false; }
    const auto ids = uniqueIds(state.selectedSceneObjects);
    std::vector<SceneObjectId> copies;
    for (auto id : ids) { copies.push_back(analyses ? duplicateComparison(state, id) : duplicateAnnotation(state, id)); }
    selectSceneRows(state, copies);
    return true;
}

bool deleteSceneSelection(UiState& state)
{
    const bool analyses = sceneSelectionHasKind(state, SceneObjectKind::comparison);
    if (!analyses && !sceneSelectionHasKind(state, SceneObjectKind::annotation)) { return false; }
    const boost::unordered_flat_set<SceneObjectId> ids(state.selectedSceneObjects.begin(), state.selectedSceneObjects.end());
    if (analyses) {
        std::erase_if(state.comparisons, [&](const auto& item) { return ids.contains(item.objectId); });
        if (ids.contains(state.activeComparisonId)) { state.activeComparisonId = state.comparisons.empty() ? 0 : state.comparisons.front().objectId; }
    } else { std::erase_if(state.annotations, [&](const auto& item) { return ids.contains(item.objectId); }); }
    clearSceneSelection(state);
    pruneMissingViewReferences(state);
    recalculateSceneBounds(state);
    markSceneDirty(state, analyses ? SceneChange::analysis : SceneChange::annotations);
    return true;
}

bool setSelectedAnnotationStyle(UiState& state, std::optional<bool> locked, std::optional<float> width,
    std::optional<std::array<float, 4>> color)
{
    if (!sceneSelectionHasKind(state, SceneObjectKind::annotation) || (width && !std::isfinite(*width))) { return false; }
    if (color && std::any_of(color->begin(), color->end(), [](float value) { return !std::isfinite(value); })) { return false; }
    const auto ids = uniqueIds(state.selectedSceneObjects);
    if (width && std::any_of(ids.begin(), ids.end(), [&](auto id) { return findAnnotation(state, id)->text.has_value(); })) { return false; }
    if (color && std::any_of(ids.begin(), ids.end(), [&](auto id) { return findAnnotation(state, id)->section.has_value(); })) { return false; }
    const auto revision = state.sceneEditRevision;
    for (auto id : ids) {
        const auto* item = findAnnotation(state, id);
        auto settings = item->settings;
        if (locked) { settings.locked = *locked; }
        if (width) { settings.width = std::clamp(*width, 1.0f, item->section ? 8.0f : 12.0f); }
        if (color) { settings.color = *color; }
        setAnnotationSettings(state, id, std::move(settings));
    }
    return revision != state.sceneEditRevision;
}

bool duplicateSelectedViews(UiState& state)
{
    if (!validViews(state)) { return false; }
    const auto ids = uniqueIds(state.viewSelection.ids);
    std::vector<ViewId> copies;
    for (auto id : ids) { copies.push_back(duplicateView(state, id)); }
    selectViewRows(state, copies);
    return true;
}

bool deleteSelectedViews(UiState& state)
{
    if (!validViews(state)) { return false; }
    const boost::unordered_flat_set<ViewId> ids(state.viewSelection.ids.begin(), state.viewSelection.ids.end());
    std::erase_if(state.views, [&](const auto& item) { return ids.contains(item.id); });
    if (ids.contains(state.activeViewId)) { state.activeViewId = 0; }
    state.viewSelection = {};
    markSceneDirty(state, SceneChange::none);
    return true;
}

bool applySelectedAnalysisInputs(UiState& state, SceneObjectId analysis, ComparisonSide side, UiInputAction action)
{
    if (action < UiInputAction::enable || action > UiInputAction::remove) { return false; }
    auto* item = findComparison(state, analysis);
    if (!item || (isUvAnalysis(item->settings.type) && side == ComparisonSide::b)) { return false; }
    auto& selection = item->inputSelection[side == ComparisonSide::a ? 0 : 1];
    const auto ids = uniqueIds(selection.ids);
    const auto eligible = inputSelectableIds(state, analysis, side);
    if (ids.empty() || std::any_of(ids.begin(), ids.end(), [&](auto id) { return !eligible.contains(id); })) { return false; }
    const auto revision = state.sceneEditRevision;
    if (action == UiInputAction::remove) { setComparisonObjects(state, ids, side, false, analysis); selection = {}; }
    else if (action == UiInputAction::isolate) { isolateComparisonObjects(state, ids, side, analysis); }
    else { setComparisonObjectsEnabled(state, ids, side, action == UiInputAction::enable, analysis); }
    return revision != state.sceneEditRevision;
}

bool applySelectedDetectors(UiState& state, SceneObjectId analysis, UiDetectorAction action)
{
    if (action < UiDetectorAction::run || action > UiDetectorAction::automaticOff) { return false; }
    auto* item = findComparison(state, analysis);
    if (!item || isUvAnalysis(item->settings.type)) { return false; }
    const auto& selected = item->detectorSelection.ids;
    if (selected.empty() || std::any_of(selected.begin(), selected.end(), [](auto id) { return !id || id > diagnosticCategoryCount; })) { return false; }
    if (action == UiDetectorAction::run && !enabledComparisonPartCount(state, ComparisonSide::a, analysis)
        && !enabledComparisonPartCount(state, ComparisonSide::b, analysis)) { return false; }
    auto settings = item->settings;
    for (auto id : uniqueIds(selected)) {
        const auto category = static_cast<DiagnosticCategory>(id - 1);
        if (action == UiDetectorAction::run || action == UiDetectorAction::cancel) {
            requestComparisonDetector(state, analysis, category, action == UiDetectorAction::cancel);
        } else if (action == UiDetectorAction::show || action == UiDetectorAction::hide) {
            setDetectorVisible(settings, category, action == UiDetectorAction::show);
            if (action == UiDetectorAction::hide && item->diagnosticFocus && item->diagnosticFocus->category == category) {
                item->diagnosticFocus.reset();
            }
        } else { setDiagnosticAutoUpdate(settings, category, action == UiDetectorAction::automaticOn); }
    }
    setComparisonSettings(state, settings, analysis);
    return true;
}

std::vector<SceneObjectId> selectedRemovalFiles(const UiState& state)
{
    if (!validSceneSelection(state)) { return {}; }
    boost::unordered_flat_set<SceneObjectId> selected(state.selectedSceneObjects.begin(), state.selectedSceneObjects.end());
    const auto objects = sceneObjects(state);
    for (const auto& object : objects) {
        if (selected.contains(object.id) && (object.kind == SceneObjectKind::comparison || object.kind == SceneObjectKind::annotation)) { return {}; }
    }
    boost::unordered_flat_set<SceneObjectId> files;
    const auto visit = [&](auto&& self, const UiSceneNode& node, bool inherited) -> void {
        const bool included = inherited || (node.kind != UiSceneNodeKind::group && selected.contains(node.objectId));
        if (included && node.kind != UiSceneNodeKind::folder && node.fileIndex < state.files.size()) {
            files.insert(state.files[node.fileIndex].objectId);
        }
        for (const auto& child : node.children) { self(self, child, included); }
    };
    for (const auto& node : state.sceneNodes) { visit(visit, node, false); }
    for (const auto& file : state.files) { if (selected.contains(file.objectId)) { files.insert(file.objectId); } }
    // A part can accompany its selected whole file, but cannot imply file removal.
    for (const auto& object : objects) {
        if (selected.contains(object.id) && object.kind == SceneObjectKind::group && !files.contains(object.fileId)) { return {}; }
    }
    std::vector<SceneObjectId> result;
    for (const auto& file : state.files) { if (files.contains(file.objectId)) { result.push_back(file.objectId); } }
    return result;
}
} // namespace woby
