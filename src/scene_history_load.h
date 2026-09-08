#pragma once

#include "scene_history.h"

namespace woby {

// Runtime adapter: load only files absent from the live scene, using their saved
// path and importer. Pure history operations validate and restore logical state.
[[nodiscard]] std::optional<UiState> loadSceneHistoryStep(const SceneHistory& history,
    const UiState& current, const SceneDocument& cleanDocument, bool redo);

} // namespace woby
