#pragma once

#include "mesh_comparison.h"
#include "analysis_presentation.h"
#include "comparison_scene.h"
#include "scene_renderer.h"

#include <future>
#include <chrono>
#include <map>
#include <string>

namespace woby
{

inline constexpr const char* comparisonSourcePayload = "WOBY_COMPARISON_SOURCES";

struct ComparisonGpuSurface
{
    woby::graphics::VertexBufferHandle vertices = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::IndexBufferHandle triangles = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::IndexBufferHandle lines = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::VertexBufferHandle samples = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::VertexBufferHandle quality = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::VertexBufferHandle boundaries = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::VertexBufferHandle nonManifold = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::VertexBufferHandle nonManifoldVertices = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::VertexBufferHandle holes = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::VertexBufferHandle finEdges = WOBY_GPU_INVALID_HANDLE, finFill = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::VertexBufferHandle winding = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::VertexBufferHandle intersectionEdges = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::VertexBufferHandle intersectionFill = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::VertexBufferHandle degenerateEdges = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::VertexBufferHandle degenerateFill = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::VertexBufferHandle duplicatePoints = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::VertexBufferHandle duplicateTriangleEdges = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::VertexBufferHandle duplicateTriangleFill = WOBY_GPU_INVALID_HANDLE;
};

struct IntersectionRuntime {
    IntersectionLimits limits;
    std::stop_source stop;
    std::future<MeshComparison> worker;
    uint64_t workerSignature = 0, revision = 0, workerRevision = 0, consumedRequest = 0;
    bool requested = false, canceled = false, autoUpdate = false;
    std::chrono::steady_clock::time_point started{};
};

struct ComparisonSourceRuntime {
    std::shared_ptr<const PreparedComparisonInputs> prepared;
    std::shared_ptr<const std::array<Mesh, 2>> inputs;
    uint64_t preparationSignature = 0;
    bool refreshUvQuality = false;
};
struct ComparisonJobRuntime {
    std::stop_source preparationStop;
    std::future<std::shared_ptr<const PreparedComparisonInputs>> preparationWorker;
    uint64_t preparationSignature = 0, preparationGeometrySignature = 0;
    std::stop_source stop;
    std::future<MeshComparison> worker;
    uint64_t workerSignature = 0;
    uint32_t workerStages = 0, attemptedStages = 0, failedStages = 0;
    bool retryDetectorsSeparately = false;
    bool allocationFailed = false;
    std::array<uint64_t, backgroundDetectorCount> consumedDetectorRequests{};
    std::array<uint64_t, backgroundDetectorCount> workerDetectorRequests{};
    uint32_t workerDetectors = 0;
    float holeSizeRatioTolerance = .05f, finMaxAreaRatio = 1.0f;
    IntersectionRuntime intersection;
    bool fullResultsRequested = false;
    uint64_t attemptedSignature = 0;
    std::string error;
};
struct ComparisonResultRuntime {
    ComparisonCacheStatus cache;
    uint64_t signature = 0, revision = 0;
    std::array<uint64_t, 8> stageRevisions{};
    MeshComparison value;
};
struct ComparisonGpuRuntime {
    ComparisonGpuSurface original, repaired;
    uint32_t uploadedStages = 0;
    std::array<uint64_t, 8> stageRevisions{};
    bool ready = false;
};
struct ComparisonInspectorRuntime {
    ComparisonInspectorCache queries;
    std::array<std::array<DiagnosticSummary, 2>, diagnosticCategoryCount> diagnosticSummaries{};
};
struct ComparisonRuntime {
    ComparisonSourceRuntime sources;
    ComparisonJobRuntime jobs;
    ComparisonResultRuntime results;
    ComparisonGpuRuntime gpu;
    ComparisonInspectorRuntime inspector;
};

struct ComparisonRuntimes {
    const UiState* owner = nullptr;
    uint64_t generation = 0;
    std::map<SceneObjectId, ComparisonRuntime> objects;
    woby::graphics::ProgramHandle program = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::UniformHandle parameters = WOBY_GPU_INVALID_HANDLE;
};

[[nodiscard]] uint64_t comparisonCurrentSignature(const ComparisonRuntime& runtime, const UiState& state, SceneObjectId id);
[[nodiscard]] bool comparisonResultsReady(const ComparisonRuntime& runtime, const UiState& state, SceneObjectId id,
    uint64_t signature, bool both, bool fullResults);
[[nodiscard]] bool comparisonDetectorReady(const ComparisonRuntime& runtime, const ComparisonSettings& settings,
    uint64_t signature, DiagnosticCategory category, bool requireGpu = false);
[[nodiscard]] bool comparisonDetectorReady(const ComparisonRuntime& runtime, const UiState& state,
    SceneObjectId id, DiagnosticCategory category, bool requireGpu = false);

[[nodiscard]] bool comparisonResultsReady(const ComparisonRuntime& runtime, const UiState& state,
    SceneObjectId id, bool fullResults = false);
[[nodiscard]] bool comparisonStagesReady(const ComparisonRuntime& runtime, const UiState& state,
    SceneObjectId id, uint32_t stages, bool requireGpu = false);
// Reuse resolved scene inputs, but always evaluate live result/settings/GPU state.
[[nodiscard]] bool comparisonStagesReady(const ComparisonRuntime& runtime, const ComparisonSettings& settings,
    uint64_t signature, uint32_t stages, bool requireGpu = false);
[[nodiscard]] ComparisonSettings readyComparisonSettings(const ComparisonRuntime& runtime,
    const UiState& state, SceneObjectId id);
void updateComparisonRuntimes(ComparisonRuntimes& runtimes, UiState& state);
void destroyComparisonRuntimes(ComparisonRuntimes& runtimes);
// False while any visible, valid comparison is queued/computing. Errors are reported to the caller.
[[nodiscard]] bool comparisonsReadyForScreenshot(const UiState& state, const ComparisonRuntimes& runtimes);

} // namespace woby
