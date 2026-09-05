#pragma once

#include "ui_state.h"
#include "scene_lifecycle_types.h"
#include <optional>

namespace woby {

// Runtime persistence metadata is deliberately outside UiState.
std::optional<SceneLifecycleError> saveSceneState(UiState& state,
    std::optional<std::filesystem::path>& currentPath, SceneDocument& cleanDocument,
    const std::filesystem::path& requestedPath, bool overwrite);

// Performs the save/dirty gate; the adapter then opens, clears, or shuts down.
std::optional<SceneLifecycleError> beginSceneLifecycle(const SceneLifecycleCommand& command,
    UiState& state, std::optional<std::filesystem::path>& currentPath,
    SceneDocument& cleanDocument, bool busy);

} // namespace woby
