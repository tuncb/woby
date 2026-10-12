#pragma once
#include "scene_history_load.h"
#include "scene_mesh_upload.h"
#include <atomic>
#include <thread>

namespace woby {
// Runtime transaction. Workers own snapshots; only publication edits the scene.
struct SceneHistoryRestore {
    std::atomic<bool> canceled = false, prepared = false;
    std::optional<UiState> result;
    std::vector<std::optional<GpuMeshUpload>> uploads;
    std::vector<size_t> reuse;
    std::string error;
    uint64_t generation = 0, revision = 0;
    size_t cursor = 0;
    bool redo = false, active = false;
    std::jthread worker;
    ~SceneHistoryRestore();
};
bool beginSceneHistoryRestore(SceneHistoryRestore& restore, const SceneHistory& history,
    const UiState& state, const SceneDocument& cleanDocument, const graphics::VertexLayout& layout, bool redo);
// Empty means pending. A value reports whether the transaction was committed.
std::optional<bool> pollSceneHistoryRestore(SceneHistoryRestore& restore, SceneHistory& history,
    UiState& state, std::vector<LoadedModelRuntime>& runtimes);
void cancelSceneHistoryRestore(SceneHistoryRestore& restore);
}
