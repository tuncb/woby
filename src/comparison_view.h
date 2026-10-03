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

struct ComparisonRuntime
{
    uint64_t sidebarRevision = uint64_t(-1);
    std::array<ComparisonInputSummary, 2> sidebarInputs;
    std::string sidebarSources;
    std::array<std::array<DiagnosticSummary, 2>, diagnosticCategoryCount> diagnosticSummaries{};
    std::stop_source preparationStop;
    std::future<std::shared_ptr<const PreparedComparisonInputs>> preparationWorker;
    uint64_t preparationSignature = 0;
    std::shared_ptr<const PreparedComparisonInputs> prepared;
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
    woby::graphics::ProgramHandle program = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::UniformHandle parameters = WOBY_GPU_INVALID_HANDLE;
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
void drawAnalysisCreationMenu(UiState& state);
void drawAnalysisCreationMenu(UiState& state, const std::vector<SceneObjectId>& sources);
void drawComparisonObjects(UiState& state, ComparisonNameEdit& edit, const ComparisonRuntimes& runtimes = {});
void drawComparisonPanelContents(UiState& state, ComparisonRuntimes& runtimes);
void submitComparisonScenes(woby::graphics::ViewId view, const UiState& state, const ComparisonRuntimes& runtimes,
    woby::graphics::ProgramHandle colorProgram, woby::graphics::UniformHandle colorUniform, SceneRenderScratch& scratch,
    woby::graphics::ProgramHandle markerProgram = WOBY_GPU_INVALID_HANDLE);
void appendVisibleComparisonPickParts(std::vector<ScenePickPart>& parts, const UiState& state,
    const ComparisonRuntimes& runtimes);
// False while any visible, valid comparison is queued/computing. Errors are reported to the caller.
[[nodiscard]] bool comparisonsReadyForScreenshot(const UiState& state, const ComparisonRuntimes& runtimes);

} // namespace woby
