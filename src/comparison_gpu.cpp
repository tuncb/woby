#include "comparison_gpu.h"
#include "uv_quality_view.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace woby {
void destroySurface(ComparisonGpuSurface &gpu)
{
    for (const auto handle : {gpu.vertices, gpu.samples, gpu.quality, gpu.boundaries, gpu.nonManifold, gpu.winding,
        gpu.nonManifoldVertices, gpu.holes, gpu.finEdges, gpu.finFill, gpu.duplicatePoints, gpu.duplicateTriangleEdges, gpu.duplicateTriangleFill, gpu.degenerateEdges, gpu.degenerateFill, gpu.intersectionEdges, gpu.intersectionFill})
    {
        if (woby::graphics::isValid(handle))
        {
            woby::graphics::destroy(handle);
        }
    }
    for (const auto handle : {gpu.triangles})
    {
        if (woby::graphics::isValid(handle))
        {
            woby::graphics::destroy(handle);
        }
    }
    gpu = {};
}
woby::graphics::VertexBufferHandle uploadEdges(const std::vector<DiagnosticEdge> &edges)
{
    if (edges.empty())
    {
        return WOBY_GPU_INVALID_HANDLE;
    }
    // DiagnosticEdge already has the packed endpoint layout consumed by the
    // renderer. Copy it directly instead of allocating another full line list.
    static_assert(sizeof(DiagnosticEdge) == 2 * sizeof(std::array<float, 3>));
    static_assert(offsetof(DiagnosticEdge, a) == 0);
    static_assert(offsetof(DiagnosticEdge, b) == sizeof(std::array<float, 3>));
    const auto bytes = comparisonBufferBytes(edges.size(), sizeof(DiagnosticEdge));
    const auto handle = woby::graphics::createVertexBuffer(
        woby::graphics::copy(edges.data(), bytes), helperLineVertexLayout());
    if (!woby::graphics::isValid(handle))
    {
        throw std::runtime_error("Cannot allocate analysis edge buffer.");
    }
    return handle;
}
woby::graphics::VertexBufferHandle uploadPositions(const std::vector<std::array<float, 3>>& positions)
{
    if (positions.empty()) { return WOBY_GPU_INVALID_HANDLE; }
    const auto handle = woby::graphics::createVertexBuffer(woby::graphics::copy(positions.data(), comparisonBufferBytes(positions.size(), sizeof(positions[0]))), helperLineVertexLayout());
    if (!woby::graphics::isValid(handle)) { throw std::runtime_error("Cannot allocate duplicate overlay buffer."); }
    return handle;
}
void appendCross(std::vector<std::array<float, 3>>& lines, const std::array<float, 3>& point, float radius)
{
    for (size_t axis = 0; axis < 3; ++axis) {
        auto a = point, b = point; a[axis] -= radius; b[axis] += radius;
        lines.push_back(a); lines.push_back(b);
    }
}
void uploadSurface(ComparisonGpuSurface& gpu, const SurfaceComparison& surface, uint32_t stages,
    const PreparedComparisonSource* prepared)
{
    if (meshGeometry(surface.source).indices.empty()) { return; }
    if (stages & comparisonQuality) {
        if (surface.qualityFaces.size() != meshGeometry(surface.source).indices.size()/3) {
            throw std::runtime_error("Surface quality display does not match its source triangles.");
        }
        const auto bytes = comparisonBufferBytes(surface.qualityFaces.size(), sizeof(SurfaceQualityFace));
        // createVertexBuffer consumes the reference synchronously; avoid an
        // additional full-size CPU copy before the native upload staging copy.
        gpu.quality = woby::graphics::createVertexBuffer(
            woby::graphics::makeRef(surface.qualityFaces.data(), bytes), {sizeof(SurfaceQualityFace)});
        if (!woby::graphics::isValid(gpu.quality)) { throw std::runtime_error("Cannot allocate surface mesh quality buffer."); }
    }
    if (stages & comparisonSource) {
        const auto vertexBytes = comparisonBufferBytes(meshGeometry(surface.source).vertices.size(), sizeof(Vertex));
        const auto indexBytes = comparisonBufferBytes(meshGeometry(surface.source).indices.size(), sizeof(uint32_t));
        if (surface.source.uvQuality && (!prepared || prepared->updateQuality)) {
            const auto qualityBytes = comparisonBufferBytes(meshGeometry(surface.source).indices.size()/3,sizeof(UvQualityFace));
            const auto values = prepared ? std::vector<UvQualityFace>{} : uvQualityFaces(surface.source, *surface.source.uvQuality);
            const auto& faces = prepared ? prepared->quality : values;
            if (faces.size() != meshGeometry(surface.source).indices.size()/3) {
                throw std::runtime_error("UV quality display does not match its source triangles.");
            }
            const auto handle = woby::graphics::createVertexBuffer(woby::graphics::copy(faces.data(), qualityBytes),{sizeof(UvQualityFace)});
            if (!woby::graphics::isValid(handle)) { throw std::runtime_error("Cannot allocate UV quality buffer."); }
            if (woby::graphics::isValid(gpu.quality)) { woby::graphics::destroy(gpu.quality); }
            gpu.quality = handle;
        }
        if (prepared && prepared->qualityOnly) { return; }
        auto vertices = prepared ? std::vector<Vertex>{} : meshGeometry(surface.source).vertices;
        if (!prepared) { generateSmoothNormals(vertices, meshGeometry(surface.source).indices); }
        gpu.vertices = woby::graphics::createVertexBuffer(woby::graphics::copy(prepared ? meshGeometry(surface.source).vertices.data() : vertices.data(), vertexBytes), meshVertexLayout());
        const auto& indices = meshGeometry(surface.source).indices;
        gpu.triangles = woby::graphics::createIndexBuffer(woby::graphics::copy(indices.data(), indexBytes), WOBY_GPU_BUFFER_INDEX32);
        if (!woby::graphics::isValid(gpu.vertices) || !woby::graphics::isValid(gpu.triangles)) {
            throw std::runtime_error("Cannot allocate analysis surface buffers.");
        }
    }
    if ((stages & comparisonDistance) && !surface.distanceFaces.empty()) {
        const auto& samples = surface.distanceFaces;
        if (samples.size() != meshGeometry(surface.source).indices.size()/3) {
            throw std::runtime_error("Distance display does not match its source triangles.");
        }
        gpu.samples = woby::graphics::createVertexBuffer(woby::graphics::copy(samples.data(), comparisonBufferBytes(samples.size(), sizeof(DistanceFace))), {sizeof(DistanceFace)});
        if (!woby::graphics::isValid(gpu.samples)) { throw std::runtime_error("Cannot allocate analysis distance buffer."); }
    }
    if (stages & comparisonTopology) {
        gpu.boundaries = uploadEdges(surface.topology.sources.empty() ? surface.diagnostics.boundaryEdges : surface.topologyBoundaries);
        gpu.nonManifold = uploadEdges(surface.topology.sources.empty() ? surface.diagnostics.nonManifoldEdges : surface.topologyNonManifold);
        gpu.nonManifoldVertices = uploadEdges(surface.nonManifoldVertexMarkers);
        gpu.holes = uploadEdges(surface.holeEdges);
        gpu.finEdges = uploadEdges(surface.finEdges);
        gpu.finFill = uploadPositions(surface.finFill);
        gpu.winding = uploadEdges(surface.topology.sources.empty() ? surface.diagnostics.inconsistentWindingEdges : surface.topologyWinding);
    }
    if (stages & comparisonDegenerates) {
        gpu.degenerateEdges = uploadPositions(surface.degenerateDisplayEdges);
        gpu.degenerateFill = uploadPositions(surface.degenerateDisplayFill);
    }
    if (stages & comparisonIntersections) {
        gpu.intersectionEdges = uploadEdges(surface.intersectionEdges);
        gpu.intersectionFill = uploadPositions(surface.intersectionDisplayFill);
    }
    if (stages & comparisonDuplicatePoints) { gpu.duplicatePoints = uploadPositions(surface.duplicatePointDisplay); }
    if (stages & comparisonDuplicateTriangles) {
        gpu.duplicateTriangleEdges = uploadPositions(surface.duplicateTriangleDisplayEdges);
        gpu.duplicateTriangleFill = uploadPositions(surface.duplicateTriangleDisplayFill);
    }
}
void destroyStage(ComparisonGpuSurface& gpu, uint32_t stages)
{
    const auto destroy = [](auto& handle) { if (woby::graphics::isValid(handle)) { woby::graphics::destroy(handle); } handle = WOBY_GPU_INVALID_HANDLE; };
    if (stages & comparisonSource) { destroy(gpu.vertices); destroy(gpu.triangles); destroy(gpu.quality); }
    if (stages & comparisonTopology) { for (auto* h : {&gpu.boundaries,&gpu.nonManifold,&gpu.winding,&gpu.nonManifoldVertices,&gpu.holes,&gpu.finEdges,&gpu.finFill}) { destroy(*h); } }
    if (stages & comparisonDuplicatePoints) { destroy(gpu.duplicatePoints); }
    if (stages & comparisonDuplicateTriangles) { destroy(gpu.duplicateTriangleEdges); destroy(gpu.duplicateTriangleFill); }
    if (stages & comparisonDegenerates) { destroy(gpu.degenerateEdges); destroy(gpu.degenerateFill); }
    if (stages & comparisonIntersections) { destroy(gpu.intersectionEdges); destroy(gpu.intersectionFill); }
    if (stages & comparisonDistance) { destroy(gpu.samples); }
    if (stages & comparisonQuality) { destroy(gpu.quality); }
}

} // namespace woby
