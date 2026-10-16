#include "scene_mesh_preparation.h"
#include "scene_buffer_size.h"
#include "point_cloud.h"

#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace woby {

uint8_t requestedGpuMeshFeatures(const UiFileState& file)
{
    uint8_t features = 0;
    if (!postLoadReady(file)) { return features; }
    if (!file.fileSettings.visible) { return features; }
    for (const auto& group : file.groupSettings) {
        if (!group.visible) { continue; }
        // Triangle overlays read the existing triangle buffer, including X-ray.
        if (group.showVertices) { features |= gpuMeshPoints; }
    }
    return features;
}

std::optional<SceneMeshPreparation> prepareSceneMesh(
    const Mesh& mesh, uint8_t features, const std::function<bool()>& shouldCancel, bool buildPointHierarchy, bool preparePoints)
{
    const auto canceled = [&] { return shouldCancel && shouldCancel(); };
    if (canceled()) { return {}; }
    (void)sceneBufferBytes(meshGeometry(mesh).vertices.size(), sizeof(Vertex));
    if (!meshGeometry(mesh).vertexColors.empty() && meshGeometry(mesh).vertexColors.size() != meshGeometry(mesh).vertices.size()) {
        throw std::runtime_error("Scene contains an invalid vertex color count.");
    }
    (void)sceneBufferBytes(meshGeometry(mesh).indices.size(), sizeof(uint32_t));
    (void)sceneBufferBytes(meshGeometry(mesh).lineIndices.size(), sizeof(uint32_t));
    if (features & gpuMeshEdges) { (void)sceneBufferBytes(meshGeometry(mesh).indices.size(), 2 * sizeof(uint32_t)); }
    if (empty(mesh) || meshGeometry(mesh).indices.size() % 3 != 0 || meshGeometry(mesh).lineIndices.size() % 2 != 0) {
        throw std::runtime_error("Scene needs valid triangles, line segments or points.");
    }
    const auto& cache = mesh.preparationCache;
    const bool cacheCurrent = !mesh.draft && cache && cache->geometry == mesh.storage.get();
    if (!cacheCurrent || !cache->indicesValidated.load(std::memory_order_acquire)) {
        const auto& geometry = meshGeometry(mesh);
        const auto vertexCount = geometry.vertices.size();
        for (const auto* indices : {&geometry.indices, &geometry.lineIndices, &geometry.pointIndices}) {
            const auto* error = indices == &geometry.lineIndices ? "Scene contains an invalid line vertex index."
                : indices == &geometry.pointIndices ? "Scene contains an invalid point vertex index."
                : "Scene contains an invalid vertex index.";
            for (size_t begin = 0; begin < indices->size();) {
                if (canceled()) { return {}; }
                const auto end = begin + std::min(size_t{65536}, indices->size() - begin);
                for (size_t i = begin; i < end; ++i) {
                    if ((*indices)[i] >= vertexCount) { throw std::runtime_error(error); }
                }
                begin = end;
            }
        }
        if (canceled()) { return {}; }
        if (cacheCurrent) { cache->indicesValidated.store(true, std::memory_order_release); }
    }
    SceneMeshPreparation result;
    result.features = features;
    result.nodeRanges.reserve(meshGeometry(mesh).nodes.size());
    if (preparePoints) { result.pointVertexIndices.reserve(meshGeometry(mesh).vertices.size()); }
    const auto vertexCount = meshGeometry(mesh).vertices.size();
    const bool singleGroup = preparePoints && meshGeometry(mesh).nodes.size() == 1;
    // A single group needs only a visited bit. Keep the scan in source order:
    // its first occurrences define persistent point IDs, including UV seams.
    std::vector<uint64_t> visited;
    std::vector<size_t> vertexGroups(preparePoints && !singleGroup ? vertexCount : 0, meshGeometry(mesh).nodes.size());
    for (size_t nodeIndex = 0; nodeIndex < meshGeometry(mesh).nodes.size(); ++nodeIndex) {
        if (canceled()) { return {}; }
        const auto& node = meshGeometry(mesh).nodes[nodeIndex];
        if (size_t(node.indexOffset) + node.indexCount > meshGeometry(mesh).indices.size()
            || node.indexOffset % 3 != 0 || node.indexCount % 3 != 0
            || size_t(node.lineIndexOffset) + node.lineIndexCount > meshGeometry(mesh).lineIndices.size()
            || node.lineIndexOffset % 2 != 0 || node.lineIndexCount % 2 != 0
            || size_t(node.pointIndexOffset) + node.pointIndexCount > meshGeometry(mesh).pointIndices.size()
            || (node.indexCount && node.lineIndexCount)
            || (node.pointIndexCount && (node.indexCount || node.lineIndexCount))) {
            throw std::runtime_error("Scene contains an invalid primitive range.");
        }
        GpuNodeRange range;
        range.triangleIndexOffset = node.indexOffset;
        range.triangleIndexCount = node.indexCount;
        range.lineIndexOffset = node.indexOffset * 2u;
        range.lineIndexCount = node.indexCount * 2u;
        range.pointIndexOffset = static_cast<uint32_t>(result.pointVertexIndices.size());
        const auto indices = preparePoints ? meshNodeIndices(mesh, node) : std::span<const uint32_t>{};
        if (singleGroup) {
            // Importers often assign render vertices in first-use order. Prove
            // that prefix while scanning; it needs no marker reads or writes.
            uint32_t nextVertex = 0;
            size_t i = 0;
            for (; i < indices.size(); ++i) {
                if (i % 65536 == 0 && canceled()) { return {}; }
                const auto vertex = indices[i];
                if (vertex > nextVertex) { break; }
                nextVertex += static_cast<uint32_t>(vertex == nextVertex);
                if (nextVertex == vertexCount) { break; }
            }
            result.pointVertexIndices.resize(nextVertex);
            std::iota(result.pointVertexIndices.begin(), result.pointVertexIndices.end(), 0u);
            if (i < indices.size() && nextVertex < vertexCount) {
                visited.resize((vertexCount + 63) / 64);
                std::fill_n(visited.begin(), nextVertex / 64, ~uint64_t{0});
                if (nextVertex % 64) { visited[nextVertex / 64] = (uint64_t{1} << (nextVertex % 64)) - 1; }
            } else { i = indices.size(); }
            for (; i < indices.size(); ++i) {
                if (i % 65536 == 0 && canceled()) { return {}; }
                const auto vertex = indices[i];
                auto& word = visited[vertex / 64];
                const auto bit = uint64_t{1} << (vertex % 64);
                if (!(word & bit)) {
                    word |= bit;
                    result.pointVertexIndices.push_back(vertex);
                    // All indices were validated above, including this tail.
                    if (result.pointVertexIndices.size() == vertexCount) { break; }
                }
            }
        } else {
            for (size_t i = 0; i < indices.size(); ++i) {
                if (i % 65536 == 0 && canceled()) { return {}; }
                const auto vertex = indices[i];
                if (vertexGroups[vertex] != nodeIndex) {
                    vertexGroups[vertex] = nodeIndex;
                    result.pointVertexIndices.push_back(vertex);
                }
            }
        }
        (void)sceneBufferBytes(result.pointVertexIndices.size(), sizeof(uint32_t));
        range.pointIndexCount = static_cast<uint32_t>(result.pointVertexIndices.size()) - range.pointIndexOffset;
        range.completeVertexSet = preparePoints && range.pointIndexCount == meshGeometry(mesh).vertices.size();
        result.nodeRanges.push_back(range);
    }
    // Release membership scratch before allocating the point hierarchy.
    std::vector<uint64_t>().swap(visited);
    std::vector<size_t>().swap(vertexGroups);
    if (features & gpuMeshEdges) {
        result.edgeIndices.reserve(meshGeometry(mesh).indices.size() * 2);
        for (size_t i = 0; i < meshGeometry(mesh).indices.size(); i += 3) {
            if (i % 65536 == 0 && canceled()) { return {}; }
            const auto a = meshGeometry(mesh).indices[i], b = meshGeometry(mesh).indices[i + 1], c = meshGeometry(mesh).indices[i + 2];
            result.edgeIndices.insert(result.edgeIndices.end(), {a, b, b, c, c, a});
        }
    }
    if (canceled()) { return {}; }
    result.uploadBytes = meshGeometry(mesh).vertices.size() * sizeof(Vertex) + meshGeometry(mesh).vertexColors.size() * sizeof(uint32_t)
        + (meshGeometry(mesh).indices.size() + meshGeometry(mesh).lineIndices.size() + result.edgeIndices.size()
            + ((features & gpuMeshPoints) ? result.pointVertexIndices.size() : 0)) * sizeof(uint32_t);
    if (!preparePoints && meshGeometry(mesh).indices.empty() && meshGeometry(mesh).lineIndices.empty()) {
        result.compactOnly = true;
        result.uploadBytes = 0; // The first frame is a placeholder; every point resource is deferred.
    }
    if (buildPointHierarchy && result.pointVertexIndices.size() >= 65536 && !mesh.freeform) {
        auto cloud = points::buildCloud(mesh, std::move(result.pointVertexIndices), result.nodeRanges, shouldCancel);
        if (!cloud) { return {}; }
        result.compactOnly = meshGeometry(mesh).indices.empty() && meshGeometry(mesh).lineIndices.empty();
        if (result.compactOnly) {
            result.uploadBytes -= meshGeometry(mesh).vertices.size() * sizeof(Vertex) + meshGeometry(mesh).vertexColors.size() * sizeof(uint32_t);
        }
        if (features & gpuMeshPoints) { result.uploadBytes -= cloud->sourceVertices.size() * sizeof(uint32_t); }
        result.uploadBytes += (cloud->points.size() + cloud->proxies.size()) * sizeof(points::Point);
        result.uploadBytes += cloud->colors.size() * sizeof(uint32_t);
        result.pointCloud = std::make_shared<const points::Cloud>(std::move(*cloud));
    }
    return result;
}

} // namespace woby
