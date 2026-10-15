#include "scene_mesh_upload.h"
#include "scene_buffer_size.h"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace woby {
namespace {
uint8_t missingFeatures(const GpuMesh& mesh, const Mesh& source, uint8_t features) {
    uint8_t missing = 0;
    if ((features & gpuMeshEdges) && !meshGeometry(source).indices.empty() && !graphics::isValid(mesh.lineIndexBuffer)) { missing |= gpuMeshEdges; }
    if ((features & gpuMeshPoints) && !mesh.pointCloud && !graphics::isValid(mesh.pointIdBuffer) && !mesh.pointVertexIndices.empty()) { missing |= gpuMeshPoints; }
    return missing;
}
std::shared_ptr<const MeshGeometry> snapshot(const Mesh& mesh) {
    return mesh.storage && !mesh.draft ? mesh.storage : std::make_shared<const MeshGeometry>(meshGeometry(mesh));
}
GpuMeshUpload begin(SceneMeshPreparation prepared, const Mesh& source, const graphics::VertexLayout& layout, bool pointsOnly, bool includeVertices) {
    if (layout.stride != sizeof(Vertex)) { throw std::invalid_argument("Mesh upload layout must match Vertex."); }
    GpuMeshUpload upload;
    upload.features = prepared.features;
    upload.mesh.nodeRanges = std::move(prepared.nodeRanges);
    upload.mesh.pointCloud = std::move(prepared.pointCloud);
    upload.mesh.compactOnly = prepared.compactOnly;
    upload.pointIds = std::make_shared<std::vector<uint32_t>>(std::move(prepared.pointVertexIndices));
    auto geometry = snapshot(source);
    std::vector<graphics::BufferUploadSource> sources;
    const auto add = [&](MeshUploadBuffer target, std::shared_ptr<const void> owner, const void* data, size_t count, uint16_t stride, bool index) {
        if (!count) { return; }
        const auto bytes = sceneBufferBytes(count, stride);
        sources.push_back({std::move(owner), data, bytes, stride, index, index});
        upload.targets.push_back(target);
        upload.totalBytes += bytes;
    };
    if (includeVertices && !upload.mesh.compactOnly) {
        add(MeshUploadBuffer::vertices, geometry, geometry->vertices.data(), geometry->vertices.size(), layout.stride, false);
        add(MeshUploadBuffer::vertexColors, geometry, geometry->vertexColors.data(), geometry->vertexColors.size(), sizeof(uint32_t), false);
    }
    if (!pointsOnly) {
        add(MeshUploadBuffer::triangles, geometry, geometry->indices.data(), geometry->indices.size(), sizeof(uint32_t), true);
        add(MeshUploadBuffer::importedLines, geometry, geometry->lineIndices.data(), geometry->lineIndices.size(), sizeof(uint32_t), true);
        auto edges = std::make_shared<const std::vector<uint32_t>>(std::move(prepared.edgeIndices));
        add(MeshUploadBuffer::edges, edges, edges->data(), edges->size(), sizeof(uint32_t), true);
    }
    if ((upload.features & gpuMeshPoints) && !upload.mesh.pointCloud) {
        add(MeshUploadBuffer::pointIds, upload.pointIds, upload.pointIds->data(), upload.pointIds->size(), sizeof(uint32_t), true);
    }
    if (const auto cloud = upload.mesh.pointCloud) {
        add(MeshUploadBuffer::pointColors, cloud, cloud->colors.data(), cloud->colors.size(), sizeof(uint32_t), false);
        const auto chunks = [&](const auto& values, MeshUploadBuffer target) {
            for (size_t offset = 0; offset < values.size(); offset += pointChunkSize) {
                add(target, cloud, values.data() + offset, std::min<size_t>(pointChunkSize, values.size() - offset), sizeof(points::Point), false);
            }
        };
        upload.mesh.pointChunks.reserve((cloud->points.size() + pointChunkSize - 1) / pointChunkSize);
        upload.mesh.proxyChunks.reserve((cloud->proxies.size() + pointChunkSize - 1) / pointChunkSize);
        chunks(cloud->points, MeshUploadBuffer::points); chunks(cloud->proxies, MeshUploadBuffer::proxies);
    }
    upload.ticket = graphics::beginBufferUpload(std::move(sources));
    return upload;
}
std::shared_ptr<graphics::BufferUpload> beginFeatures(const GpuMesh& mesh, const Mesh& source, uint8_t features) {
    const auto missing = missingFeatures(mesh, source, features);
    const bool edges = (missing & gpuMeshEdges) != 0;
    const bool ids = (missing & gpuMeshPoints) != 0;
    if (!edges && !ids) { return {}; }
    Mesh owned = source;
    publishMeshGeometry(owned);
    return graphics::beginBufferUpload([owned = std::move(owned), edges, ids](const std::atomic<bool>& canceled) {
        auto prepared = prepareSceneMesh(owned, static_cast<uint8_t>((edges ? gpuMeshEdges : 0) | (ids ? gpuMeshPoints : 0)),
            [&] { return canceled.load(std::memory_order_acquire); }, false);
        std::vector<graphics::BufferUploadSource> buffers;
        if (!prepared) { return buffers; }
        const auto add = [&](std::vector<uint32_t> values) {
            auto owner = std::make_shared<const std::vector<uint32_t>>(std::move(values));
            if (!owner->empty()) { buffers.push_back({owner, owner->data(), sceneBufferBytes(owner->size(), sizeof(uint32_t)), sizeof(uint32_t), true, true}); }
        };
        if (edges) { add(std::move(prepared->edgeIndices)); }
        if (ids) { add(std::move(prepared->pointVertexIndices)); }
        return buffers;
    });
}
void publishFeatures(GpuMesh& mesh, const std::shared_ptr<graphics::BufferUpload>& ticket, uint8_t features) {
    const auto buffers = graphics::adoptBufferUpload(ticket);
    size_t index = 0;
    if ((features & gpuMeshEdges) && !graphics::isValid(mesh.lineIndexBuffer)) { mesh.lineIndexBuffer = buffers.indices.at(index++); }
    if ((features & gpuMeshPoints) && !mesh.pointCloud && !graphics::isValid(mesh.pointIdBuffer) && !mesh.pointVertexIndices.empty()) {
        mesh.pointIdBuffer = buffers.indices.at(index++);
    }
    mesh.resourceRevision = nextMeshContentRevision();
}
}

