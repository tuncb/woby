#include "annotation_work.h"
#include <algorithm>
#include <stdexcept>

namespace woby {
void cancelAnnotationWork(const std::shared_ptr<AnnotationWork>& work)
{
    if (work) { work->cancel.request_stop(); }
}
void submitAnnotationWork(AnnotationExecutor& executor, const std::shared_ptr<AnnotationWork>& work)
{
    if (!executor.worker.joinable()) {
        executor.worker = std::jthread([&executor](std::stop_token stop) {
            for (;;) {
                std::shared_ptr<AnnotationWork> task;
                {
                    std::unique_lock lock(executor.mutex);
                    executor.wake.wait(lock, stop, [&] { return executor.pending != nullptr; });
                    if (stop.stop_requested()) { return; }
                    task = std::move(executor.pending);
                    executor.active = task;
                }
                std::stop_callback shutdown(stop, [&] { task->cancel.request_stop(); });
                try {
                    if (!task->cancel.stop_requested()) { task->compute(task->cancel.get_token()); }
                } catch (const std::exception& error) { task->error = error.what(); }
                catch (...) { task->error = "Annotation computation failed."; }
                task->compute = {}; // Release owned inputs before advertising completion.
                task->done.store(true, std::memory_order_release);
                {
                    std::lock_guard lock(executor.mutex);
                    executor.active.reset();
                }
            }
        });
    }
    {
        std::lock_guard lock(executor.mutex);
        cancelAnnotationWork(executor.active);
        cancelAnnotationWork(executor.pending);
        executor.pending = work;
    }
    executor.wake.notify_one();
}
AnnotationPartsSnapshot snapshotAnnotationParts(std::span<const ScenePickPart> parts)
{
    AnnotationPartsSnapshot result;
    std::vector<const Mesh*> originals;
    size_t copyBytes = 0;
    // Check the whole request before copying: many small parts must not turn
    // the bounded fallback into an unbounded UI-thread scene copy.
    for (const auto& part : parts) {
        if (!part.mesh || std::find(originals.begin(), originals.end(), part.mesh) != originals.end()) { continue; }
        originals.push_back(part.mesh);
        if (!annotationMeshSnapshotReady(*part.mesh)) {
            copyBytes += part.mesh->vertices.size() * sizeof(Vertex) + part.mesh->indices.size() * sizeof(uint32_t);
            if (copyBytes > annotationInlineSnapshotBytes) { throw std::runtime_error("Annotation geometry is still being prepared."); }
        }
    }
    originals.clear();
    for (auto part : parts) {
        if (!part.mesh) { continue; }
        const auto found = std::find(originals.begin(), originals.end(), part.mesh);
        size_t index = static_cast<size_t>(found - originals.begin());
        if (found == originals.end()) {
            const auto& mesh = *part.mesh;
            std::shared_ptr<const Mesh> owner;
            if (annotationMeshSnapshotReady(mesh)) { owner = mesh.annotationCache->snapshot; }
            if (!owner) {
                if (mesh.indices.size() >= 50000 * 3) { throw std::runtime_error("Annotation geometry is still being prepared."); }
                auto small = std::make_shared<Mesh>();
                small->vertices = mesh.vertices; small->indices = mesh.indices; small->nodes = mesh.nodes;
                owner = std::move(small);
            }
            originals.push_back(part.mesh); result.owners.push_back(std::move(owner));
        }
        part.mesh = result.owners[index].get();
        part.diagnosticEdges = {}; // No borrowed scene storage crosses the worker boundary.
        result.parts.push_back(part);
    }
    return result;
}
AnnotationWorkIdentity annotationWorkIdentity(const UiState& state)
{
    AnnotationWorkIdentity result;
    result.generation = state.sceneGeneration; result.revision = state.sceneEditRevision;
    result.camera = state.camera; result.selection = state.selectedSceneObjects;
    for (const auto& file : state.files) {
        const auto& mesh = file.mesh;
        result.sources.push_back({mesh.vertices.data(), mesh.indices.data(), mesh.vertices.size(), mesh.indices.size(), mesh.annotationCache});
    }
    return result;
}
bool annotationWorkCurrent(const AnnotationWorkIdentity& identity, const UiState& state, bool selection)
{
    if (identity.generation != state.sceneGeneration || identity.revision != state.sceneEditRevision
        || identity.camera != state.camera || (selection && identity.selection != state.selectedSceneObjects)
        || identity.sources.size() != state.files.size()) { return false; }
    for (size_t i = 0; i < identity.sources.size(); ++i) {
        const auto& source = identity.sources[i]; const auto& mesh = state.files[i].mesh;
        if (source.vertices != mesh.vertices.data() || source.indices != mesh.indices.data()
            || source.vertexCount != mesh.vertices.size() || source.indexCount != mesh.indices.size()
            // Publishing the first cache does not change a small owned snapshot.
            // Replacing/invalidating an existing cache still rejects old work.
            || (source.cache && source.cache != mesh.annotationCache)) { return false; }
    }
    return true;
}
} // namespace woby
