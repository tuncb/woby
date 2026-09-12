#include "ui_operations.h"
#include "comparison_scene.h"
#include "mesh_comparison.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <stdexcept>
#include <utility>

namespace woby {
bool sceneObjectSelected(const UiState& state, SceneObjectId id)
{
    return std::find(state.selectedSceneObjects.begin(), state.selectedSceneObjects.end(), id)
        != state.selectedSceneObjects.end();
}

void selectSceneObject(UiState& state, SceneObjectId id, bool toggle, bool contextClick)
{
    if (!findSceneObject(state, id)) {
        return;
    }
    setPropertiesPaneVisible(state, true);
    if (findComparison(state, id)) {
        state.activeComparisonId = id;
    }
    const bool selected = sceneObjectSelected(state, id);
    if (contextClick && selected) {
        return;
    }
    if (contextClick) {
        state.selectedSceneObjects.push_back(id);
    } else if (!toggle) {
        state.selectedSceneObjects = {id};
    } else if (selected) {
        std::erase(state.selectedSceneObjects, id);
    } else {
        state.selectedSceneObjects.push_back(id);
    }
    if (const auto* comparison = selectedComparison(state)) {
        state.activeComparisonId = comparison->objectId;
    }
}

void clearSceneSelection(UiState& state)
{
    state.selectedSceneObjects.clear();
}

const UiComparison* selectedComparison(const UiState& state)
{
    return state.selectedSceneObjects.size() == 1 && state.selectedSceneObjects.front() != invalidSceneObjectId
        ? findComparison(state, state.selectedSceneObjects.front()) : nullptr;
}

namespace {
bool comparablePart(const UiFileState& file, size_t index)
{
    if (index >= file.mesh.nodes.size() || index >= file.groupSettings.size()) { return false; }
    const auto& node = file.mesh.nodes[index];
    return node.indexCount >= 3 && node.indexCount % 3 == 0
        && static_cast<size_t>(node.indexOffset) + node.indexCount <= file.mesh.indices.size();
}

std::array<float, 3> initialComparisonTranslation(const UiState& state,
    SceneObjectId excluded = invalidSceneObjectId)
{
    const auto bounds = combineBounds(state.files, state.sceneNodes);
    float rightEdge = bounds.max[0];
    for (const auto& other : state.comparisons) {
        if (other.objectId == excluded) { continue; }
        if (const auto display = comparisonDisplayBounds(state, other.objectId)) {
            rightEdge = std::max(rightEdge, display->max[0]);
        }
    }
    const std::array<float, 3> translation{
        rightEdge - bounds.min[0] + std::max(bounds.max[0] - bounds.min[0], 1.0f) * .25f, 0, 0};
    return finitePosition(translation) ? translation : std::array<float, 3>{};
}
} // namespace

std::vector<SceneObjectId> comparisonObjectParts(const UiState& state, const std::vector<SceneObjectId>& objects)
{
    std::vector<SceneObjectId> parts;
    const auto selected = [&objects](SceneObjectId id) {
        return id != invalidSceneObjectId && std::find(objects.begin(), objects.end(), id) != objects.end();
    };
    const auto appendFile = [&parts](const UiFileState& file) {
        for (size_t i = 0; i < file.groupSettings.size(); ++i) {
            if (comparablePart(file, i)) { parts.push_back(file.groupSettings[i].objectId); }
        }
    };
    // Resolve files and individual parts even when no explicit scene tree exists.
    for (const auto& file : state.files) {
        if (selected(file.objectId)) { appendFile(file); }
        else {
            for (size_t i = 0; i < file.groupSettings.size(); ++i) {
                if (selected(file.groupSettings[i].objectId) && comparablePart(file, i)) {
                    parts.push_back(file.groupSettings[i].objectId);
                }
            }
        }
    }
    const auto visit = [&](auto&& self, const UiSceneNode& node, bool included) -> void {
        included = included || selected(node.objectId);
        if (included && node.fileIndex < state.files.size()) {
            const auto& file = state.files[node.fileIndex];
            if (node.kind == UiSceneNodeKind::file) { appendFile(file); }
            else if (node.kind == UiSceneNodeKind::group && comparablePart(file, node.groupIndex)) {
                parts.push_back(file.groupSettings[node.groupIndex].objectId);
            }
        }
        for (const auto& child : node.children) { self(self, child, included); }
    };
    for (const auto& node : state.sceneNodes) { visit(visit, node, false); }
    std::sort(parts.begin(), parts.end());
    parts.erase(std::unique(parts.begin(), parts.end()), parts.end());
    std::erase(parts, invalidSceneObjectId);
    return parts;
}

const UiComparison* findComparison(const UiState& state, SceneObjectId id)
{
    if (id == invalidSceneObjectId) { id = state.activeComparisonId; }
    for (const auto& comparison : state.comparisons) {
        if (comparison.objectId == id) { return &comparison; }
    }
    return nullptr;
}

UiComparison* findComparison(UiState& state, SceneObjectId id)
{
    if (id == invalidSceneObjectId) { id = state.activeComparisonId; }
    for (auto& comparison : state.comparisons) {
        if (comparison.objectId == id) { return &comparison; }
    }
    return nullptr;
}

ComparisonSettings comparisonSettings(const UiState& state, SceneObjectId id)
{
    const auto* comparison = findComparison(state, id);
    return comparison ? comparison->settings : ComparisonSettings{};
}

bool comparisonContains(const UiState& state, SceneObjectId part, ComparisonSide side, SceneObjectId id)
{
    const auto* comparison = findComparison(state, id);
    if (!comparison || part == invalidSceneObjectId) { return false; }
    const auto& members = side == ComparisonSide::a ? comparison->a : comparison->b;
    return std::any_of(members.begin(), members.end(), [part](const UiComparisonPart& member) {
        return member.objectId == part;
    });
}

size_t comparisonPartCount(const UiState& state, ComparisonSide side, SceneObjectId id)
{
    size_t count = 0;
    for (const auto& file : state.files) {
        for (size_t i = 0; i < file.groupSettings.size(); ++i) {
            if (comparablePart(file, i) && comparisonContains(state, file.groupSettings[i].objectId, side, id)) { ++count; }
        }
    }
    return count;
}

bool comparisonPartEnabled(const UiState& state, SceneObjectId part, ComparisonSide side, SceneObjectId id)
{
    const auto* comparison = findComparison(state, id);
    if (!comparison || part == invalidSceneObjectId) { return false; }
    const auto& members = side == ComparisonSide::a ? comparison->a : comparison->b;
    return std::any_of(members.begin(), members.end(), [part](const UiComparisonPart& member) {
        return member.objectId == part && member.enabled;
    });
}

size_t enabledComparisonPartCount(const UiState& state, ComparisonSide side, SceneObjectId id)
{
    size_t count = 0;
    for (const auto& file : state.files) {
        for (size_t i = 0; i < file.groupSettings.size(); ++i) {
            if (comparablePart(file, i) && comparisonPartEnabled(state, file.groupSettings[i].objectId, side, id)) { ++count; }
        }
    }
    return count;
}

void setComparisonObjectsEnabled(UiState& state, const std::vector<SceneObjectId>& objects, ComparisonSide side,
    bool enabled, SceneObjectId id)
{
    auto* comparison = findComparison(state, id);
    if (!comparison) { return; }
    const auto parts = comparisonObjectParts(state, objects);
    auto& members = side == ComparisonSide::a ? comparison->a : comparison->b;
    bool changed = false;
    for (auto& member : members) {
        if (objects.empty() || std::binary_search(parts.begin(), parts.end(), member.objectId)) {
            changed = changed || member.enabled != enabled;
            member.enabled = enabled;
        }
    }
    if (!changed) { return; }
    recalculateSceneBounds(state);
    markSceneDirty(state);
}

namespace {
bool hasEnabledMissingComparisonParts(const UiState& state, SceneObjectId id)
{
    const auto* comparison = findComparison(state, id);
    if (!comparison) { return false; }
    for (const auto* members : {&comparison->a, &comparison->b}) {
        for (const auto& member : *members) {
            if (member.enabled && comparisonObjectParts(state, {member.objectId}).empty()) { return true; }
        }
    }
    return false;
}
} // namespace

size_t missingComparisonPartCount(const UiState& state, SceneObjectId id)
{
    const auto* comparison = findComparison(state, id);
    if (!comparison) { return 0; }
    return comparison->a.size() + comparison->b.size()
        - comparisonPartCount(state, ComparisonSide::a, id) - comparisonPartCount(state, ComparisonSide::b, id);
}

SceneObjectId createComparison(UiState& state)
{
    UiComparison comparison;
    size_t number = 1;
    do {
        comparison.name = "Analysis " + std::to_string(number++);
    } while (std::any_of(state.comparisons.begin(), state.comparisons.end(), [&](const UiComparison& other) {
        return other.name == comparison.name;
    }));
    comparison.settings.enabled = true;
    comparison.translation = initialComparisonTranslation(state);
    state.comparisons.push_back(std::move(comparison));
    assignSceneObjectIds(state);
    const auto id = state.comparisons.back().objectId;
    selectSceneObject(state, id);
    markSceneDirty(state);
    return id;
}

SceneObjectId duplicateComparison(UiState& state, SceneObjectId id)
{
    const auto* source = findComparison(state, id);
    if (!source) { return invalidSceneObjectId; }
    auto copy = *source;
    copy.diagnosticFocus.reset();
    copy.objectId = invalidSceneObjectId;
    copy.name += " copy";
    const auto sourceBounds = comparisonDisplayBounds(state, id);
    float rightEdge = combineBounds(state.files, state.sceneNodes).max[0];
    for (const auto& other : state.comparisons) {
        if (const auto display = comparisonDisplayBounds(state, other.objectId)) {
            rightEdge = std::max(rightEdge, display->max[0]);
        }
    }
    if (sourceBounds) {
        copy.translation[0] += rightEdge - sourceBounds->min[0]
            + std::max(sourceBounds->max[0] - sourceBounds->min[0], 1.0f) * .25f;
    }
    if (!finitePosition(copy.translation)) { copy.translation = source->translation; }
    state.comparisons.push_back(std::move(copy));
    assignSceneObjectIds(state);
    const auto created = state.comparisons.back().objectId;
    selectSceneObject(state, created);
    recalculateSceneBounds(state);
    markSceneDirty(state);
    return created;
}

void removeComparison(UiState& state, SceneObjectId id)
{
    if (id == invalidSceneObjectId || !findComparison(state, id)) { return; }
    std::erase_if(state.comparisons, [id](const UiComparison& comparison) { return comparison.objectId == id; });
    std::erase(state.selectedSceneObjects, id);
    pruneMissingViewReferences(state);
    if (state.activeComparisonId == id) {
        state.activeComparisonId = state.comparisons.empty() ? invalidSceneObjectId : state.comparisons.front().objectId;
    }
    recalculateSceneBounds(state);
    markSceneDirty(state);
}

void renameComparison(UiState& state, SceneObjectId id, const std::string& name)
{
    if (auto* comparison = findComparison(state, id)) {
        const auto normalized = name.empty() ? "Analysis" : name;
        if (comparison->name == normalized) { return; }
        comparison->name = normalized;
        markSceneDirty(state);
    }
}

void setComparisonTranslation(UiState& state, SceneObjectId id, const std::array<float, 3>& translation)
{
    if (!finitePosition(translation)) { return; }
    if (auto* comparison = findComparison(state, id)) {
        comparison->translation = translation;
        recalculateSceneBounds(state);
        markSceneDirty(state);
    }
}

void frameComparison(UiState& state, SceneObjectId id)
{
    resetComparisonDiagnosticFocus(state, id);
    if (const auto bounds = comparisonDisplayBounds(state, id)) { frameComparisonBounds(state, *bounds); }
}

namespace {
bool diagnosticFocusCurrent(const UiState& state, const UiComparison& comparison)
{
    const auto& focus = comparison.diagnosticFocus;
    return focus && comparison.settings.enabled && focus->signature != 0
        && focus->side == comparison.settings.diagnosticSide
        && focus->category == comparison.settings.diagnosticCategory
        && focus->signature == comparisonGeometrySignature(state, comparison.objectId);
}
}

const DiagnosticEdge* focusedComparisonDiagnostic(const UiState& state,
    const MeshComparison& result, uint64_t resultSignature, SceneObjectId id)
{
    const auto* comparison = findComparison(state, id);
    if (!comparison || !diagnosticFocusCurrent(state, *comparison)
        || comparison->diagnosticFocus->signature != resultSignature) { return nullptr; }
    const auto& focus = *comparison->diagnosticFocus;
    const auto& edges = comparisonDiagnosticEdges(result, focus.side, focus.category);
    return focus.index < edges.size() ? &edges[focus.index] : nullptr;
}

void resetComparisonDiagnosticFocus(UiState& state, SceneObjectId id)
{
    if (auto* comparison = findComparison(state, id)) { comparison->diagnosticFocus.reset(); }
}

void validateComparisonDiagnosticFocus(UiState& state, const MeshComparison& result,
    uint64_t resultSignature, SceneObjectId id)
{
    if (!focusedComparisonDiagnostic(state, result, resultSignature, id)) {
        resetComparisonDiagnosticFocus(state, id);
    }
}

void navigateComparisonDiagnostic(UiState& state, const MeshComparison& result,
    uint64_t resultSignature, int step, SceneObjectId id)
{
    validateComparisonDiagnosticFocus(state, result, resultSignature, id);
    auto* comparison = findComparison(state, id);
    if (!comparison || !comparison->settings.enabled || resultSignature == 0
        || resultSignature != comparisonGeometrySignature(state, id)) { return; }
    const auto& settings = comparison->settings;
    const auto& edges = comparisonDiagnosticEdges(result, settings.diagnosticSide, settings.diagnosticCategory);
    if (edges.empty()) { return; }
    size_t index = 0;
    if (comparison->diagnosticFocus && step != 0) {
        index = comparison->diagnosticFocus->index;
        if (step < 0) { index = index == 0 ? edges.size() - 1 : index - 1; }
        else { index = index + 1 == edges.size() ? 0 : index + 1; }
    } else if (step < 0) { index = edges.size() - 1; }
    const auto& edge = edges[index];
    if (!finitePosition(edge.a) || !finitePosition(edge.b)) { return; }
    std::vector<Vertex> points(2);
    points[0].position = edge.a;
    points[1].position = edge.b;
    auto bounds = calculateBounds(points);
    // Keep enough surrounding surface to understand the defect without allowing
    // distant, unrelated scene objects to overwhelm a small edge's framing.
    const auto resultBounds = comparisonDisplayBounds(state, id);
    bounds.radius = std::max(bounds.radius * 3.0f, resultBounds ? resultBounds->radius * .06f : .001f);
    for (size_t k = 0; k < 3; ++k) {
        bounds.min[k] += comparison->translation[k];
        bounds.max[k] += comparison->translation[k];
        bounds.center[k] += comparison->translation[k];
    }
    comparison->diagnosticFocus = DiagnosticFocus{resultSignature, index, settings.diagnosticSide, settings.diagnosticCategory};
    frameComparisonBounds(state, bounds);
}

ComparisonMembershipAction comparisonMembershipAction(
    const UiState& state, const std::vector<SceneObjectId>& objects, ComparisonSide side, SceneObjectId id)
{
    const auto parts = comparisonObjectParts(state, objects);
    if (parts.empty()) { return ComparisonMembershipAction::unavailable; }
    for (const auto part : parts) {
        if (!comparisonContains(state, part, side, id)) { return ComparisonMembershipAction::add; }
    }
    return ComparisonMembershipAction::remove;
}

bool canCompareGroups(const UiState& state, SceneObjectId id)
{
    return enabledComparisonPartCount(state, ComparisonSide::a, id) != 0
        && enabledComparisonPartCount(state, ComparisonSide::b, id) != 0 && !hasEnabledMissingComparisonParts(state, id);
}

bool canCompareSceneSelection(const UiState& state)
{
    const auto& selection = state.selectedSceneObjects;
    return (selection.size() == 1 || (selection.size() == 2 && selection[0] != selection[1]))
        && std::all_of(selection.begin(), selection.end(), [&](SceneObjectId object) {
            return !comparisonObjectParts(state, {object}).empty();
        });
}

bool canInspectComparison(const UiState& state, SceneObjectId id)
{
    return (enabledComparisonPartCount(state, ComparisonSide::a, id) != 0
        || enabledComparisonPartCount(state, ComparisonSide::b, id) != 0) && !hasEnabledMissingComparisonParts(state, id);
}

ComparisonSettings effectiveComparisonSettings(const UiState& state, SceneObjectId id)
{
    if (const auto* comparison = findComparison(state, id); comparison && diagnosticFocusCurrent(state, *comparison)) {
        auto settings = comparison->settings;
        settings.mode = settings.diagnosticSide == ComparisonSide::a ? ComparisonMode::original : ComparisonMode::repaired;
        return settings;
    }
    auto settings = comparisonSettings(state, id);
    const bool hasA = enabledComparisonPartCount(state, ComparisonSide::a, id) != 0;
    const bool hasB = enabledComparisonPartCount(state, ComparisonSide::b, id) != 0;
    if (settings.mode == ComparisonMode::surfaceQuality) {
        if (!hasB && hasA) { settings.quality.onOriginal = true; }
        if (!hasA && hasB) { settings.quality.onOriginal = false; }
    } else {
        if (!hasB && hasA) { settings.mode = ComparisonMode::original; }
        if (!hasA && hasB) { settings.mode = ComparisonMode::repaired; }
    }
    return settings;
}

bool compareSceneSelection(UiState& state)
{
    if (!canCompareSceneSelection(state)) { return false; }
    const auto selection = state.selectedSceneObjects;
    const auto id = createComparison(state);
    setComparisonObjects(state, {selection[0]}, ComparisonSide::a, true, id);
    if (selection.size() == 2) { setComparisonObjects(state, {selection[1]}, ComparisonSide::b, true, id); }
    frameCameraToScene(state);
    return true;
}

void setComparisonSettings(UiState& state, ComparisonSettings settings, SceneObjectId id)
{
    if (auto* comparison = findComparison(state, id)) {
        const bool wasEnabled = comparison->settings.enabled;
        comparison->settings = normalizedComparisonSettings(settings);
        if (comparison->settings.enabled && !wasEnabled) { setPropertiesPaneVisible(state, true); }
        recalculateSceneBounds(state);
        markSceneDirty(state);
    }
}

void setPropertiesPaneVisible(UiState& state, bool visible)
{
    state.propertiesPaneVisible = visible;
}

void setScreenshotSettings(UiState& state, ScreenshotSettings settings)
{
    state.screenshotSettings = normalizedScreenshotSettings(settings);
}

void setComparisonObjects(UiState& state, const std::vector<SceneObjectId>& objects, ComparisonSide side, bool member,
    SceneObjectId id)
{
    const auto parts = comparisonObjectParts(state, objects);
    if (parts.empty()) { return; }
    if (!findComparison(state, id) && member && id == invalidSceneObjectId) {
        const auto selection = state.selectedSceneObjects;
        id = createComparison(state);
        state.selectedSceneObjects = selection;
    }
    auto* comparison = findComparison(state, id);
    if (!comparison) { return; }
    auto& members = side == ComparisonSide::a ? comparison->a : comparison->b;
    for (const auto part : parts) {
        if (member) {
            if (comparisonContains(state, part, side, comparison->objectId)) { continue; }
            const auto info = findSceneObject(state, part);
            const auto file = info ? findSceneObject(state, info->fileId) : std::nullopt;
            members.push_back({part, (file ? file->name + " / " : "") + (info ? info->name : "Missing part")});
        } else {
            std::erase_if(members, [part](const UiComparisonPart& entry) { return entry.objectId == part; });
        }
    }
    if (member) { setPropertiesPaneVisible(state, true); }
    recalculateSceneBounds(state);
    markSceneDirty(state);
}

void removeMissingComparisonParts(UiState& state, ComparisonSide side, SceneObjectId id)
{
    if (auto* comparison = findComparison(state, id)) {
        auto& members = side == ComparisonSide::a ? comparison->a : comparison->b;
        std::erase_if(members, [&](const UiComparisonPart& part) {
            return comparisonObjectParts(state, {part.objectId}).empty();
        });
        recalculateSceneBounds(state);
        markSceneDirty(state);
    }
}

void clearComparisonGroup(UiState& state, ComparisonSide side, SceneObjectId id)
{
    if (auto* comparison = findComparison(state, id)) {
        (side == ComparisonSide::a ? comparison->a : comparison->b).clear();
        recalculateSceneBounds(state);
        markSceneDirty(state);
    }
}

void swapComparisonGroups(UiState& state, SceneObjectId id)
{
    if (auto* comparison = findComparison(state, id)) {
        std::swap(comparison->a, comparison->b);
        recalculateSceneBounds(state);
        markSceneDirty(state);
    }
}

void frameComparisonBounds(UiState& state, const Bounds& bounds)
{
    if (!finitePosition(bounds.min) || !finitePosition(bounds.max)) {
        return;
    }
    state.camera = frameCameraBounds(bounds, state.upAxis);
}

namespace {

float finiteOr(float value, float fallback)
{
    return std::isfinite(value) ? value : fallback;
}

float clampFinite(float value, float minValue, float maxValue, float fallback)
{
    return std::clamp(finiteOr(value, fallback), minValue, maxValue);
}

std::array<float, 3> finiteArrayOrZero(const std::array<float, 3>& value)
{
    return {
        finiteOr(value[0], 0.0f),
        finiteOr(value[1], 0.0f),
        finiteOr(value[2], 0.0f),
    };
}

std::array<float, 3> clampFiniteArray(
    const std::array<float, 3>& value,
    float minValue,
    float maxValue)
{
    return {
        clampFinite(value[0], minValue, maxValue, 0.0f),
        clampFinite(value[1], minValue, maxValue, 0.0f),
        clampFinite(value[2], minValue, maxValue, 0.0f),
    };
}

bool validFileIndex(const UiState& state, size_t fileIndex)
{
    return fileIndex != invalidSceneNodeIndex && fileIndex < state.files.size();
}

bool validGroupIndex(const UiState& state, size_t fileIndex, size_t groupIndex)
{
    return validFileIndex(state, fileIndex)
        && groupIndex != invalidSceneNodeIndex
        && groupIndex < state.files[fileIndex].groupSettings.size();
}

bool pruneRemovedFile(UiSceneNode& node, size_t removedFileIndex)
{
    if ((node.kind == UiSceneNodeKind::file || node.kind == UiSceneNodeKind::group)
        && node.fileIndex == removedFileIndex) {
        return false;
    }

    if ((node.kind == UiSceneNodeKind::file || node.kind == UiSceneNodeKind::group)
        && node.fileIndex != invalidSceneNodeIndex
        && node.fileIndex > removedFileIndex) {
        --node.fileIndex;
    }

    node.children.erase(
        std::remove_if(
            node.children.begin(),
            node.children.end(),
            [removedFileIndex](UiSceneNode& child) {
                return !pruneRemovedFile(child, removedFileIndex);
            }),
        node.children.end());

    return node.kind != UiSceneNodeKind::folder || !node.children.empty();
}

bool setFolderNodesVisible(UiSceneNode& node, bool visible)
{
    bool changed = false;
    if (node.kind == UiSceneNodeKind::folder) {
        changed = node.settings.visible != visible;
        node.settings.visible = visible;
    }

    for (auto& child : node.children) {
        changed = setFolderNodesVisible(child, visible) || changed;
    }
    return changed;
}

UiSceneNode& ensureFolderSceneNode(
    std::vector<UiSceneNode>& nodes,
    const std::string& name)
{
    const auto found = std::find_if(
        nodes.begin(),
        nodes.end(),
        [&name](const UiSceneNode& node) {
            return node.kind == UiSceneNodeKind::folder && node.name == name;
        });
    if (found != nodes.end()) {
        return *found;
    }

    UiSceneNode folder;
    folder.kind = UiSceneNodeKind::folder;
    folder.name = name;
    nodes.push_back(std::move(folder));
    return nodes.back();
}

std::string folderDisplayName(const std::filesystem::path& path)
{
    const std::string filename = path.filename().string();
    if (!filename.empty()) {
        return filename;
    }

    return path.string();
}

} // namespace

size_t totalGroupCount(const std::vector<UiFileState>& files)
{
    size_t groupCount = 0;
    for (const auto& file : files) {
        groupCount += file.groupSettings.size();
    }

    return groupCount;
}

size_t totalGroupCount(const UiState& state)
{
    return totalGroupCount(state.files);
}

size_t countVisibleGroups(const std::vector<UiGroupState>& groups)
{
    size_t visibleCount = 0;
    for (const auto& group : groups) {
        if (group.visible) {
            ++visibleCount;
        }
    }

    return visibleCount;
}

size_t countVisibleFileGroups(const UiFileState& file)
{
    if (!file.fileSettings.visible) {
        return 0u;
    }

    return countVisibleGroups(file.groupSettings);
}

size_t countVisibleSceneGroups(const UiState& state)
{
    if (!state.sceneNodes.empty()) {
        size_t visibleCount = 0;
        for (const auto& node : state.sceneNodes) {
            visibleCount += countVisibleSceneNodeGroups(state, node);
        }
        return visibleCount;
    }

    size_t visibleCount = 0;
    for (const auto& file : state.files) {
        visibleCount += countVisibleFileGroups(file);
    }

    return visibleCount;
}

size_t countSceneNodeGroups(const UiState& state, const UiSceneNode& node)
{
    if (node.kind == UiSceneNodeKind::group) {
        return validGroupIndex(state, node.fileIndex, node.groupIndex) ? 1u : 0u;
    }

    if (node.kind == UiSceneNodeKind::file
        && validFileIndex(state, node.fileIndex)
        && node.children.empty()) {
        return state.files[node.fileIndex].groupSettings.size();
    }

    size_t groupCount = 0;
    for (const auto& child : node.children) {
        groupCount += countSceneNodeGroups(state, child);
    }
    return groupCount;
}

size_t countVisibleSceneNodeGroups(const UiState& state, const UiSceneNode& node)
{
    if (node.kind == UiSceneNodeKind::folder && !node.settings.visible) {
        return 0u;
    }

    if (node.kind == UiSceneNodeKind::group) {
        if (!validGroupIndex(state, node.fileIndex, node.groupIndex)) {
            return 0u;
        }
        const auto& file = state.files[node.fileIndex];
        return file.fileSettings.visible && file.groupSettings[node.groupIndex].visible ? 1u : 0u;
    }

    if (node.kind == UiSceneNodeKind::file) {
        if (!validFileIndex(state, node.fileIndex)) {
            return 0u;
        }
        const auto& file = state.files[node.fileIndex];
        if (!file.fileSettings.visible) {
            return 0u;
        }
        if (node.children.empty()) {
            return countVisibleFileGroups(file);
        }
    }

    size_t visibleCount = 0;
    for (const auto& child : node.children) {
        visibleCount += countVisibleSceneNodeGroups(state, child);
    }
    return visibleCount;
}

size_t countEnabledGroupRenderMode(const std::vector<UiGroupState>& groups, UiRenderMode mode)
{
    size_t enabledCount = 0;
    for (const auto& group : groups) {
        if (groupRenderModeEnabled(group, mode)) {
            ++enabledCount;
        }
    }

    return enabledCount;
}

size_t countEnabledSceneRenderMode(const UiState& state, UiRenderMode mode)
{
    if (!state.sceneNodes.empty()) {
        size_t enabledCount = 0;
        for (const auto& node : state.sceneNodes) {
            enabledCount += countEnabledSceneNodeRenderMode(state, node, mode);
        }
        return enabledCount;
    }

    size_t enabledCount = 0;
    for (const auto& file : state.files) {
        enabledCount += countEnabledGroupRenderMode(file.groupSettings, mode);
    }

    return enabledCount;
}

size_t countEnabledSceneNodeRenderMode(
    const UiState& state,
    const UiSceneNode& node,
    UiRenderMode mode)
{
    if (node.kind == UiSceneNodeKind::group) {
        if (!validGroupIndex(state, node.fileIndex, node.groupIndex)) {
            return 0u;
        }
        return groupRenderModeEnabled(state.files[node.fileIndex].groupSettings[node.groupIndex], mode) ? 1u : 0u;
    }

    if (node.kind == UiSceneNodeKind::file
        && validFileIndex(state, node.fileIndex)
        && node.children.empty()) {
        return countEnabledGroupRenderMode(state.files[node.fileIndex].groupSettings, mode);
    }

    size_t enabledCount = 0;
    for (const auto& child : node.children) {
        enabledCount += countEnabledSceneNodeRenderMode(state, child, mode);
    }
    return enabledCount;
}

bool groupRenderModeEnabled(const UiGroupState& group, UiRenderMode mode)
{
    switch (mode) {
    case UiRenderMode::solidMesh:
        return group.showSolidMesh;
    case UiRenderMode::triangles:
        return group.showTriangles;
    case UiRenderMode::vertices:
        return group.showVertices;
    }

    return false;
}

void setGroupRenderMode(UiGroupState& group, UiRenderMode mode, bool enabled)
{
    switch (mode) {
    case UiRenderMode::solidMesh:
        group.showSolidMesh = enabled;
        break;
    case UiRenderMode::triangles:
        group.showTriangles = enabled;
        break;
    case UiRenderMode::vertices:
        group.showVertices = enabled;
        break;
    }
}

void toggleGroupRenderMode(UiGroupState& group, UiRenderMode mode)
{
    setGroupRenderMode(group, mode, !groupRenderModeEnabled(group, mode));
}

void setAllGroupRenderModes(std::vector<UiGroupState>& groups, UiRenderMode mode, bool enabled)
{
    for (auto& group : groups) {
        setGroupRenderMode(group, mode, enabled);
    }
}

void setUiScale(UiState& state, float scale)
{
    state.uiScale = std::clamp(finiteOr(scale, 1.0f), 1.0f, 2.0f);
}

void applyInspectionPreset(UiState& state, UiInspectionPreset preset)
{
    switch (preset) {
    case UiInspectionPreset::solid:
    case UiInspectionPreset::edges:
    case UiInspectionPreset::vertices: break;
    default: return;
    }
    setAllSceneRenderModes(state, UiRenderMode::solidMesh, true);
    setAllSceneRenderModes(state, UiRenderMode::triangles, preset != UiInspectionPreset::solid);
    setAllSceneRenderModes(state, UiRenderMode::vertices, preset == UiInspectionPreset::vertices);
    setShowOrigin(state, false);
    setShowGrid(state, false);
}

void setAllSceneRenderModes(UiState& state, UiRenderMode mode, bool enabled)
{
    bool changed = false;
    for (auto& file : state.files) {
        for (const auto& group : file.groupSettings) {
            changed = changed || groupRenderModeEnabled(group, mode) != enabled;
        }
        setAllGroupRenderModes(file.groupSettings, mode, enabled);
    }
    if (changed) {
        markSceneDirty(state);
    }
}

void setSceneNodeSubtreeRenderMode(
    UiState& state,
    UiSceneNode& node,
    UiRenderMode mode,
    bool enabled)
{
    notifySceneEdit(state);
    if (node.kind == UiSceneNodeKind::group) {
        if (validGroupIndex(state, node.fileIndex, node.groupIndex)) {
            setGroupRenderMode(state.files[node.fileIndex].groupSettings[node.groupIndex], mode, enabled);
        }
        return;
    }

    if (node.kind == UiSceneNodeKind::file
        && validFileIndex(state, node.fileIndex)
        && node.children.empty()) {
        setAllGroupRenderModes(state.files[node.fileIndex].groupSettings, mode, enabled);
        return;
    }

    for (auto& child : node.children) {
        setSceneNodeSubtreeRenderMode(state, child, mode, enabled);
    }
}

void setFileVisible(UiFileState& file, bool visible)
{
    file.fileSettings.visible = visible;
    for (auto& group : file.groupSettings) {
        group.visible = visible;
    }
}

void toggleFileVisible(UiFileState& file)
{
    setFileVisible(file, countVisibleFileGroups(file) != file.groupSettings.size());
}

void setAllSceneVisible(UiState& state, bool visible)
{
    bool changed = false;
    for (auto& comparison : state.comparisons) {
        changed = changed || comparison.settings.enabled != visible;
        comparison.settings.enabled = visible;
    }
    for (auto& node : state.sceneNodes) {
        changed = setFolderNodesVisible(node, visible) || changed;
    }
    for (auto& file : state.files) {
        changed = changed || file.fileSettings.visible != visible;
        for (const auto& group : file.groupSettings) {
            changed = changed || group.visible != visible;
        }
        setFileVisible(file, visible);
    }
    refreshSceneTreeFolderVisibility(state);
    if (changed) {
        markSceneDirty(state);
    }
}

static void setSceneNodeSubtreeVisibleRecursive(UiState& state, UiSceneNode& node, bool visible)
{
    if (node.kind == UiSceneNodeKind::folder) {
        node.settings.visible = visible;
    } else if (node.kind == UiSceneNodeKind::file) {
        if (validFileIndex(state, node.fileIndex)) {
            setFileVisible(state.files[node.fileIndex], visible);
        }
    } else if (validGroupIndex(state, node.fileIndex, node.groupIndex)) {
        auto& file = state.files[node.fileIndex];
        setGroupVisible(file, file.groupSettings[node.groupIndex], visible);
    }

    for (auto& child : node.children) {
        setSceneNodeSubtreeVisibleRecursive(state, child, visible);
    }
}

void setSceneNodeSubtreeVisible(UiState& state, UiSceneNode& node, bool visible)
{
    setSceneNodeSubtreeVisibleRecursive(state, node, visible);
    refreshSceneTreeFolderVisibility(state);
    notifySceneEdit(state);
}

void setGroupVisible(UiGroupState& group, bool visible)
{
    group.visible = visible;
}

void toggleGroupVisible(UiGroupState& group)
{
    setGroupVisible(group, !group.visible);
}

void setGroupVisible(UiFileState& file, UiGroupState& group, bool visible)
{
    setGroupVisible(group, visible);
    if (visible) {
        file.fileSettings.visible = true;
        return;
    }

    if (countVisibleGroups(file.groupSettings) == 0u) {
        file.fileSettings.visible = false;
    }
}

void toggleGroupVisible(UiFileState& file, UiGroupState& group)
{
    setGroupVisible(file, group, !group.visible);
}

void setGroupVisible(UiState& state, UiFileState& file, UiGroupState& group, bool visible)
{
    setGroupVisible(file, group, visible);
    refreshSceneTreeFolderVisibility(state);
    notifySceneEdit(state);
}

void toggleGroupVisible(UiState& state, UiFileState& file, UiGroupState& group)
{
    setGroupVisible(state, file, group, !group.visible);
}

void setShowOrigin(UiState& state, bool visible)
{
    if (state.showOrigin != visible) {
        state.showOrigin = visible;
        markSceneDirty(state);
    }
}

void toggleShowOrigin(UiState& state)
{
    setShowOrigin(state, !state.showOrigin);
}

void setShowGrid(UiState& state, bool visible)
{
    if (state.showGrid != visible) {
        state.showGrid = visible;
        markSceneDirty(state);
    }
}

void toggleShowGrid(UiState& state)
{
    setShowGrid(state, !state.showGrid);
}

void setShowDimensions(UiState& state, bool visible)
{
    if (state.showDimensions != visible) {
        state.showDimensions = visible;
        markSceneDirty(state);
    }
}

void setSceneUpAxis(UiState& state, SceneUpAxis upAxis)
{
    if (state.upAxis != upAxis) {
        state.upAxis = upAxis;
        frameCameraToScene(state);
        markSceneDirty(state);
    }
}

void toggleSceneUpAxis(UiState& state)
{
    setSceneUpAxis(
        state,
        state.upAxis == SceneUpAxis::z ? SceneUpAxis::y : SceneUpAxis::z);
}

void setMasterVertexPointSize(UiState& state, float value)
{
    const float clampedValue = clampFinite(
        value,
        minVertexPointSize,
        maxVertexPointSize,
        defaultMasterVertexPointSize);
    if (state.masterVertexPointSize != clampedValue) {
        state.masterVertexPointSize = clampedValue;
        markSceneDirty(state);
    }
}

void setFileVertexSizeScale(UiFileState& file, float value)
{
    file.vertexSizeScale = clampFinite(value, minVertexSizeScale, maxVertexSizeScale, 1.0f);
}

void setGroupVertexSizeScale(UiGroupState& group, float value)
{
    group.vertexSizeScale = clampFinite(value, minVertexSizeScale, maxVertexSizeScale, 1.0f);
}

void setFileTranslation(UiFileSettings& settings, const std::array<float, 3>& value)
{
    settings.translation = finiteArrayOrZero(value);
}

void setSceneNodeTranslation(UiSceneNodeSettings& settings, const std::array<float, 3>& value)
{
    settings.translation = finiteArrayOrZero(value);
}

void setGroupTranslation(UiGroupState& group, const std::array<float, 3>& value)
{
    group.translation = finiteArrayOrZero(value);
}

void setFileRotationDegrees(UiFileSettings& settings, const std::array<float, 3>& value)
{
    settings.rotationDegrees = clampFiniteArray(value, minRotationDegrees, maxRotationDegrees);
}

void setSceneNodeRotationDegrees(UiSceneNodeSettings& settings, const std::array<float, 3>& value)
{
    settings.rotationDegrees = clampFiniteArray(value, minRotationDegrees, maxRotationDegrees);
}

void setGroupRotationDegrees(UiGroupState& group, const std::array<float, 3>& value)
{
    group.rotationDegrees = clampFiniteArray(value, minRotationDegrees, maxRotationDegrees);
}

void setFileScale(UiFileSettings& settings, float value)
{
    settings.scale = clampFinite(value, minGroupScale, maxGroupScale, 1.0f);
}

void setSceneNodeScale(UiSceneNodeSettings& settings, float value)
{
    settings.scale = clampFinite(value, minGroupScale, maxGroupScale, 1.0f);
}

void setGroupScale(UiGroupState& group, float value)
{
    group.scale = clampFinite(value, minGroupScale, maxGroupScale, 1.0f);
}

void setFileOpacity(UiFileSettings& settings, float value)
{
    settings.opacity = clampFinite(value, minGroupOpacity, maxGroupOpacity, 1.0f);
}

void setSceneNodeOpacity(UiSceneNodeSettings& settings, float value)
{
    settings.opacity = clampFinite(value, minGroupOpacity, maxGroupOpacity, 1.0f);
}

void setGroupOpacity(UiGroupState& group, float value)
{
    group.opacity = clampFinite(value, minGroupOpacity, maxGroupOpacity, 1.0f);
}

void setGroupColor(UiGroupState& group, const std::array<float, 4>& value)
{
    group.color = {
        clampFinite(value[0], 0.0f, 1.0f, 1.0f),
        clampFinite(value[1], 0.0f, 1.0f, 1.0f),
        clampFinite(value[2], 0.0f, 1.0f, 1.0f),
        clampFinite(value[3], 0.0f, 1.0f, 1.0f),
    };
}

void resetGroupColor(UiGroupState& group, size_t colorIndex)
{
    group.color = defaultGroupColor(colorIndex);
}

void resetGroupTransform(UiGroupState& group)
{
    group.scale = 1.0f;
    group.opacity = 1.0f;
    group.translation = {};
    group.rotationDegrees = {};
}

void resetFileTransform(UiFileSettings& settings)
{
    settings.scale = 1.0f;
    settings.opacity = 1.0f;
    settings.translation = {};
    settings.rotationDegrees = {};
}

void resetSceneNodeTransform(UiSceneNodeSettings& settings)
{
    settings.scale = 1.0f;
    settings.opacity = 1.0f;
    settings.translation = {};
    settings.rotationDegrees = {};
}

bool groupTransformIsDefault(const UiGroupState& group)
{
    return group.scale == 1.0f
        && group.opacity == 1.0f
        && group.translation == std::array<float, 3>{}
        && group.rotationDegrees == std::array<float, 3>{};
}

bool fileTransformIsDefault(const UiFileSettings& settings)
{
    return settings.scale == 1.0f
        && settings.opacity == 1.0f
        && settings.translation == std::array<float, 3>{}
        && settings.rotationDegrees == std::array<float, 3>{};
}

bool sceneNodeTransformIsDefault(const UiSceneNodeSettings& settings)
{
    return settings.scale == 1.0f
        && settings.opacity == 1.0f
        && settings.translation == std::array<float, 3>{}
        && settings.rotationDegrees == std::array<float, 3>{};
}

void recalculateSceneBounds(UiState& state)
{
    state.sceneBounds = combineBounds(state.files, state.sceneNodes);
    std::vector<Vertex> corners;
    if (countVisibleSceneGroups(state) != 0) {
        corners.resize(2);
        corners[0].position = state.sceneBounds.min;
        corners[1].position = state.sceneBounds.max;
    }
    bool hasComparisonBounds = false;
    for (const auto& comparison : state.comparisons) {
        if (!comparison.settings.enabled) { continue; }
        if (const auto bounds = comparisonDisplayBounds(state, comparison.objectId)) {
            hasComparisonBounds = true;
            Vertex first, last;
            first.position = bounds->min;
            last.position = bounds->max;
            corners.push_back(first);
            corners.push_back(last);
        }
    }
    if (hasComparisonBounds) { state.sceneBounds = calculateBounds(corners); }
}

void frameCameraToScene(UiState& state)
{
    state.camera = frameCameraBounds(state.sceneBounds, state.upAxis);
}

void setCameraView(UiState& state, CameraView view)
{
    state.camera = cameraWithView(state.camera, view);
}

void fitCameraToScene(UiState& state)
{
    state.camera = fitCameraBounds(state.camera, state.sceneBounds);
}

void fitCameraToSelection(UiState& state)
{
    if (const auto bounds = selectedSceneBounds(state)) {
        state.camera = fitCameraBounds(state.camera, *bounds);
    }
}

void appendFolderTreeSceneNode(
    UiState& state,
    const std::filesystem::path& root,
    size_t firstFileIndex,
    size_t fileCount)
{
    UiSceneNode rootNode;
    rootNode.kind = UiSceneNodeKind::folder;
    rootNode.name = folderDisplayName(root);

    const std::filesystem::path absoluteRoot = std::filesystem::absolute(root).lexically_normal();
    for (size_t offset = 0; offset < fileCount; ++offset) {
        const size_t fileIndex = firstFileIndex + offset;
        if (fileIndex >= state.files.size()) {
            break;
        }

        const std::filesystem::path absoluteFile =
            std::filesystem::absolute(state.files[fileIndex].path).lexically_normal();
        std::filesystem::path relativePath = absoluteFile.lexically_relative(absoluteRoot);
        if (relativePath.empty()) {
            relativePath = absoluteFile.filename();
        }

        std::vector<UiSceneNode>* children = &rootNode.children;
        for (const auto& part : relativePath.parent_path()) {
            const std::string name = part.string();
            if (name.empty() || name == "." || name == "..") {
                continue;
            }
            children = &ensureFolderSceneNode(*children, name).children;
        }

        children->push_back(createFileSceneNode(state.files[fileIndex], fileIndex));
    }

    state.sceneNodes.push_back(std::move(rootNode));
    assignSceneObjectIds(state);
    refreshSceneTreeFolderCenters(state);
    notifySceneEdit(state);
}

bool removeFileFromState(UiState& state, size_t fileIndex)
{
    if (fileIndex >= state.files.size()) {
        return false;
    }

    state.files.erase(state.files.begin() + static_cast<std::ptrdiff_t>(fileIndex));
    state.sceneNodes.erase(
        std::remove_if(
            state.sceneNodes.begin(),
            state.sceneNodes.end(),
            [fileIndex](UiSceneNode& node) {
                return !pruneRemovedFile(node, fileIndex);
            }),
        state.sceneNodes.end());
    std::erase_if(state.selectedSceneObjects, [&state](SceneObjectId id) {
        return !findSceneObject(state, id).has_value();
    });
    pruneMissingViewReferences(state);
    recalculateSceneBounds(state);
    frameCameraToScene(state);
    markSceneDirty(state);
    return true;
}

UiState prepareSceneReplacement(const UiState& current,
    std::vector<UiFileState> files, const SceneDocument& document)
{
    UiState prepared;
    prepared.running = current.running;
    prepared.uiScale = current.uiScale;
    prepared.viewerPaneWidth = current.viewerPaneWidth;
    prepared.viewerPaneVisible = current.viewerPaneVisible;
    prepared.propertiesPaneVisible = current.propertiesPaneVisible;
    prepared.propertiesPaneWidth = current.propertiesPaneWidth;
    prepared.nextObjectId = current.nextObjectId;
    prepared.nextViewId = current.nextViewId;
    prepared.sceneGeneration = current.sceneGeneration + 1;
    prepared.files = std::move(files);
    // Every replacement receives fresh IDs, even when reopening the same file.
    for (auto& file : prepared.files) {
        file.objectId = invalidSceneObjectId;
        for (auto& group : file.groupSettings) {
            group.objectId = invalidSceneObjectId;
        }
    }
    applySceneNodeRecords(prepared, document.nodes);
    setSceneUpAxis(prepared, document.upAxis);
    setShowOrigin(prepared, document.showOrigin);
    setShowGrid(prepared, document.showGrid);
    setShowDimensions(prepared, document.showDimensions);
    setMasterVertexPointSize(prepared, document.masterVertexPointSize);
    for (const auto& record : document.comparisons) {
        UiComparison comparison;
        comparison.name = record.name.empty() ? "Analysis" : record.name;
        comparison.settings = normalizedComparisonSettings(record.settings);
        comparison.translation = record.translation && finitePosition(*record.translation)
            ? *record.translation : std::array<float, 3>{};
        const auto loadParts = [&](const std::vector<SceneComparisonPartRecord>& references) {
            std::vector<UiComparisonPart> result;
            for (const auto& reference : references) {
                UiComparisonPart part{invalidSceneObjectId, reference.name, reference.enabled};
                if (reference.fileIndex >= 0 && static_cast<size_t>(reference.fileIndex) < prepared.files.size()
                    && reference.groupIndex >= 0) {
                    const auto f = static_cast<size_t>(reference.fileIndex);
                    const auto g = static_cast<size_t>(reference.groupIndex);
                    const auto& file = prepared.files[f];
                    if (comparablePart(file, g) && f < document.files.size() && g < document.files[f].groups.size()
                        && file.mesh.nodes[g].name == document.files[f].groups[g].name) {
                        part.objectId = file.groupSettings[g].objectId;
                    }
                }
                if (part.objectId == invalidSceneObjectId || std::none_of(result.begin(), result.end(), [&](const auto& other) {
                    return other.objectId == part.objectId;
                })) { result.push_back(std::move(part)); }
            }
            return result;
        };
        comparison.a = loadParts(record.a);
        comparison.b = loadParts(record.b);
        prepared.comparisons.push_back(std::move(comparison));
    }
    assignSceneObjectIds(prepared);
    for (size_t index = 0; index < document.comparisons.size(); ++index) {
        if (!document.comparisons[index].translation) {
            auto& comparison = prepared.comparisons[index];
            comparison.translation = initialComparisonTranslation(prepared, comparison.objectId);
        }
    }
    loadSceneAnnotations(prepared, document);
    loadSceneViews(prepared, document);
    if (!prepared.comparisons.empty()) { prepared.activeComparisonId = prepared.comparisons.front().objectId; }
    recalculateSceneBounds(prepared);
    prepared.camera = document.camera ? normalizedSceneCamera(*document.camera)
        : frameCameraBounds(prepared.sceneBounds, prepared.upAxis);
    clearSceneDirty(prepared);
    return prepared;
}

void setSceneDirty(UiState& state, bool dirty)
{
    state.isDirty = dirty;
}

void notifySceneEdit(UiState& state)
{
    ++state.sceneEditRevision;
}

void markSceneDirty(UiState& state)
{
    for (auto& comparison : state.comparisons) {
        if (comparison.diagnosticFocus && !diagnosticFocusCurrent(state, comparison)) {
            comparison.diagnosticFocus.reset();
        }
    }
    notifySceneEdit(state);
    setSceneDirty(state, true);
}

void clearSceneDirty(UiState& state)
{
    setSceneDirty(state, false);
}

void updateSceneDirty(UiState& state, const SceneDocument& cleanDocument)
{
    setSceneDirty(state, !sceneContentEqual(createSceneDocument(state), cleanDocument));
}

void setViewerPaneWidth(UiState& state, float value, float minWidth, float maxWidth)
{
    state.viewerPaneWidth = std::clamp(
        finiteOr(value, minWidth),
        minWidth,
        std::max(minWidth, maxWidth));
}

void setPropertiesPaneWidth(UiState& state, float value, float minWidth, float maxWidth)
{
    state.propertiesPaneWidth = std::clamp(
        finiteOr(value, minWidth), minWidth, std::max(minWidth, maxWidth));
}

void setViewerPaneVisible(UiState& state, bool visible)
{
    state.viewerPaneVisible = visible;
}

void toggleViewerPaneVisible(UiState& state)
{
    setViewerPaneVisible(state, !state.viewerPaneVisible);
}

void requestQuit(UiState& state)
{
    state.running = false;
}

void setCameraOrbiting(UiState& state, bool enabled)
{
    state.cameraInput.orbiting = enabled;
}

void setCameraRolling(UiState& state, bool enabled)
{
    state.cameraInput.rolling = enabled;
}

void setCameraPanning(UiState& state, bool enabled)
{
    state.cameraInput.panning = enabled;
}

void orbitUiCamera(UiState& state, float deltaX, float deltaY)
{
    orbitCamera(state.camera, deltaX, deltaY, state.upAxis);
}

void rollUiCamera(UiState& state, float deltaX)
{
    rollCamera(state.camera, deltaX);
}

void panUiCamera(UiState& state, float deltaX, float deltaY, float viewportHeight)
{
    panCamera(state.camera, deltaX, deltaY, viewportHeight, state.upAxis);
}

void dollyUiCamera(UiState& state, float amount)
{
    dollyCamera(state.camera, amount);
}

void setUiCamera(UiState& state, const CameraPlacement& placement)
{
    state.camera = cameraWithPlacement(state.camera, placement);
}

void lookAtUiCamera(UiState& state, const std::array<float, 3>& eye, const std::array<float, 3>& target)
{
    state.camera = cameraLookingAt(state.camera, eye, target, state.upAxis);
}

void frameCameraToObject(UiState& state, SceneObjectId object)
{
    const auto bounds = sceneObjectBounds(state, {object});
    if (!bounds) { throw std::invalid_argument("Object has no visible geometry to frame."); }
    state.camera = fitCameraBounds(state.camera, *bounds);
}

void navigateUiCamera(UiState& state, const CameraNavigation& navigation)
{
    for (float value : {navigation.yawDegrees, navigation.pitchDegrees, navigation.rollDegrees,
        navigation.right, navigation.up, navigation.forward, navigation.distanceFactor}) {
        if (!std::isfinite(value)) { throw std::invalid_argument("Camera navigation requires finite values."); }
    }
    if (navigation.distanceFactor <= 0) { throw std::invalid_argument("Camera distance factor must be positive."); }
    constexpr float degreesToRadians = 0.017453292519943295f;
    auto camera = state.camera;
    const float yawSign = state.upAxis == SceneUpAxis::y ? -1.0f : 1.0f;
    orbitCamera(camera, std::fmod(navigation.yawDegrees, 360.0f) * degreesToRadians / (0.006f * yawSign),
        std::clamp(navigation.pitchDegrees, -180.0f, 180.0f) * degreesToRadians / 0.006f, state.upAxis);
    rollCamera(camera, std::fmod(navigation.rollDegrees, 360.0f) * degreesToRadians / 0.006f);
    moveCameraLocal(camera, navigation.right, navigation.up, navigation.forward, state.upAxis);
    camera.distance = std::max(0.001f, camera.distance * navigation.distanceFactor);
    const auto eye = cameraEye(camera, state.upAxis);
    for (float value : {camera.target[0], camera.target[1], camera.target[2], camera.distance,
        camera.yawRadians, camera.pitchRadians, camera.rollRadians, eye.x, eye.y, eye.z}) {
        if (!std::isfinite(value)) { throw std::invalid_argument("Camera navigation exceeds the finite coordinate range."); }
    }
    state.camera = camera;
}

} // namespace woby
