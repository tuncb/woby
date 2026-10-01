#pragma once

#include "model_mesh.h"
#include "ui_state.h"
#include "scene_pick.h"

#include "graphics.h"

#include <cstdint>
#include <vector>

namespace woby {

struct MarkerDrawContext;

// CPU scratch owned by the viewport/export runtime, never by logical UiState.
// Rebuilt on each submission; stale borrowed pointers are never read across frames.
struct SceneRenderScratch {
    std::vector<ScenePickPart> parts;
    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 3>> focusPoints;
    std::vector<const ScenePickPart*> annotationSources;
    std::vector<PickMatrix> annotationTransforms;
    std::vector<DiagnosticEdge> annotationLines;
};

struct GpuNodeRange {
    uint32_t triangleIndexOffset = 0;
    uint32_t triangleIndexCount = 0;
    uint32_t lineIndexOffset = 0;
    uint32_t lineIndexCount = 0;
    uint32_t pointIndexOffset = 0;
    uint32_t pointIndexCount = 0;
};

struct GpuMesh {
    woby::graphics::VertexBufferHandle vertexBuffer = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::IndexBufferHandle triangleIndexBuffer = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::IndexBufferHandle lineIndexBuffer = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::IndexBufferHandle pointIdBuffer = WOBY_GPU_INVALID_HANDLE;
    std::vector<GpuNodeRange> nodeRanges;
    std::vector<uint32_t> pointVertexIndices;
};

struct LoadedModelRuntime {
    GpuMesh gpuMesh;
    // Retry failed optional uploads only when the requested modes change.
    uint8_t requestedFeatures = 0;
};

enum GpuMeshFeature : uint8_t { gpuMeshEdges = 1, gpuMeshPoints = 2 };
[[nodiscard]] uint8_t requestedGpuMeshFeatures(const UiFileState& file);

[[nodiscard]] woby::graphics::VertexLayout meshVertexLayout();
[[nodiscard]] woby::graphics::VertexLayout helperLineVertexLayout();

[[nodiscard]] GpuMesh createGpuMesh(
    const Mesh& mesh,
    const woby::graphics::VertexLayout& meshLayout,
    uint8_t features = 0);
// Optional display buffers are retained once built; drawing never allocates.
void prepareGpuMeshFeatures(GpuMesh& gpuMesh, const Mesh& mesh, uint8_t features);
void destroyGpuMesh(GpuMesh& mesh);
void destroyModelRuntimes(std::vector<LoadedModelRuntime>& runtimes);

[[nodiscard]] uint32_t vertexPointSize(float masterSize, float groupScale);
// Two vec4 uniforms; split the ID offset to preserve all 32 bits in float storage.
[[nodiscard]] std::array<float, 8> pointSpriteParameters(
    float pointSize, uint32_t viewWidth, uint32_t viewHeight, uint32_t pointOffset);

void submitSceneFiles(
    woby::graphics::ViewId viewId,
    const std::vector<UiFileState>& files,
    const std::vector<UiSceneNode>& sceneNodes,
    const std::vector<LoadedModelRuntime>& runtimes,
    float masterVertexPointSize,
    woby::graphics::ProgramHandle meshProgram,
    woby::graphics::UniformHandle uvGridUniform,
    woby::graphics::ProgramHandle colorProgram,
    woby::graphics::ProgramHandle pointSpriteProgram,
    woby::graphics::UniformHandle colorUniform,
    woby::graphics::UniformHandle pointParamsUniform,
    uint32_t sceneViewportWidth,
    uint32_t viewportHeight,
    MarkerDrawContext* markers = nullptr);

void submitSceneHelpers(
    woby::graphics::ViewId viewId,
    const UiState& state,
    const woby::graphics::VertexLayout& layout,
    woby::graphics::ProgramHandle program,
    woby::graphics::UniformHandle colorUniform);

void submitSceneSelection(woby::graphics::ViewId viewId, std::span<const ScenePickPart> parts,
    const woby::graphics::VertexLayout& layout, woby::graphics::ProgramHandle program, woby::graphics::UniformHandle colorUniform,
    SceneRenderScratch& scratch);

} // namespace woby
