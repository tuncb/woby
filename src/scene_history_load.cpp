#include "scene_history_load.h"
#include "model_load.h"

#include <algorithm>
#include <stdexcept>

namespace woby {

std::optional<UiState> loadSceneHistoryStep(const SceneHistory& history,
    const UiState& current, const SceneDocument& cleanDocument, bool redo)
{
    if (!(redo ? canRedoScene(history) : canUndoScene(history))) { return {}; }
    const auto& target = history.snapshots[redo ? history.cursor + 1 : history.cursor - 1];
    if (target.content.sceneGeneration != current.sceneGeneration) { return {}; }
    std::vector<UiFileState> reloaded;
    for (const auto& file : target.content.files) {
        if (std::any_of(current.files.begin(), current.files.end(),
                [&](const auto& live) { return live.objectId == file.objectId; })) { continue; }
        try {
            auto imported = loadModel(file.path, file.importerId);
            auto restored = createUiFileState(file.path, std::move(imported.mesh), 0, std::move(imported.importerId));
            restored.objectId = file.objectId;
            reloaded.push_back(std::move(restored));
        } catch (const std::exception& error) {
            throw std::runtime_error("Could not reload " + file.path.string() + ": " + error.what());
        }
    }
    return prepareSceneHistoryStep(history, current, cleanDocument, redo, std::move(reloaded));
}

} // namespace woby
