#pragma once

#include "mesh_comparison.h"
#include "scene_renderer.h"

#include <future>
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
    bgfx::VertexBufferHandle winding = BGFX_INVALID_HANDLE;
};

struct ComparisonRuntime
{
    std::stop_source stop;
    std::future<MeshComparison> worker;
    uint64_t workerSignature = 0;
    uint64_t attemptedSignature = 0;
    uint64_t resultSignature = 0;
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

// Transient scene-panel editor state; the committed name belongs to UiState.
struct ComparisonNameEdit {
    SceneObjectId objectId = invalidSceneObjectId;
    std::string text;
    bool focus = false;
    int lastFrame = -1;
};

void updateComparisonRuntimes(ComparisonRuntimes& runtimes, const UiState& state);
void destroyComparisonRuntimes(ComparisonRuntimes& runtimes);
void drawComparisonObjects(UiState& state, ComparisonNameEdit& edit);
void drawComparisonPanelContents(UiState& state, ComparisonRuntimes& runtimes);
void submitComparisonScenes(bgfx::ViewId view, const UiState& state, const ComparisonRuntimes& runtimes,
    bgfx::ProgramHandle colorProgram, bgfx::UniformHandle colorUniform);
void appendVisibleComparisonPickParts(std::vector<ScenePickPart>& parts, const UiState& state,
    const ComparisonRuntimes& runtimes);
// False while any visible, valid comparison is queued/computing. Errors are reported to the caller.
[[nodiscard]] bool comparisonsReadyForScreenshot(const UiState& state, const ComparisonRuntimes& runtimes);

} // namespace woby
