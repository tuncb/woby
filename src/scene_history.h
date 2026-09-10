#pragma once

#include "ui_state.h"

#include <deque>

namespace woby {

// Logical scene metadata only: paths, importer IDs, settings and object IDs.
// No vertex/index buffers, GPU handles, source-file IO, or UI/timing objects.
struct SceneSnapshot {
    UiState content; // File meshes are empty; geometry belongs only to the live scene.
    SceneDocument document;
    std::vector<SceneObjectId> identities;
    // Navigation is restored only across an explicit Apply View action.
    std::optional<ViewApplication> viewApplication;
};

struct SceneHistory {
    std::deque<SceneSnapshot> snapshots;
    size_t cursor = 0;
    uint64_t interaction = 0;
    bool coalescing = false;
    uint64_t recordedRevision = 0;
};

void resetSceneHistory(SceneHistory& history, const UiState& state);
// Flush operation notifications. Unchanged revisions do no scene traversal or
// allocation. A nonzero token identifies a continuous interaction; zero records
// its final release frame, then closes the action even without a new edit.
// Returns true if notified content was inspected (including no-op edits).
bool recordSceneHistory(SceneHistory& history, const UiState& state, uint64_t interaction = 0);
void finishSceneHistoryInteraction(SceneHistory& history);
[[nodiscard]] bool canUndoScene(const SceneHistory& history);
[[nodiscard]] bool canRedoScene(const SceneHistory& history);
// Prepare first so runtime resources can be staged before committing either
// logical state or the history cursor. Supply reloaded files for absent objects.
// Failure leaves the live scene untouched; the adapter consumes it with skip below.
[[nodiscard]] std::optional<UiState> prepareSceneHistoryStep(const SceneHistory& history,
    const UiState& current, const SceneDocument& cleanDocument, bool redo,
    std::vector<UiFileState> reloaded = {});
void commitSceneHistoryStep(SceneHistory& history, UiState& state, UiState prepared, bool redo);

void skipSceneHistoryStep(SceneHistory& history, const UiState& current, bool redo);

} // namespace woby
