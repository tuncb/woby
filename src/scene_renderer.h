#pragma once

#include "model_mesh.h"
#include "scene_mesh_preparation.h"
#include "ui_state.h"
#include "scene_pick.h"
#include "scene_draw_plan.h"
#include "point_cloud.h"

#include "graphics.h"

#include <cstdint>
#include <vector>

namespace woby {

struct MarkerDrawContext;
struct AdaptivePointRuntime;

// CPU scratch owned by the viewport/export runtime, never by logical UiState.
// Rebuilt on each submission; stale borrowed pointers are never read across frames.
struct SceneRenderScratch {
    SceneDrawCache drawCache;
    std::vector<ScenePickPart> parts;
    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 3>> focusPoints;
    std::vector<const ScenePickPart*> annotationSources;
    std::vector<PickMatrix> annotationTransforms;
    std::vector<DiagnosticEdge> annotationLines;
};

struct GpuMesh {
    bool freeformPrepared = false;
    woby::graphics::VertexBufferHandle vertexBuffer = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::IndexBufferHandle triangleIndexBuffer = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::IndexBufferHandle lineIndexBuffer = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::IndexBufferHandle pointIdBuffer = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::IndexBufferHandle importedLineBuffer = WOBY_GPU_INVALID_HANDLE;
    std::vector<GpuNodeRange> nodeRanges;
    std::vector<uint32_t> pointVertexIndices;
    std::shared_ptr<const points::Cloud> pointCloud;
    std::vector<graphics::VertexBufferHandle> pointChunks, proxyChunks;
    bool compactOnly = false;
};
inline constexpr uint32_t pointChunkSize = 1048576;
[[nodiscard]] inline std::span<const uint32_t> meshPointVertexIndices(const GpuMesh& mesh) {
    return mesh.pointCloud ? std::span<const uint32_t>(mesh.pointCloud->sourceVertices) : mesh.pointVertexIndices;
}

struct LoadedModelRuntime {
    GpuMesh gpuMesh;
    // Retry failed optional uploads only when the requested modes change.
    uint8_t requestedFeatures = 0;
};

[[nodiscard]] woby::graphics::VertexLayout meshVertexLayout();
[[nodiscard]] woby::graphics::VertexLayout helperLineVertexLayout();

[[nodiscard]] GpuMesh createGpuMesh(
    const Mesh& mesh,
    const woby::graphics::VertexLayout& meshLayout,
    uint8_t features = 0);
// A staged mesh is never published to the scene until all buffers are uploaded.
struct GpuMeshUpload {
    GpuMesh mesh;
    std::vector<uint32_t> edgeIndices;
    uint8_t features = 0;
    uint8_t bufferIndex = 0;
    uint32_t bufferOffset = 0;
    uint32_t pointChunk = 0;
    size_t uploadedBytes = 0;
    size_t totalBytes = 0;
};
[[nodiscard]] GpuMeshUpload beginGpuMeshUpload(SceneMeshPreparation prepared);
// Source geometry must remain unchanged until completion. Each call copies at
// most byteBudget bytes into graphics-owned staging; no source pointer escapes.
[[nodiscard]] bool stepGpuMeshUpload(GpuMeshUpload& upload, const Mesh& source,
    const woby::graphics::VertexLayout& layout, uint32_t byteBudget);
void abortGpuMeshUpload(GpuMeshUpload& upload);

// Optional display buffers are retained once built; drawing never allocates.
void prepareGpuMeshFeatures(GpuMesh& gpuMesh, const Mesh& mesh, uint8_t features);
void destroyGpuMesh(GpuMesh& mesh);
void destroyModelRuntimes(std::vector<LoadedModelRuntime>& runtimes);

[[nodiscard]] uint32_t vertexPointSize(float masterSize, float groupScale);
// Two vec4 uniforms; split the ID offset to preserve all 32 bits in float storage.
[[nodiscard]] std::array<float, 8> pointSpriteParameters(
    float pointSize, uint32_t viewWidth, uint32_t viewHeight, uint32_t pointOffset);

struct TriangleEdgePrograms {
    woby::graphics::ProgramHandle surface = WOBY_GPU_INVALID_HANDLE, markerSurface = WOBY_GPU_INVALID_HANDLE,
        lines = WOBY_GPU_INVALID_HANDLE, markerLines = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::UniformHandle parameters = WOBY_GPU_INVALID_HANDLE;
    bool nativeBarycentrics = false;
};
[[nodiscard]] TriangleEdgePrograms createTriangleEdgePrograms(const std::filesystem::path& assets,
    bool forceVertexPulling = false);
void destroyTriangleEdgePrograms(TriangleEdgePrograms& programs);

void submitSceneFiles(
    woby::graphics::ViewId viewId,
    const SceneDrawPlan& plan,
    const std::vector<LoadedModelRuntime>& runtimes,
    woby::graphics::ProgramHandle meshProgram,
    woby::graphics::UniformHandle uvGridUniform,
    woby::graphics::ProgramHandle colorProgram,
    woby::graphics::ProgramHandle pointSpriteProgram,
    woby::graphics::UniformHandle colorUniform,
    woby::graphics::UniformHandle pointParamsUniform,
    const TriangleEdgePrograms& edgePrograms,
    uint32_t sceneViewportWidth,
    uint32_t viewportHeight,
    MarkerDrawContext* markers = nullptr,
    bool importedLinesOnly = false,
    AdaptivePointRuntime* adaptivePoints = nullptr); // Line pass follows surfaces/analyses; colorProgram is vs_line_sprite.

void submitSceneHelpers(
    woby::graphics::ViewId viewId,
    const UiState& state,
    const woby::graphics::VertexLayout& layout,
    woby::graphics::ProgramHandle program,
    woby::graphics::UniformHandle colorUniform);

void submitSceneSelection(woby::graphics::ViewId viewId, std::span<const ScenePickPart> parts, const UiState& state,
    const woby::graphics::VertexLayout& layout, woby::graphics::ProgramHandle program, woby::graphics::UniformHandle colorUniform,
    SceneRenderScratch& scratch);

} // namespace woby
