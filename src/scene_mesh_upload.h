#pragma once
#include "model_mesh.h"
#include "scene_mesh_preparation.h"
#include "graphics.h"
#include "point_cloud.h"

namespace woby {
struct GpuMesh {
    uint64_t resourceRevision = nextMeshContentRevision();
    graphics::VertexBufferHandle vertexBuffer = WOBY_GPU_INVALID_HANDLE;
    graphics::VertexBufferHandle vertexColors = WOBY_GPU_INVALID_HANDLE;
    graphics::VertexBufferHandle pointColors = WOBY_GPU_INVALID_HANDLE;
    graphics::IndexBufferHandle triangleIndexBuffer = WOBY_GPU_INVALID_HANDLE;
    graphics::IndexBufferHandle lineIndexBuffer = WOBY_GPU_INVALID_HANDLE;
    graphics::IndexBufferHandle pointIdBuffer = WOBY_GPU_INVALID_HANDLE;
    graphics::IndexBufferHandle importedLineBuffer = WOBY_GPU_INVALID_HANDLE;
    std::vector<GpuNodeRange> nodeRanges;
    std::vector<uint32_t> pointVertexIndices;
    std::shared_ptr<const points::Cloud> pointCloud;
    std::vector<graphics::VertexBufferHandle> pointChunks, proxyChunks;
    bool compactOnly = false;
};
inline constexpr uint32_t pointChunkSize = 1048576;
inline std::span<const uint32_t> meshPointVertexIndices(const GpuMesh& mesh) {
    return mesh.pointCloud ? std::span<const uint32_t>(mesh.pointCloud->sourceVertices) : mesh.pointVertexIndices;
}
enum class MeshUploadBuffer { vertices, vertexColors, pointColors, triangles, importedLines, edges, pointIds, points, proxies };
struct GpuMeshUpload {
    GpuMesh mesh;
    std::shared_ptr<graphics::BufferUpload> ticket;
    std::shared_ptr<std::vector<uint32_t>> pointIds;
    std::vector<MeshUploadBuffer> targets;
    uint8_t features = 0;
    uint64_t totalBytes = 0, uploadedBytes = 0;
    bool complete = false;
};
struct LoadedModelRuntime {
    GpuMesh gpuMesh;
    uint8_t requestedFeatures = 0;
    std::shared_ptr<graphics::BufferUpload> featureUpload;
    uint8_t uploadingFeatures = 0;
    uint64_t featureGeometryRevision = 0;
};
graphics::VertexLayout meshVertexLayout();
graphics::VertexLayout helperLineVertexLayout();
GpuMeshUpload beginGpuMeshUpload(SceneMeshPreparation prepared, const Mesh& source, const graphics::VertexLayout& layout);
GpuMeshUpload beginGpuPointUpload(SceneMeshPreparation prepared, const Mesh& source, const graphics::VertexLayout& layout, bool includeVertices);
bool pollGpuMeshUpload(GpuMeshUpload& upload);
void waitGpuMeshUpload(GpuMeshUpload& upload); // Synchronous test adapters.
void abortGpuMeshUpload(GpuMeshUpload& upload);
void commitGpuPointUpload(GpuMesh& destination, GpuMeshUpload& upload);
GpuMesh createGpuMesh(const Mesh& mesh, const graphics::VertexLayout& layout, uint8_t features = 0);
void prepareGpuMeshFeatures(GpuMesh& mesh, const Mesh& source, uint8_t features);
void updateGpuMeshFeatures(LoadedModelRuntime& runtime, const Mesh& source, uint8_t features);
void destroyGpuMesh(GpuMesh& mesh);
void destroyModelRuntimes(std::vector<LoadedModelRuntime>& runtimes);
}
