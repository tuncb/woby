#pragma once

#include "mesh_comparison.h"
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
    bgfx::VertexBufferHandle vertices = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle triangles = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle lines = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle samples = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle quality = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle boundaries = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle nonManifold = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle nonManifoldVertices = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle holes = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle finEdges = BGFX_INVALID_HANDLE, finFill = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle winding = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle intersectionEdges = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle intersectionFill = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle degenerateEdges = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle degenerateFill = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle duplicatePoints = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle duplicateTriangleEdges = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle duplicateTriangleFill = BGFX_INVALID_HANDLE;
};

struct IntersectionRuntime {
    IntersectionLimits limits;
    std::stop_source stop;
    std::future<MeshComparison> worker;
    uint64_t workerSignature = 0, revision = 0, workerRevision = 0, consumedRequest = 0;
    bool requested = false, canceled = false, autoUpdate = false;
    std::chrono::steady_clock::time_point started{};
};

struct ComparisonRuntime
{
    std::stop_source stop;
    std::future<MeshComparison> worker;
    uint64_t workerSignature = 0;
    uint32_t workerStages = 0, attemptedStages = 0, uploadedStages = 0;
    uint32_t failedStages = 0;
    bool retryDetectorsSeparately = false;
    std::array<uint64_t, backgroundDetectorCount> consumedDetectorRequests{};
    std::array<uint64_t, backgroundDetectorCount> workerDetectorRequests{};
    uint32_t workerDetectors = 0;
    float holeSizeRatioTolerance = .05f;
    float finMaxAreaRatio = 1.0f;
    IntersectionRuntime intersection;
    ComparisonCacheStatus cache;
    std::shared_ptr<const std::array<Mesh, 2>> inputs;
    bool fullResultsRequested = false;
    uint64_t attemptedSignature = 0;
    uint64_t resultSignature = 0, resultsRevision = 0;
    bool ready = false;
    MeshComparison result;
    ComparisonGpuSurface originalGpu, repairedGpu;
    std::string error;
    SurfaceQualityMetric uploadedQualityMetric = SurfaceQualityMetric::longestEdge;
};

struct ComparisonRuntimes {
    std::map<SceneObjectId, ComparisonRuntime> objects;
    bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle parameters = BGFX_INVALID_HANDLE;
};

[[nodiscard]] bool comparisonDetectorReady(const ComparisonRuntime& runtime, const UiState& state,
    SceneObjectId id, DiagnosticCategory category, bool requireGpu = false);

// Transient scene-panel editor state; the committed name belongs to UiState.
struct ComparisonNameEdit {
    SceneObjectId objectId = invalidSceneObjectId;
    std::string text;
    bool focus = false;
    int lastFrame = -1;
};

[[nodiscard]] bool comparisonResultsReady(const ComparisonRuntime& runtime, const UiState& state,
    SceneObjectId id, bool fullResults = false);
[[nodiscard]] bool comparisonStagesReady(const ComparisonRuntime& runtime, const UiState& state,
    SceneObjectId id, uint32_t stages, bool requireGpu = false);
[[nodiscard]] ComparisonSettings readyComparisonSettings(const ComparisonRuntime& runtime,
    const UiState& state, SceneObjectId id);
void updateComparisonRuntimes(ComparisonRuntimes& runtimes, UiState& state);
void destroyComparisonRuntimes(ComparisonRuntimes& runtimes);
void drawComparisonObjects(UiState& state, ComparisonNameEdit& edit);
void drawComparisonPanelContents(UiState& state, ComparisonRuntimes& runtimes);
void submitComparisonScenes(bgfx::ViewId view, const UiState& state, const ComparisonRuntimes& runtimes,
    bgfx::ProgramHandle colorProgram, bgfx::UniformHandle colorUniform, SceneRenderScratch& scratch);
void appendVisibleComparisonPickParts(std::vector<ScenePickPart>& parts, const UiState& state,
    const ComparisonRuntimes& runtimes);
// False while any visible, valid comparison is queued/computing. Errors are reported to the caller.
[[nodiscard]] bool comparisonsReadyForScreenshot(const UiState& state, const ComparisonRuntimes& runtimes);

} // namespace woby
