#include "scene_history.h"
#include "ui_operations.h"

#include <algorithm>
#include <stdexcept>

namespace woby {
namespace {

std::vector<SceneObjectId> sceneIdentities(const UiState& state)
{
    std::vector<SceneObjectId> result;
    for (const auto& object : sceneObjects(state)) { result.push_back(object.id); }
    // Missing comparison references must also retain their original identities.
    for (const auto& comparison : state.comparisons) {
        for (const auto& part : comparison.a) { result.push_back(part.objectId); }
        for (const auto& part : comparison.b) { result.push_back(part.objectId); }
    }
    return result;
}

SceneSnapshot snapshot(const UiState& state, SceneDocument document,
    std::vector<SceneObjectId> identities)
{
    SceneSnapshot result;
    document.camera.reset();
    result.document = std::move(document);
    result.identities = std::move(identities);
    auto& content = result.content;
    content.sceneGeneration = state.sceneGeneration;
    content.showOrigin = state.showOrigin;
    content.showGrid = state.showGrid;
    content.upAxis = state.upAxis;
    content.masterVertexPointSize = state.masterVertexPointSize;
    content.comparisons = state.comparisons;
    content.sceneNodes = state.sceneNodes;
    for (const auto& file : state.files) {
        UiFileState metadata;
        metadata.path = file.path;
        metadata.importerId = file.importerId;
        metadata.groupSettings = file.groupSettings;
        metadata.fileSettings = file.fileSettings;
        metadata.vertexSizeScale = file.vertexSizeScale;
        metadata.objectId = file.objectId;
        content.files.push_back(std::move(metadata));

    }
    return result;
}

} // namespace

void resetSceneHistory(SceneHistory& history, const UiState& state)
{
    SceneHistory fresh;
    fresh.snapshots.push_back(snapshot(state, createSceneDocument(state), sceneIdentities(state)));
    fresh.recordedRevision = state.sceneEditRevision;
    history = std::move(fresh);
}

void finishSceneHistoryInteraction(SceneHistory& history)
{
    history.interaction = 0;
    history.coalescing = false;
}

bool recordSceneHistory(SceneHistory& history, const UiState& state, uint64_t interaction)
{
    if (history.snapshots.empty()
        || history.snapshots[history.cursor].content.sceneGeneration != state.sceneGeneration) {
        resetSceneHistory(history, state);
        return true;
    }
    const bool merge = history.coalescing && history.interaction != 0
        && (interaction == history.interaction || interaction == 0);
    const bool notified = history.recordedRevision != state.sceneEditRevision;
    bool changed = false;
    if (notified) {
        const auto& previous = history.snapshots[history.cursor];
        auto document = createSceneDocument(state);
        auto identities = sceneIdentities(state);
        changed = !sceneContentEqual(document, previous.document) || identities != previous.identities;
        if (changed) {
            auto next = snapshot(state, std::move(document), std::move(identities));
            history.snapshots.erase(history.snapshots.begin() + static_cast<std::ptrdiff_t>(history.cursor + 1),
                history.snapshots.end());
            if (merge) {
                history.snapshots[history.cursor] = std::move(next);
            } else {
                history.snapshots.push_back(std::move(next));
                ++history.cursor;
            }
            history.coalescing = interaction != 0;
        }
        history.recordedRevision = state.sceneEditRevision;
    }
    if (!changed && !merge) {
        history.coalescing = false;
    }
    history.interaction = interaction;
    if (interaction == 0) {
        finishSceneHistoryInteraction(history);
        // A drag returning to its starting value is not an undo action.
        if (merge && history.cursor > 0
            && history.snapshots[history.cursor].document == history.snapshots[history.cursor - 1].document
            && history.snapshots[history.cursor].identities == history.snapshots[history.cursor - 1].identities) {
            history.snapshots.pop_back();
            --history.cursor;
        }
    }
    return notified;
}

bool canUndoScene(const SceneHistory& history) { return history.cursor > 0; }
bool canRedoScene(const SceneHistory& history) { return history.cursor + 1 < history.snapshots.size(); }

std::optional<UiState> prepareSceneHistoryStep(const SceneHistory& history,
    const UiState& current, const SceneDocument& cleanDocument, bool redo,
    std::vector<UiFileState> reloaded)
{
    if (!(redo ? canRedoScene(history) : canUndoScene(history))) { return {}; }
    const auto& target = history.snapshots[redo ? history.cursor + 1 : history.cursor - 1];
    if (target.content.sceneGeneration != current.sceneGeneration) { return {}; }
    UiState prepared = target.content;
    for (size_t index = 0; index < prepared.files.size(); ++index) {
        auto& file = prepared.files[index];
        const auto live = std::find_if(current.files.begin(), current.files.end(),
            [&](const auto& candidate) { return candidate.objectId == file.objectId; });
        const auto loaded = std::find_if(reloaded.begin(), reloaded.end(),
            [&](const auto& candidate) { return candidate.objectId == file.objectId; });
        const auto* source = live != current.files.end() ? &*live
            : (loaded != reloaded.end() ? &*loaded : nullptr);
        if (!source) { throw std::runtime_error("Model must be reloaded: " + file.path.string()); }
        const auto& groups = target.document.files[index].groups;
        if (source->mesh.nodes.size() != groups.size() || source->groupSettings.size() != groups.size()) {
            throw std::runtime_error("Source model parts changed; cannot restore references: " + file.path.string());
        }
        for (size_t group = 0; group < groups.size(); ++group) {
            if (source->mesh.nodes[group].name != groups[group].name) {
                throw std::runtime_error("Source model parts changed; cannot restore references: " + file.path.string());
            }
            // Geometry may have changed since removal. Keep historical appearance,
            // transforms and IDs, but use the current source's geometric bounds.
            file.groupSettings[group].center = source->groupSettings[group].center;
            file.groupSettings[group].localBounds = source->groupSettings[group].localBounds;
            file.groupSettings[group].localBoundsValid = source->groupSettings[group].localBoundsValid;
        }
        file.fileSettings.center = source->fileSettings.center;
        if (live != current.files.end()) { file.mesh = live->mesh; }
        else { file.mesh = std::move(loaded->mesh); }
    }
    prepared.running = current.running;
    prepared.sceneEditRevision = current.sceneEditRevision + 1;
    prepared.nextObjectId = current.nextObjectId;
    prepared.uiScale = current.uiScale;
    prepared.screenshotSettings = current.screenshotSettings;
    prepared.viewerPaneVisible = current.viewerPaneVisible;
    prepared.viewerPaneWidth = current.viewerPaneWidth;
    prepared.propertiesPaneVisible = current.propertiesPaneVisible;
    prepared.camera = current.camera;
    prepared.cameraInput = current.cameraInput;
    prepared.selectedSceneObjects = current.selectedSceneObjects;
    std::erase_if(prepared.selectedSceneObjects, [&](SceneObjectId id) { return !findSceneObject(prepared, id); });
    prepared.activeComparisonId = findComparison(prepared, current.activeComparisonId)
        ? current.activeComparisonId
        : (prepared.comparisons.empty() ? invalidSceneObjectId : prepared.comparisons.front().objectId);
    refreshSceneTreeFolderCenters(prepared);
    recalculateSceneBounds(prepared);
    updateSceneDirty(prepared, cleanDocument);
    return prepared;
}

void commitSceneHistoryStep(SceneHistory& history, UiState& state, UiState prepared, bool redo)
{
    state = std::move(prepared);
    history.recordedRevision = state.sceneEditRevision;
    if (redo) { ++history.cursor; } else { --history.cursor; }
    finishSceneHistoryInteraction(history);
}

void skipSceneHistoryStep(SceneHistory& history, const UiState& current, bool redo)
{
    if (!(redo ? canRedoScene(history) : canUndoScene(history))) { return; }
    const size_t next = redo ? history.cursor + 1 : history.cursor - 1;
    // Consume the failed action and use the unchanged live scene as its baseline.
    // Otherwise the next frame would record the failed undo as a new edit and
    // erase the redo branch.
    history.snapshots[next] = snapshot(current, createSceneDocument(current), sceneIdentities(current));
    history.cursor = next;
    history.recordedRevision = current.sceneEditRevision;
    finishSceneHistoryInteraction(history);
}

} // namespace woby
