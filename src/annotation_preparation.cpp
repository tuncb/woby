#include "annotation_preparation.h"
#include "surface_annotation.h"
#include "ui_operations.h"
#include <algorithm>

namespace woby {
void cancelAnnotationPreparation(AnnotationPreparationRuntime& runtime)
{
    runtime.job.reset(); // Stop and join before destroying the job.
    runtime.error.clear();
}

bool annotationPreparationReady(const UiState& state)
{
    if (!scenePostLoadReady(state)) { return false; }
    const bool missing = std::any_of(state.files.begin(), state.files.end(),
        [](const auto& file) { return !annotationMeshSnapshotReady(file.mesh); });
    if (missing) {
        size_t copyBytes = 0;
        std::vector<const Mesh*> meshes;
        for (const auto& part : scenePickParts(state)) {
            if (part.mesh && !annotationMeshCacheReady(*part.mesh)) { return false; }
            if (part.mesh && part.mesh->draft
                && std::find(meshes.begin(), meshes.end(), part.mesh) == meshes.end()) {
                meshes.push_back(part.mesh);
                copyBytes += meshGeometry(*part.mesh).vertices.size() * sizeof(Vertex) + meshGeometry(*part.mesh).indices.size() * sizeof(uint32_t);
                if (copyBytes > annotationInlineSnapshotBytes) { return false; }
            }
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
            if (job.owner == &state && job.generation == state.sceneGeneration
                && job.fileId == file.objectId && job.contentRevision == mesh.contentRevision
                && meshGeometry(mesh).vertices.data() == job.vertices && meshGeometry(mesh).indices.data() == job.indices
                && meshGeometry(mesh).vertices.size() == job.vertexCount && meshGeometry(mesh).indices.size() == job.indexCount) {
                mesh.annotationCache = job.result;
            }
        }
        runtime.error = job.error;
        runtime.job.reset();
        validateAnnotationTargets(state);
    }
    if (!runtime.error.empty()) { return; }
    for (auto& file : state.files) {
        auto& mesh = file.mesh;
        if (!postLoadReady(file) || meshGeometry(mesh).indices.empty() || meshGeometry(mesh).nodes.empty() || annotationMeshSnapshotReady(mesh)) { continue; }
        publishMeshGeometry(mesh);
        auto job = std::make_unique<AnnotationPreparationJob>();
        job->owner = &state; job->generation = state.sceneGeneration;
        job->fileId = file.objectId; job->contentRevision = mesh.contentRevision;
        job->vertices = meshGeometry(mesh).vertices.data(); job->indices = meshGeometry(mesh).indices.data();
        job->vertexCount = meshGeometry(mesh).vertices.size(); job->indexCount = meshGeometry(mesh).indices.size();
        try {
            job->worker = std::jthread([task = job.get(), input = mesh](std::stop_token stop) mutable {
                try {
                    task->result = buildAnnotationMeshCache(std::move(input), [&] { return stop.stop_requested(); });
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
