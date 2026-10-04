#include "scene_mesh_preparation.h"
#include "scene_buffer_size.h"

#include <stdexcept>

namespace woby {

uint8_t requestedGpuMeshFeatures(const UiFileState& file)
{
    uint8_t features = 0;
    if (!file.fileSettings.visible || file.fileSettings.opacity <= 0) { return features; }
    for (const auto& group : file.groupSettings) {
        if (!group.visible || group.opacity <= 0) { continue; }
        if (group.showTriangles) { features |= gpuMeshEdges; }
        if (group.showVertices) { features |= gpuMeshPoints; }
    }
    return features;
}

std::optional<SceneMeshPreparation> prepareSceneMesh(
    const Mesh& mesh, uint8_t features, const std::function<bool()>& shouldCancel)
{
    const auto canceled = [&] { return shouldCancel && shouldCancel(); };
    if (canceled()) { return {}; }
    (void)sceneBufferBytes(mesh.vertices.size(), sizeof(Vertex));
    (void)sceneBufferBytes(mesh.indices.size(), sizeof(uint32_t));
    (void)sceneBufferBytes(mesh.lineIndices.size(), sizeof(uint32_t));
    if (features & gpuMeshEdges) { (void)sceneBufferBytes(mesh.indices.size(), 2 * sizeof(uint32_t)); }
    if (empty(mesh) || mesh.indices.size() % 3 != 0 || mesh.lineIndices.size() % 2 != 0) {
        throw std::runtime_error("Scene needs valid triangles, line segments or points.");
    }
    for (const auto* indices : {&mesh.indices, &mesh.lineIndices, &mesh.pointIndices}) {
        for (size_t i = 0; i < indices->size(); ++i) {
            if (i % 65536 == 0 && canceled()) { return {}; }
            if ((*indices)[i] >= mesh.vertices.size()) {
                throw std::runtime_error(indices == &mesh.lineIndices ? "Scene contains an invalid line vertex index."
                    : indices == &mesh.pointIndices ? "Scene contains an invalid point vertex index."
                    : "Scene contains an invalid vertex index.");
            }
        }
    }
    SceneMeshPreparation result;
    result.features = features;
    result.nodeRanges.reserve(mesh.nodes.size());
    result.pointVertexIndices.reserve(mesh.vertices.size());
    std::vector<size_t> vertexGroups(mesh.vertices.size(), mesh.nodes.size());
    for (size_t nodeIndex = 0; nodeIndex < mesh.nodes.size(); ++nodeIndex) {
        if (canceled()) { return {}; }
        const auto& node = mesh.nodes[nodeIndex];
        if (size_t(node.indexOffset) + node.indexCount > mesh.indices.size()
            || node.indexOffset % 3 != 0 || node.indexCount % 3 != 0
            || size_t(node.lineIndexOffset) + node.lineIndexCount > mesh.lineIndices.size()
            || node.lineIndexOffset % 2 != 0 || node.lineIndexCount % 2 != 0
            || size_t(node.pointIndexOffset) + node.pointIndexCount > mesh.pointIndices.size()
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
        const auto indices = meshNodeIndices(mesh, node);
        for (size_t i = 0; i < indices.size(); ++i) {
            if (i % 65536 == 0 && canceled()) { return {}; }
            const auto vertex = indices[i];
            if (vertexGroups[vertex] != nodeIndex) {
                vertexGroups[vertex] = nodeIndex;
                result.pointVertexIndices.push_back(vertex);
            }
        }
        (void)sceneBufferBytes(result.pointVertexIndices.size(), sizeof(uint32_t));
        range.pointIndexCount = static_cast<uint32_t>(result.pointVertexIndices.size()) - range.pointIndexOffset;
        result.nodeRanges.push_back(range);
    }
    if (features & gpuMeshEdges) {
        result.edgeIndices.reserve(mesh.indices.size() * 2);
        for (size_t i = 0; i < mesh.indices.size(); i += 3) {
            if (i % 65536 == 0 && canceled()) { return {}; }
            const auto a = mesh.indices[i], b = mesh.indices[i + 1], c = mesh.indices[i + 2];
            result.edgeIndices.insert(result.edgeIndices.end(), {a, b, b, c, c, a});
        }
    }
    if (canceled()) { return {}; }
    result.uploadBytes = mesh.vertices.size() * sizeof(Vertex)
        + (mesh.indices.size() + mesh.lineIndices.size() + result.edgeIndices.size()
            + ((features & gpuMeshPoints) ? result.pointVertexIndices.size() : 0)) * sizeof(uint32_t);
    return result;
}

} // namespace woby
