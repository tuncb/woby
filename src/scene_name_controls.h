#pragma once
#include "scene_objects.h"

namespace woby {
// Adapter-owned text/focus state. Never part of scene content or history.
struct SceneNameEdit {
    SceneObjectId objectId = 0;
    uint64_t generation = 0;
    std::string text, initialName, originalName;
    bool focus = false, invalid = false, active = false;
    int submittedFrame = -1;
};
enum class SceneNameInputResult { editing, apply, cancel };
void beginSceneNameEdit(SceneNameEdit& edit, const SceneObjectInfo& object, uint64_t generation, bool focus = true);
// Commit click-away edits to their original target; discard stale scene/object edits.
void synchronizeSceneNameEdit(UiState& state, SceneNameEdit& edit, bool finish = false);
// Draw only; the caller applies a completed buffer through ui_operations.
SceneNameInputResult drawSceneNameInput(const char* label, SceneNameEdit& edit, float width);
void drawSceneNameError(const SceneNameEdit& edit);
} // namespace woby
