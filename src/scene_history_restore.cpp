#include "scene_history_restore.h"
#include <stdexcept>
#include <utility>

namespace woby {
SceneHistoryRestore::~SceneHistoryRestore() { cancelSceneHistoryRestore(*this); }

void cancelSceneHistoryRestore(SceneHistoryRestore& restore) {
    restore.canceled.store(true, std::memory_order_release);
    if (restore.worker.joinable()) { restore.worker.join(); }
    for (auto& upload : restore.uploads) { if (upload) { abortGpuMeshUpload(*upload); } }
    restore.uploads.clear(); restore.reuse.clear(); restore.result.reset(); restore.active = false;
}

bool beginSceneHistoryRestore(SceneHistoryRestore& restore, const SceneHistory& history,
    const UiState& state, const SceneDocument& cleanDocument, const graphics::VertexLayout& layout, bool redo) {
    if (restore.active || !(redo ? canRedoScene(history) : canUndoScene(history))) { return false; }
    cancelSceneHistoryRestore(restore);
    restore.canceled.store(false, std::memory_order_release);
    restore.prepared.store(false, std::memory_order_release);
    restore.error.clear(); restore.generation = state.sceneGeneration; restore.revision = state.sceneEditRevision;
    restore.cursor = history.cursor; restore.redo = redo;
    restore.worker = std::jthread([&restore, history, current = state, cleanDocument, layout, redo] {
        const auto canceled = [&] { return restore.canceled.load(std::memory_order_acquire); };
        try {
            restore.result = loadSceneHistoryStep(history, current, cleanDocument, redo, canceled);
            if (restore.result && !canceled()) {
                restore.uploads.resize(restore.result->files.size());
                restore.reuse.resize(restore.result->files.size(), invalidSceneNodeIndex);
                for (size_t index = 0; index < restore.result->files.size() && !canceled(); ++index) {
                    const auto& file = restore.result->files[index];
                    for (size_t old = 0; old < current.files.size(); ++old) {
                        if (current.files[old].objectId == file.objectId) { restore.reuse[index] = old; break; }
                    }
                    if (restore.reuse[index] == invalidSceneNodeIndex) {
                        auto mesh = prepareSceneMesh(file.mesh, requestedGpuMeshFeatures(file), canceled,
                            !file.mesh.preparationDeferred, !file.mesh.preparationDeferred);
                        if (!mesh) { break; }
                        restore.uploads[index] = beginGpuMeshUpload(std::move(*mesh), file.mesh, layout);
                    }
                }
            }
        } catch (const std::exception& error) { restore.error = error.what(); }
        restore.prepared.store(true, std::memory_order_release);
        restore.prepared.notify_all();
    });
    restore.active = true;
    return true;
}

std::optional<bool> pollSceneHistoryRestore(SceneHistoryRestore& restore, SceneHistory& history,
    UiState& state, std::vector<LoadedModelRuntime>& runtimes) {
    if (!restore.active) { return {}; }
    if (restore.generation != state.sceneGeneration || restore.revision != state.sceneEditRevision || restore.cursor != history.cursor) {
        cancelSceneHistoryRestore(restore); return false;
    }
    if (!restore.prepared.load(std::memory_order_acquire)) { return {}; }
    if (restore.worker.joinable()) { restore.worker.join(); }
    try {
        if (!restore.error.empty()) { throw std::runtime_error(restore.error); }
        if (!restore.result) { cancelSceneHistoryRestore(restore); return false; }
        for (auto& upload : restore.uploads) { if (upload && !pollGpuMeshUpload(*upload)) { return {}; } }
        const bool navigation = restore.redo ? history.snapshots[history.cursor + 1].viewApplication.has_value()
            : history.snapshots[history.cursor].viewApplication.has_value();
        refreshSceneHistoryRuntimeState(*restore.result, state, navigation);
        std::vector<LoadedModelRuntime> staged(restore.result->files.size());
        for (size_t index = 0; index < staged.size(); ++index) {
            if (restore.reuse[index] != invalidSceneNodeIndex) {
                staged[index] = std::exchange(runtimes[restore.reuse[index]], LoadedModelRuntime{});
            } else {
                staged[index].gpuMesh = std::move(restore.uploads[index]->mesh);
                staged[index].requestedFeatures = restore.uploads[index]->features;
                restore.uploads[index].reset();
            }
        }
        destroyModelRuntimes(runtimes); runtimes = std::move(staged);
        commitSceneHistoryStep(history, state, std::move(*restore.result), restore.redo);
        cancelSceneHistoryRestore(restore);
        return true;
    } catch (...) {
        skipSceneHistoryStep(history, state, restore.redo);
        cancelSceneHistoryRestore(restore);
        throw;
    }
}
}
