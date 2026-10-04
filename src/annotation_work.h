#pragma once
#include "surface_annotation.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace woby {
struct AnnotationWork {
    std::stop_source cancel;
    std::function<void(std::stop_token)> compute;
    std::string error;
    std::atomic<bool> done = false;
};
// One active request and one replaceable queued request. Cancellation never
// joins on the UI thread; heavy input/scratch destruction happens on the worker.
struct AnnotationExecutor {
    std::mutex mutex;
    std::condition_variable_any wake;
    std::shared_ptr<AnnotationWork> pending, active;
    std::jthread worker;
};
void submitAnnotationWork(AnnotationExecutor& executor, const std::shared_ptr<AnnotationWork>& work);
void cancelAnnotationWork(const std::shared_ptr<AnnotationWork>& work);

struct AnnotationPartsSnapshot {
    std::vector<std::shared_ptr<const Mesh>> owners;
    std::vector<ScenePickPart> parts;
};
[[nodiscard]] AnnotationPartsSnapshot snapshotAnnotationParts(std::span<const ScenePickPart> parts);
struct AnnotationWorkIdentity {
    uint64_t generation = 0, revision = 0;
    SceneCamera camera;
    std::vector<SceneObjectId> selection;
    struct Source {
        const Vertex* vertices = nullptr;
        const uint32_t* indices = nullptr;
        size_t vertexCount = 0, indexCount = 0;
        std::shared_ptr<const MeshAnnotationCache> cache;
    };
    std::vector<Source> sources;
};
[[nodiscard]] AnnotationWorkIdentity annotationWorkIdentity(const UiState& state);
[[nodiscard]] bool annotationWorkCurrent(const AnnotationWorkIdentity& identity, const UiState& state,
    bool selection = false);
} // namespace woby
