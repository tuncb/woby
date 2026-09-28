#include "annotation_preparation.h"
#include "surface_annotation.h"
#include "ui_operations.h"
#include <algorithm>

namespace woby {
void cancelAnnotationPreparation(AnnotationPreparationRuntime& runtime)
{
    runtime.job.reset(); // jthread requests stop and joins before geometry can be released.
    runtime.error.clear();
}

bool annotationPreparationReady(const UiState& state)
{
    const bool missing = std::any_of(state.files.begin(), state.files.end(),
        [](const auto& file) { return !annotationMeshCacheReady(file.mesh); });
    if (missing) {
        for (const auto& part : scenePickParts(state)) {
            if (part.mesh && !annotationMeshCacheReady(*part.mesh)) { return false; }
        }
    }
    for (const auto& item : state.annotations) {
        if (item.targetPending) { return false; }
    }
    return true;
}

void updateAnnotationPreparation(AnnotationPreparationRuntime& runtime, UiState& state)
{
    if (runtime.job) {
        if (!runtime.job->done.load(std::memory_order_acquire)) { return; }
        const auto& job = *runtime.job;
        for (auto& file : state.files) {
            auto& mesh = file.mesh;
            if (mesh.vertices.data() == job.vertices && mesh.indices.data() == job.indices
                && mesh.vertices.size() == job.vertexCount && mesh.indices.size() == job.indexCount) {
                mesh.annotationCache = job.result;
            }
        }
        runtime.error = job.error;
        runtime.job.reset();
        validateAnnotationTargets(state);
    }
    if (!runtime.error.empty()) { return; }
    for (const auto& file : state.files) {
        const auto& mesh = file.mesh;
        if (annotationMeshCacheReady(mesh)) { continue; }
        auto job = std::make_unique<AnnotationPreparationJob>();
        job->vertices = mesh.vertices.data(); job->indices = mesh.indices.data();
        job->vertexCount = mesh.vertices.size(); job->indexCount = mesh.indices.size();
        // Spans survive moves of Mesh/UiFileState (e.g. append/reallocation).
        const std::span<const Vertex> vertices = mesh.vertices;
        const std::span<const uint32_t> indices = mesh.indices;
        const std::span<const MeshNode> nodes = mesh.nodes;
        try {
            job->worker = std::jthread([task = job.get(), vertices, indices, nodes](std::stop_token stop) {
                try {
                    task->result = buildAnnotationMeshCache(vertices, indices, nodes, [&] { return stop.stop_requested(); });
                } catch (const std::exception& error) { task->error = error.what(); }
                catch (...) { task->error = "Annotation preparation failed."; }
                task->done.store(true, std::memory_order_release);
            });
            runtime.job = std::move(job);
        } catch (const std::exception& error) { runtime.error = error.what(); }
        return; // One worker limits memory bandwidth contention while navigating.
    }
}
} // namespace woby
