#pragma once

#include "mesh_comparison.h"
#include "scene_renderer.h"

#include <future>
#include <string>

namespace woby
{

struct ComparisonGpuSurface
{
    bgfx::VertexBufferHandle vertices = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle triangles = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle lines = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle samples = BGFX_INVALID_HANDLE;
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
    bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle parameters = BGFX_INVALID_HANDLE;
    std::string error;
};

void updateComparisonRuntime(ComparisonRuntime &runtime, const UiState &state);
void destroyComparisonRuntime(ComparisonRuntime &runtime);
void drawComparisonPanel(UiState &state, ComparisonRuntime &runtime, float rightEdge, float width, float height);
[[nodiscard]] bool submitComparisonScene(bgfx::ViewId view, const UiState &state, const ComparisonRuntime &runtime,
                                         bgfx::ProgramHandle colorProgram, bgfx::UniformHandle colorUniform);

} // namespace woby
