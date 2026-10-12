#pragma once

#include "scene_renderer.h"
#include <atomic>
#include <optional>
#include <string_view>
#include <thread>

namespace woby {

struct PostLoadGroupBounds {
    Bounds local;
    std::array<Coordinate, 2> original{};
};
struct PostLoadData {
    std::array<Coordinate, 2> originalBounds{};
    std::vector<PostLoadGroupBounds> groups;
    std::shared_ptr<const SourceMeshData> source;
    std::shared_ptr<const MeshAnnotationCache> annotations;
    SceneMeshPreparation points;
};
// Deterministic CPU preparation over an owned immutable mesh snapshot.
[[nodiscard]] std::optional<PostLoadData> preparePostLoadData(
    const Mesh& mesh, const std::function<bool()>& canceled = {});

struct PostLoadIdentity {
    const UiState* owner = nullptr;
    uint64_t generation = 0, revision = 0;
    SceneObjectId fileId = invalidSceneObjectId;
    std::shared_ptr<const MeshGeometry> geometry;
};
[[nodiscard]] PostLoadIdentity postLoadIdentity(const UiState& state, const UiFileState& file);
[[nodiscard]] bool postLoadCurrent(const PostLoadIdentity& identity, const UiState& state);

struct PostLoadJob {
    PostLoadIdentity identity;
    std::optional<PostLoadData> result;
    std::string error;
    std::atomic<bool> done = false;
    std::jthread worker; // Last: stop/join before destroying worker-owned output.
};
struct PostLoadFrame {
    PostLoadIdentity identity;
    uint64_t token = 0;
};
struct PostLoadRuntime {
    ~PostLoadRuntime();
    std::vector<PostLoadFrame> frames;
    std::unique_ptr<PostLoadJob> job;
    std::vector<std::unique_ptr<PostLoadJob>> retired;
    std::optional<GpuMeshUpload> upload;
    uint64_t noticeGeneration = 0, completions = 0, noticeCompletions = 0;
    const UiState* noticeOwner = nullptr;
    bool noticePending = false, noticeReady = false;
    double readyAt = 0;
};

// Record only frames submitted after scene publication. Completion is polled separately.
void notePostLoadFrame(PostLoadRuntime& runtime, const UiState& state, uint64_t token);
void updatePostLoading(PostLoadRuntime& runtime, UiState& state,
    std::vector<LoadedModelRuntime>& models, const graphics::VertexLayout& layout,
    uint64_t completedFrame);
void cancelPostLoading(PostLoadRuntime& runtime);
// Time is supplied by the adapter, so notice transitions can be tested without sleeps.
void updatePostLoadNotice(PostLoadRuntime& runtime, const UiState& state, double now);
[[nodiscard]] std::string_view postLoadNotice(const PostLoadRuntime& runtime, const UiState& state, double now);
[[nodiscard]] std::string_view postLoadError(const UiState& state);

} // namespace woby