GpuMeshUpload beginGpuMeshUpload(SceneMeshPreparation prepared, const Mesh& source, const graphics::VertexLayout& layout) {
    return begin(std::move(prepared), source, layout, false, true);
}
GpuMeshUpload beginGpuPointUpload(SceneMeshPreparation prepared, const Mesh& source, const graphics::VertexLayout& layout, bool includeVertices) {
    prepared.features = gpuMeshPoints;
    return begin(std::move(prepared), source, layout, true, includeVertices);
}
bool pollGpuMeshUpload(GpuMeshUpload& upload) {
    if (upload.complete) { return true; }
    upload.uploadedBytes = graphics::bufferUploadStats(upload.ticket).uploadedBytes;
    if (!graphics::bufferUploadComplete(upload.ticket)) { return false; }
    const auto buffers = graphics::adoptBufferUpload(upload.ticket);
    size_t vertex = 0, index = 0;
    for (const auto target : upload.targets) {
        switch (target) {
        case MeshUploadBuffer::vertices: upload.mesh.vertexBuffer = buffers.vertices[vertex++]; break;
        case MeshUploadBuffer::vertexColors: upload.mesh.vertexColors = buffers.vertices[vertex++]; break;
        case MeshUploadBuffer::pointColors: upload.mesh.pointColors = buffers.vertices[vertex++]; break;
        case MeshUploadBuffer::triangles: upload.mesh.triangleIndexBuffer = buffers.indices[index++]; break;
        case MeshUploadBuffer::importedLines: upload.mesh.importedLineBuffer = buffers.indices[index++]; break;
        case MeshUploadBuffer::edges: upload.mesh.lineIndexBuffer = buffers.indices[index++]; break;
        case MeshUploadBuffer::pointIds: upload.mesh.pointIdBuffer = buffers.indices[index++]; break;
        case MeshUploadBuffer::points: upload.mesh.pointChunks.push_back(buffers.vertices[vertex++]); break;
        case MeshUploadBuffer::proxies: upload.mesh.proxyChunks.push_back(buffers.vertices[vertex++]); break;
        }
    }
    upload.mesh.pointVertexIndices = std::move(*upload.pointIds);
    upload.pointIds.reset(); upload.complete = true;
    return true;
}
void waitGpuMeshUpload(GpuMeshUpload& upload) { graphics::waitBufferUpload(upload.ticket); (void)pollGpuMeshUpload(upload); }
void abortGpuMeshUpload(GpuMeshUpload& upload) { graphics::cancelBufferUpload(upload.ticket); destroyGpuMesh(upload.mesh); upload = {}; }
GpuMesh createGpuMesh(const Mesh& mesh, const graphics::VertexLayout& layout, uint8_t features) {
    auto prepared = prepareSceneMesh(mesh, features, {}, !mesh.preparationDeferred, !mesh.preparationDeferred);
    auto upload = beginGpuMeshUpload(std::move(*prepared), mesh, layout);
    try { waitGpuMeshUpload(upload); }
    catch (...) { abortGpuMeshUpload(upload); throw; }
    return std::move(upload.mesh);
}
void commitGpuPointUpload(GpuMesh& destination, GpuMeshUpload& upload) {
    if (!upload.complete) { throw std::invalid_argument("Point upload is incomplete."); }
    if (graphics::isValid(upload.mesh.vertexBuffer)) {
        graphics::destroy(destination.vertexBuffer);
        destination.vertexBuffer = std::exchange(upload.mesh.vertexBuffer, WOBY_GPU_INVALID_HANDLE);
        graphics::destroy(destination.vertexColors);
        destination.vertexColors = std::exchange(upload.mesh.vertexColors, WOBY_GPU_INVALID_HANDLE);
    }
    graphics::destroy(destination.pointIdBuffer);
    for (auto value : destination.pointChunks) { graphics::destroy(value); }
    for (auto value : destination.proxyChunks) { graphics::destroy(value); }
    destination.pointIdBuffer = std::exchange(upload.mesh.pointIdBuffer, WOBY_GPU_INVALID_HANDLE);
    graphics::destroy(destination.pointColors);
    destination.pointColors = std::exchange(upload.mesh.pointColors, WOBY_GPU_INVALID_HANDLE);
    destination.nodeRanges = std::move(upload.mesh.nodeRanges);
    destination.pointVertexIndices = std::move(upload.mesh.pointVertexIndices);
    destination.pointCloud = std::move(upload.mesh.pointCloud);
    destination.pointChunks = std::move(upload.mesh.pointChunks);
    destination.proxyChunks = std::move(upload.mesh.proxyChunks);
    destination.compactOnly = upload.mesh.compactOnly;
    destination.resourceRevision = nextMeshContentRevision();
    if (destination.compactOnly && graphics::isValid(destination.vertexBuffer)) { graphics::destroy(destination.vertexBuffer); destination.vertexBuffer = WOBY_GPU_INVALID_HANDLE; }
    if (destination.compactOnly && graphics::isValid(destination.vertexColors)) { graphics::destroy(destination.vertexColors); destination.vertexColors = WOBY_GPU_INVALID_HANDLE; }
    upload = {};
}
void prepareGpuMeshFeatures(GpuMesh& mesh, const Mesh& source, uint8_t features) {
    const auto missing = missingFeatures(mesh, source, features);
    if (const auto ticket = beginFeatures(mesh, source, missing)) { graphics::waitBufferUpload(ticket); publishFeatures(mesh, ticket, missing); }
}
void updateGpuMeshFeatures(LoadedModelRuntime& runtime, const Mesh& source, uint8_t features) {
    if (runtime.featureUpload && runtime.featureGeometryRevision != source.contentRevision) {
        graphics::cancelBufferUpload(runtime.featureUpload); runtime.featureUpload.reset(); runtime.requestedFeatures = 0;
    }
    if (runtime.featureUpload && graphics::bufferUploadComplete(runtime.featureUpload)) {
        auto ticket = std::exchange(runtime.featureUpload, {});
        publishFeatures(runtime.gpuMesh, ticket, runtime.uploadingFeatures);
    }
    if (!runtime.featureUpload && runtime.requestedFeatures != features) {
        runtime.requestedFeatures = features;
        runtime.featureGeometryRevision = source.contentRevision;
        runtime.uploadingFeatures = missingFeatures(runtime.gpuMesh, source, features);
        runtime.featureUpload = beginFeatures(runtime.gpuMesh, source, features);
    }
}
}
