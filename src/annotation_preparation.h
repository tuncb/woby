#pragma once

#include "ui_state.h"
#include <atomic>
#include <memory>
#include <thread>

namespace woby {
struct AnnotationPreparationJob {
    const Vertex* vertices = nullptr;
    const uint32_t* indices = nullptr;
    size_t vertexCount = 0, indexCount = 0;
    std::shared_ptr<const MeshAnnotationCache> result;
    std::string error;
    std::atomic<bool> done = false;
    // Last member: stop/join before destroying anything used by the worker.
    std::jthread worker;
};
struct AnnotationPreparationRuntime {
    std::unique_ptr<AnnotationPreparationJob> job;
    std::string error;
};
// Main-thread adapter. Call after scene commit; only this function publishes results.
void updateAnnotationPreparation(AnnotationPreparationRuntime& runtime, UiState& state);
// Required BEFORE geometry removal/replacement/mutation; cooperative cancellation
// joins the worker after at most one small block, not the full preparation pass.
void cancelAnnotationPreparation(AnnotationPreparationRuntime& runtime);
// Projection examines visible occluders too, so all visible model caches are needed.
[[nodiscard]] bool annotationPreparationReady(const UiState& state);
} // namespace woby
