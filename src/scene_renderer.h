#pragma once

#include "model_mesh.h"
#include "ui_state.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <vector>

namespace woby {

struct GpuNodeRange {
    uint32_t triangleIndexOffset = 0;
    uint32_t triangleIndexCount = 0;
    uint32_t lineIndexOffset = 0;
    uint32_t lineIndexCount = 0;
    uint32_t pointIndexOffset = 0;
    uint32_t pointIndexCount = 0;
    uint32_t pointSpriteIndexOffset = 0;
    uint32_t pointSpriteIndexCount = 0;
};

struct GpuMesh {
    bgfx::VertexBufferHandle vertexBuffer = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle pointSpriteVertexBuffer = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle triangleIndexBuffer = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle lineIndexBuffer = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle pointSpriteIndexBuffer = BGFX_INVALID_HANDLE;
    std::vector<GpuNodeRange> nodeRanges;
    std::vector<uint32_t> pointVertexIndices;
};

struct LoadedModelRuntime {
    GpuMesh gpuMesh;
};

[[nodiscard]] bgfx::VertexLayout meshVertexLayout();
[[nodiscard]] bgfx::VertexLayout pointSpriteVertexLayout();
[[nodiscard]] bgfx::VertexLayout helperLineVertexLayout();

[[nodiscard]] GpuMesh createGpuMesh(
    const Mesh& mesh,
    const bgfx::VertexLayout& meshLayout,
    const bgfx::VertexLayout& pointSpriteLayout);
void destroyGpuMesh(GpuMesh& mesh);
void destroyModelRuntimes(std::vector<LoadedModelRuntime>& runtimes);

[[nodiscard]] uint32_t vertexPointSize(float masterSize, float groupScale);

void submitSceneFiles(
    bgfx::ViewId viewId,
    const std::vector<UiFileState>& files,
    const std::vector<UiSceneNode>& sceneNodes,
    const std::vector<LoadedModelRuntime>& runtimes,
    float masterVertexPointSize,
    bgfx::ProgramHandle meshProgram,
    bgfx::ProgramHandle colorProgram,
    bgfx::ProgramHandle pointSpriteProgram,
    bgfx::UniformHandle colorUniform,
    bgfx::UniformHandle pointParamsUniform,
    uint32_t sceneViewportWidth,
    uint32_t viewportHeight);

void submitSceneHelpers(
    bgfx::ViewId viewId,
    const UiState& state,
    const bgfx::VertexLayout& layout,
    bgfx::ProgramHandle program,
    bgfx::UniformHandle colorUniform);

} // namespace woby
